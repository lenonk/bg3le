# bg3le against the real extender

`reference/*.txt` is output captured from the Script Extender on Windows.
`tools/check-reference.sh` runs the same 25 queries against a running bg3le
and reports how far apart the answers are. This is where that stood on
2026-09-24.

    25 queries: 12 identical, 2 differ only in order, 10 differ in content

Addresses are normalised before comparing — upstream's pointers are Windows
addresses and ours are this process's — and a difference that disappears when
both sides are sorted is reported as an ordering difference, because a dump's
key order is whatever `pairs()` gave and Lua does not define it.

## What the harness found

Three queries failed outright, and two of the three were real bugs:

- **`Ext.Entity.GetAllEntitiesWithComponent` returned handles**, where
  upstream returns entity objects. A caller writing
  `for _, e in ipairs(...) do if e.DisplayName` got "attempt to index a number
  value". `GetAllEntities` and `GetAllEntitiesWithUuid` had it too.
- **`entity:GetAllComponents()` did not exist.** It does now: bg3le asks the
  question the other way round from upstream — for each component it has
  metadata for, does this entity carry it — which reaches the same set, since
  a component bg3le cannot describe could not be returned either way.
- **`AttributesUnavailable` appeared in every stat dump** as a null. It is
  bg3le's own diagnostic, not one of upstream's keys, and a mod iterating a
  stat would have seen it. It is listed as a diagnostic now and appears only
  when it has something to report.

After those, `entity-component-health` is byte-identical to the real
extender's capture, and `stats-weapon` and `stats-base-weapon` match in
content with only their key order differing.

## What still differs, and why

