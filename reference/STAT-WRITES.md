# Writing stats

`stat.Conditions = "..."` and `stat:SetRawAttribute(name, value)` are how
a mod changes the game's own stats at runtime. 5eSpells does it a few
hundred times in one `StatsLoaded` handler, and until this landed the
handler stopped at its first write.

## What an attribute is

One `int32` in the stat object's indexed properties, at the index its
modifier occupies in the object's modifier list. What the int32 *means*
depends on the modifier's kind:

| kind | meaning of the int32 | writable |
| --- | --- | --- |
| Int | the value | yes |
| Enumeration | the value; labels map to it | yes |
| Conditions | index into `RPGStats::Conditions` | yes |
| FixedString | index into `RPGStats::FixedStrings`, which holds string-table ids | see below |
| Float, GUID, Flags | index into the matching pool | no |
| StatsFunctors, RollConditions, Requirements | the engine holds these compiled | no |

So the write itself is a single store. What takes care is getting a value
into a pool first, because a value a mod builds at runtime is not in one.

## Adding to a pool

Growing the array is not an option: the engine owns it and frees it with
its own allocator, so replacing the buffer with one of ours hands it a
pointer to free that it did not allocate. What is available is the slack
past the end — a Larian array carries a capacity as well as a size, and
this build has plenty: the condition pool holds 5,620 of 8,192, the string
pool 30,989 of 40,000.

An entry goes into the spare capacity and **the size is raised to include
it**. The first version did not raise it, reasoning that an entry the
engine does not count is an entry it will never destruct. That was wrong
in the way that matters: an index past the size is out of range to every
reader, this file's included, so the attribute read back empty. Writing a
condition therefore *cleared* it — and an interrupt with no condition is
an interrupt that always fires. That is almost certainly what made the
engine grind during the first attempt at this.

Verified: after 5eSpells' pass, `Interrupt_AttackOfOpportunity.Conditions`
reads `"not S5E_IsInvisibleSeen() and IsAbleToReact(context.Observer)
and ..."` — the mod's prefix in front of the engine's own text.

## FixedString attributes

A string attribute needs a string-table id, and text a mod builds at
runtime has none. `ls::FixedString` is a 32-bit index: sub-table in the
low four bits, bucket in the next sixteen, entry above that. The
sub-tables are length classes -- entry sizes 48 through 2080, each holding
a 24-byte header and the text.

Free entries are kept on a list threaded through the headers'
`NextFreeIndex`, and **`field_1180` is its head**. That is not a guess:
every sub-table's `field_1180` has its own index in the low four bits --
0 through 10, all eleven of them -- which is the bottom of a FixedString
id, and the rest decodes the way ids do. For sub-table 6 the head reads
bucket 172 of 173 and entry 25 of 292, both in range, which they would
not be if the reading were wrong.

So an entry is taken the way the engine takes one: pop the head, put what
it pointed at in its place, with a compare-and-swap -- that 32-bit counter
above the id is the signature of a tagged lock-free stack, and the engine
interns on other threads throughout a level load.

Two wrong versions came first, and both are worth recording:

- **Taking the entry at `field_1100`.** That field is a count of live
  entries, not a high-water mark, so the entry sat in the middle of the
  allocated region and was *on* the free list. Writing over it cut the
  chain. The engine then handed the same entry out again -- a stat's
  `Boosts` read back as a Gustav animation path -- and spent the rest of
  the level load at 250% CPU, which is what walking a severed free list
  looks like from outside.
- **Popping with a plain load and store.** Correct with the game idle,
  and it lost the race every time during a load.

Nothing is added to the hash map the engine interns through, which is a
deliberate limitation: the map's layout is not established here, and the
only consequence is that the engine interning the same text later makes
its own second entry -- which is what the table looks like anyway when a
string arrives twice before either copy is released. The refcount is set
high enough never to reach zero, so an entry never returns to the list
and the id a mod holds stays its own.