**The install is not the same one.** Eight of the ten are this and nothing
else: `stats-count` (24k stats against upstream's), `mod-loadorder` (a
different mod set), `entity-host` and `entity-component-list` (a different
save), `staticdata-actionresource` (125 action resources against 87),
`entity-component-types` and `types-count` (a newer bg3se's metadata),
`enums-list` (296 enums against 295 — bg3le has `SurfaceTransformActionType`
and the capture does not).

**Two are real representation differences**, both in the functor and
expression area that `reference/STAT-WRITES.md` already describes as partial:

- A dice expression reads as its source text where upstream decodes it into
  fields. Upstream's `BURNING` has `AmountOfDices: 1`, `DiceValue: "D4"`,
  `DiceAdditionalValue: 0`, `DiceNegative: false`; bg3le has `Code: "1d4"`.
- Upstream's dumper prints `*RECURSION*` where a nested functor refers back to
  its parent. bg3le's objects are built fresh per read rather than being the
  same proxy, so the cycle is not there to detect and the structure expands.

**One is cosmetic.** Upstream's static data entries carry a `Get` method that
shows up in a dump; bg3le's do not.

## Using it

    ./tools/check-reference.sh          # needs the game running with bg3le

The queries are parsed out of `tools/grab-reference.sh` rather than restated,
so the capture script and the comparison cannot drift. bg3le's answers land in
`reference/bg3le/` for diffing.

It deliberately does not decide pass or fail. Most of the remaining
differences are the install, and a harness that called those failures would be
ignored within a week. The number worth watching is "differ in content" on the
queries that do not depend on the save or the mod set.

## `StatsExpressionPooled.Params` — measured, 2026-09-24

The first actual reading of it, so the next attempt starts from numbers
rather than from the shape of the declaration.

`Ext.Stats.Get("Target_MainHandAttack").SpellProperties[1].Functors[1]` is a
`DealDamage` whose `Damage` is a pooled expression. bg3le reads its `Code`
and `RefCount` correctly — `"Placeholder0"` and a live refcount, matching the
captured reference — and `Params` as:

    Params: one entry, the number 121

against upstream's

    "Params": ["Placeholder", 0]

Two things are wrong, and they are separate.

**The count.** `Array<Param>` is a pointer, a capacity and a size, and the
size is a `uint32` read through the container's own accessor — it does not
depend on `sizeof(Param)` at all. bg3le reads 1 where upstream reports 2. So
either the array being read is not the one upstream reads, or the field
offset of `Params` within `StatsExpressionInternal` is wrong here and the
size being read belongs to something else. The offset is the thing to check
first, and it is checkable: the buffer pointer next to it has to be a
readable allocation.

**The element.** 121 is `0x79`. `Param` is a nine-alternative variant, so its
discriminant is one byte in both libc++ and libstdc++, sitting after the
union — and the union's size is what differs between bg3le's build and the
engine's. Decoding as a number at all means the active index bg3le read
points at one of the integer alternatives (`int32_t`, or one of the four
enums) rather than at `Variant2`, which is where the string `"Placeholder"`
lives. 121 is not a byte of `"Placeholder"`, so it is not simply the string
read as an integer.

### The bytes

`Ext._Internal.ObjectFieldAddress` was added for this — the object
counterpart of `FieldAddress`, which only took an entity handle. With it:

    expression at 0x5b58f547b70  class StatsExpressionPooled  path "Params"
    Params field at 0x5b58f547b70, 16 bytes
    buffer 0x5b5440b8340  capacity 2  size 2

      +  0  07 3f 0b 44 b5 05 00 00  73 63 72 69 70 74 69 6f
      + 16  6e 22 20 22 68 35 38 66  00 39 33 39 39 67 35 36
      + 32  00 00 00 00 38 62 30 67  38 62 64 62 67 36 31 36
      + 48  35 31 35 36 33 62 36 39  07 22 00 31 22 00 00 00
      + 64  00 3c 0b 44 b5 05 00 00  73 69 74 69 6f 6e 45 66
      + 80  66 65 63 74 22 20 22 32  00 39 61 65 32 64 35 2d
      + 96  03 05 00 00 32 00 00 00  00 00 00 00 00 00 00 00
      +112  00 00 00 00 00 00 00 00  04 00 00 00 00 00 00 00

Three things fall out, and the third is the one that matters.

**The count is 2 after all.** The size field reads 2, not 1. The `#Params == 1`
that Lua reported is an artefact of the reader: `read_object_path` returns
nil for a valueless variant and assigns it into the array, so a nil second
element leaves a hole and `#` stops at one. The count was never wrong.

**`Params` is at offset 0**, as the declaration says, and that is
corroborated rather than assumed: `Code` is declared at +16 and reads back
`"Placeholder0"` correctly, so the two members either side of that boundary
both agree.

**But the buffer does not hold what upstream reports.** Upstream's first
param is the string `"Placeholder"`, eleven characters, which is short enough
to live inline in an `STDString` — so somewhere in these bytes there should
be `50 6c 61 63 65 68 6f 6c 64 65 72` with a length of `0b` at the end of its
sixteen. There is no `50 6c 61 63` anywhere in the dump. What is there is
fragments of unrelated text — `"scriptio"`, `"n" "h58f"`, `"sitionEffect"
"2"`, `"9ae2d5-"` — which is a string pool, and two eight-byte values that
look like pointers into that same allocation (`0x5b5440b3f07` at +0 and
`0x5b5440b3c00` at +64).

### The object, which settles the header

Dumping the pooled expression itself rather than its buffer, with
`"Placeholder0"` as a known anchor — twelve characters, so inline, with `0c`
in the sixteenth byte:

      +  0  40 83 0b 44 b5 05 00 00  02 00 00 00 02 00 00 00  |@..D............|
      + 16  50 6c 61 63 65 68 6f 6c  64 65 72 30 00 55 00 0c  |Placeholder0.U..|
      + 32  13 06 00 00 4f 4e 45 5f  41 55 52 41 22 00 00 00  |....ONE_AURA"...|
      + 48  80 a4 0b 44 b5 05 00 00  02 00 00 00 02 00 00 00  |...D............|

Every declared offset is confirmed, and none of it needed guessing:

- `+0` `Array<Param>` = buffer `0x5b5440b8340`, capacity 2, size 2 — the
  pointer, capacity and size in the order bg3se declares them
- `+16` `Code`, inline, `0c` long: `"Placeholder0"`
- `+32` `RefCount` = `0x613` = 1555, which is what bg3le reports
- `+48` the next pooled expression's own header, so the object is 40 bytes
  and the pool packs them at 48

So the earlier reading was wrong on both counts: the header is right, the
buffer pointer is right, and the pooled expression bg3le resolved is the
right one. `"ONE_AURA"` at +36 is the next object's `Code`, not a stray.

### Solved: the engine's `Param` is 32 bytes with the discriminant at +24

`"Placeholder0"` was a bad subject — its parameters are a word and a zero,
identifiable in nothing. `Target_HeartStopper`'s `SpellSuccess` damage has a
52-character code, `"Placeholder0+max(DexterityModifier,StrengthModifier)"`,
and fifteen parameters. Its object reads:

      +  0  00 22 58 8f b5 05 00 00  0f 00 00 00 0f 00 00 00
      + 16  00 ea 0b 44 b5 05 00 00  34 00 00 00 38 00 00 80
      + 32  0e 00 00 00 ...

`Params` holds 15, and `Code` is a *heap* string this time: a pointer, size
`0x34` = 52 — the code's exact length — and a capacity with the top bit set,
which is the heap flag. Two independent confirmations of the same offsets.

The buffer is where the answer was. It looks like garbage — fragments of stat
file text, `"MINDWEAVER_FR"`, `"data \"DisplayName\""`, `"ealDamage(5d8,Cold)"`
— because the pool handed out memory that had held a stats file and a
`Param` only writes the bytes it uses. What is regular is *where* the writes
land:

    small values at +0, +24, +32, +56, +64, +88, +96, +120, +128, +152, ...

which is +0 and +24 of every 32 bytes. So **`sizeof(Param)` is 32, the
payload is at +0 and the discriminant is one byte at +24.** Reading it that
way gives, for the fifteen elements, discriminants
`0 0 7 0 7 0 2 1 3 6 …` — every one inside a nine-alternative variant's
range.

And it decodes `"Placeholder0"` to upstream's answer exactly:

    element 0   payload 7   discriminant 0 -> StatsExpressionType[7]
    element 1   payload 0   discriminant 7 -> int32_t

`StatsExpressionType` label 7 is `"Placeholder"`, so that reads
`["Placeholder", 0]`, which is what the captured reference says. The
parameters are the code's tokens in prefix order, which is also why the
52-character code produces `Add, Placeholder, 0, Max, 2, Variable, …`.

### Fixed

`EngineLayout<T>` in `src/vendor/component_meta.cpp` is the override, and
`Param` is its one entry: size 32, discriminant at +24. Everywhere else the
layout still comes from the type itself, which is right because the
declaration and the engine agree there.

    Code    Placeholder0
    Params  ["Placeholder", 0]

which is what the captured reference says, byte for byte. The 52-character
expression reads as a token stream in prefix order:

    Add, Placeholder, 0, Max, 2, Variable, AbilityOverride, …

Thirteen of its fifteen parameters decode; two read nil, and both are nested
`Variant2`s whose own discriminant is still read through `index()` on
bg3le's compiled type. That is the same class of problem one level down and
the same fix if it turns out to need one — but `Variant2` measures 24 in both
bg3le and the engine, so it may be something else, and it is not worth
guessing at before measuring.

### Why the compiled size disagrees, and it is not a mystery

bg3le compiles `sizeof(Param)` as **40**, not 32. Every alternative is small
enough for 32 — `RollDefinition` 12, `ResourceRollDefinition` 24,
`Variant2` 24, `StatusGroup` 8, the enums 1, `int32_t` 4, `bool` 1 — and
`Variant2` is 24 here, matching the engine. But even
`std::variant<StatsExpressionType, Variant2>` measures 40 under this libc++,
so the eight extra bytes come from the standard library's own nesting rather
than from anything bg3se declared. `StatsExpressionPooled` itself is 40 and
matches the engine, so this is the one type in the chain where the compiler
and the engine disagree.

Which means the fix does not depend on winning that argument. The engine's
layout is measured and confirmed twice over, so the field metadata should
describe the engine's `Param` — stride 32, discriminant at +24 — rather than
what this compiler happens to produce, the same principle that gave
`STDString` this build's sixteen-byte layout instead of `std::string`'s.
`variant_index_thunk` reads through `index()` on bg3le's own type and cannot
be used for this one; it needs the measured offsets.