## Reading, which is what actually made this hard

With the writes correct, 5eSpells' `StatsLoaded` handler still never
finished. It was not the writes: the story thread was inside the mod's own
code, and `BG3LE_COUNT_READS=1` said why -- **two hundred and twenty
million fault-tolerant reads, a million a second, still climbing**. Every
read here is a `process_vm_readv`, which is the price of turning a bad
pointer into `EFAULT` instead of a crash, and something was doing it in a
loop over whole engine structures.

`perf` named it: **73% of the extender's CPU in `bg3le_fixed_string`**,
which resolves an id to text and had a cache. The cache only kept
successes. An *unset* FixedString field does not resolve, so it missed
every time, and each miss walked the sub-table, the bucket array and the
header -- three system calls, on every read of every empty field of every
object. Keeping failures too is a four-line change and it is the whole
difference between a handler that never finishes and one that takes five
seconds. An id that does not resolve is not going to start resolving.

Four more lookups were linear where they had to be indexed, each found by
the same method -- look at what the numbers say, not at what the code
looks like:

- `Ext.Stats.Get` scanned all 27,821 objects comparing names, two system
  calls an element, on every call. The comment above `Ext.Stats.GetStats`
  had already worked out that this makes a mod's pass quadratic; the
  lookup is a hash map now.
- `Ext.Stats.GetStats(list)` walked every object asking for its name and
  its modifier list, about six calls an element and a quarter of a second
  a call, in a loop. Names are indexed by modifier list, which also means
  the filter argument works rather than being refused.
- `Ext.StaticData.Get` looked a GUID up by scanning a bank's key array
  with a read per key. The comment said "banks hold hundreds of entries
  and this runs once per lookup"; both halves were wrong. Keys are read
  once per bank and indexed.
- Reading a stat read its modifier metadata, its list's modifiers and its
  indexed properties per attribute rather than once, and the text behind
  a pool index every time rather than keeping it.

And `Ext.Stats.Get` no longer snapshots every attribute: it returns a
proxy that reads on access, which is what upstream's object does. The
snapshot decoded two hundred values and built two hundred table entries
whether or not the caller wanted one, and because a mod keeps what it
fetches the collector's share of each fetch grew with the heap -- 8ms per
stat at two thousand, 40ms at six, 120ms at ten. Quadratic, and it read as
a hang.

An array whose reported element count is implausible is now treated as
empty, with the class and path named once, rather than walked.

## Where it ends up

5eSpells' whole `StatsLoaded` handler runs, in about five seconds. It
rewrites forty interrupt conditions, appends to `PotentSpellcasting.Boosts`
and interns three new strings on the way, and every one of them reads back
through a fresh `Ext.Stats.Get`. The level load spends 5.5s in bg3le, most
of it that handler. Sixty million reads for the whole load, against two
hundred and twenty million for a load that never finished.

## What is not attempted

`SetRawAttribute` on a functor attribute — `SpellProperties`,
`StatsFunctors` — needs the engine's own parser: those are held compiled,
not as text, and upstream delegates to `Object::SetRawAttribute`, which
has no symbol here. Storing an index to text the engine never compiles
would look like it worked and do nothing.

`stat:Sync()` and `Ext.Stats.Sync` rebuild the prototype, as upstream's
`RPGStats::SyncWithPrototypeManager` does: `src/vendor/stat_sync.cpp`
resets the fields bg3se's `SyncStat` resets and calls the engine's own
`SpellPrototype::Init`, `StatusPrototype::Init` (then the status loader's
boost parse) or `InterruptPrototype::Init`. See "Finding the Init
functions" below. A passive has no Init: its loader builds each passive
inline, skipping any already in the manager's map and returning at once
if the manager's `Initialized` byte (+0x18) is set. Sync takes the node out
of its bucket, clears the byte and calls the loader, which builds just that
one; the fresh prototype is then copied into the old node and the old node
relinked, so its address holds. `SetPersistence`, and Sync's `persist` argument, are deprecated
upstream and warn once there; they do here.

## Copying a stat

`stat:CopyFrom(name)` works. Upstream's `Object::CopyFrom` refuses across
modifier lists and then assigns `AIFlags` and every `IndexedProperties`
entry, and those properties are the whole of a stat's scalar surface, so this
copies the same array element by element rather than approximating it.
Measured between two spells: 204 of 204 properties carried, and afterwards
198 of 199 scalar attributes agree with the source -- the exception being
`Name`, which is a separate member and must not be copied.

`Object::Functors` and `Object::RollConditions` are not carried. Those are
two hash maps of compiled objects upstream copies after the property loop,
and writing them needs a HashMap writer bg3le does not have. The log says so
once per stat rather than leaving it implied.

## The prototypes are writable directly

The rebuild has no symbol, but the thing it would rebuild is reachable.
`Ext.Stats.GetCachedSpell(name)` and its three siblings resolve the engine's
*compiled* form of a stat, and since a reflected object writes through, a
field of one can be assigned:

    local p = Ext.Stats.GetCachedSpell("Projectile_MagicMissile")
    p.Level = 4        -- reads back as 4 through a fresh GetCachedSpell

Verified on a live prototype: 57 fields, `Level` written and read back and
put back. So an attribute the engine has already compiled can be changed
after all — not by syncing the stat, but by writing the prototype the stat
was compiled into.

For a passive, Sync goes through its loader instead; see above.

## Finding the Init functions

None has a symbol, but the executable kept its relocations (`.rela.text`,
`.rela.data.rel.ro`, ...), so every reference in it is on record.
`tools/relocs-xref.py` reads them.

- An attribute name the loader reads ("AlternativeCastTextEvents") is a
  string view in a sorted table in `.data.rel.ro`; a per-name static
  initializer builds an interned name from it, and the startup initializer
  stores a pointer to that name's id in a global. `relocs-xref.py table`
  follows that chain for a whole table and counts which functions read the
  globals: for the spell table, `0x5e35980` reads 19 of its names and has
  exactly one caller, the spell loader, which zeroes a fresh 0x338-byte
  prototype, calls it with `(proto, &name)`, inserts into the map and
  appends to the name list at `+0x88` -- upstream's Windows pattern, move
  for move.
- The status loader (`0x3030350`) allocates 0x110-byte prototypes and calls
  `0x27e4d90(proto, &name, flags)` fourteen times with hardcoded statuses
  (upstream's INSURFACE pattern), then once per stat. Its second pass
  parses Boosts: an empty `+0xc0`, the text, and a type-erased callback
  (implementation pointer, then invoke, copy and manage, a scratch buffer
  and `&Boosts`) handed to `0x30317a0`.
- The loaders run in sequence from one function; `0x2fb8e20` is the
  interrupt loader, and its loop calls `0x2fc33d0(proto, stats object)`
  with a 0x1f0-byte stride.
- The passive loader, `0x2fc1b00(manager)`, reads the passive table's names
  itself. It walks every stats object of the passive list, looks its name
  up in the manager's chained map (bucket `key % HashSize`), and for a miss
  allocates a 0x220-byte node (next, key, 0x210-byte prototype), links it
  at the bucket head and fills it inline. It ends by setting `+0x18`.

Every one is pinned by offset and checked before use: its opening bytes,
and where it loads the global at `0x7bbd418`, that the object there holds
the RPGStats bg3le found at `+0xc8`. A different build fails the check and
Sync reports it rather than calling anything.

Syncing an unchanged stat leaves its prototype as the loader built it: 400
of 400 spells and 399 of 400 statuses compare equal after a sync (BLUR's
boost gets a fresh GUID from the reparse). 21 of 400 interrupts come out
different, and rightly -- their Conditions now carry the edits 5eSpells
made to their stats and asked to be synced.
