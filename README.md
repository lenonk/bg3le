# bg3le

A script extender for the **native Linux build** of Baldur's Gate 3.

The existing Script Extender targets the Windows build, so Linux players run
the game under Proton to get it. bg3le attaches to `bin/bg3` directly.

## Credit

This project stands on [Norbyte's Baldur's Gate 3 Script
Extender](https://github.com/Norbyte/bg3se). The game-structure definitions and
the Lua binding framework under `vendor/bg3se/` are theirs (MIT + Commons
Clause); bg3le reuses them rather than rediscovering years of reverse
engineering, and would not be a realistic project otherwise. **Thank you.**

See [vendor/NOTICE.md](vendor/NOTICE.md) for attribution and every change made
to the vendored code — all of it to compile under clang rather than MSVC, none
of it behavioural. Its component definitions have been checked against the
native build rather than assumed: `Ext._Internal.SizeAudit()` compares every
component's declared size with the size the engine recorded, and
`tools/meta-check.c` checks field offsets without needing the game.

## What works

- Osiris is live: `Osi.*` and the bare-global helpers, callable from an
  interactive prompt while the game runs. Out-parameter counts and parameter
  types come from Osiris' own function database rather than from the
  caller's argument count, and the answer is cached under the story version
  — walking it costs 130,000 reads on the story thread otherwise. A name
  with several arities answers to each: Osiris declares `ApplyStatus` with
  three, four and five parameters, the engine's mapping holds only the five,
  and the other two are the story's own, so a count the mapping does not
  have goes the way story functions go
- Lua host with `Ext.Log`, `Ext.Json`, `Ext.Math` (scalar), `Ext.Table`,
  `Ext.Timer`, `Ext.Utils`, and `_D`/`_P`/`_PW`/`_PE`. The interpreter is
  Norbyte's Lua fork, the same one bg3se uses — see
  [external/lua/README.bg3le](external/lua/README.bg3le) for why that is not
  optional
- **`Ext.Events` and `Ext.ModEvents` are upstream's own library**, ported
  from bg3se's `LuaScripts/Libs/Events`: handlers run by `Priority`,
  `e:StopPropagation()` ends a throw, a subscription id carries its event in
  the high 32 bits, and subscribing or unsubscribing during a throw takes
  effect after it. Each context has upstream's event set, and any other name
  is a missing event that says so when subscribed to. Every handler —
  events, net channels and listeners, console commands, timers, Osiris
  listeners, IMGUI callbacks — runs under `xpcall` with `debug.traceback`
  and fails with upstream's message for that kind of handler; a timer
  callback gets its handle, as upstream's does
- **Installed mods run.** Mods ship their Lua inside a `.pak`, so bg3le reads
  the archives in the profile's `Mods` directory and the install's
  `Data/Mods`, finds each module by its `ScriptExtender/Config.json`, and
  loads its bootstrap, `Ext.Require` targets and plain `require()` calls
  straight out of the archive. Each mod gets `Mods[ModTable]` as its
  environment with the real globals behind it, and `ModuleUUID` set before
  its table enters `Mods` — Mod Configuration Menu watches that assignment,
  so filling it in afterwards makes every mod look anonymous. Loose
  directories still work, via `BG3LE_MOD_PATH`. Of one 57-mod set, all five
  script mods load and run, MCM included (v1.40.1, "SE version 32")
- **Achievements with mods active**, the way bg3se's `EnableAchievements`
  does on Windows. bg3se patches `ls::ModuleSettings::IsModded`; nothing here
  exports that name, so bg3le byte-patches the engine's per-module "is this
  module official" predicate to return true, at startup and before the
  game's `fork()`, which opens every consumer of the mod check at once
  (the Osiris `UnlockAchievement` native's two checks and the cached
  "modded" badges on the Load Game list). Off with `BG3LE_ACHIEVEMENTS=0`
  or `"EnableAchievements": false` in `ScriptExtenderSettings.json`.
  Contributed by Igor Tarasyuk; the investigation and the addresses a
  maintainer needs when the binary updates are in
  [reference/ACHIEVEMENTS-DIAGNOSIS.md](reference/ACHIEVEMENTS-DIAGNOSIS.md)
- **The story's own procedures and databases are callable.** All 19,078 of
  them — `Osi.PROC_*`, `Osi.DB_*` and the story's events, which carry no
  dispatch handle and so cannot go through the DIV boundary at all. They run
  the way the engine runs them: a tuple is inserted into the Rete node the
  function stands for. `Osi.DB_Foo(...)` inserts a fact, `Osi.DB_Foo:Get(...)`
  reads them back with nil as a wildcard, `Osi.DB_Foo:Delete(...)` retracts
  with nil as a wildcard there too, and a bare `PROC_Foo(...)` works as it
  does upstream. Osiris interns its strings, so an argument is interned
  through `COsiStringTable::AddStr` and released afterwards. Nothing here is
  bg3se's offsets: `InsertTuple` is at `+0x68` on this build, not `+0x50`,
  `DeleteTuple` at `+0x70` and not `+0x78`, and the structures were read out
  of the engine's own disassembly —
  see [reference/OSIRIS-STORY-CALLS.md](reference/OSIRIS-STORY-CALLS.md).
  Resolved on first mention rather than at load, as upstream resolves its
  own, so the level load still costs 0.08s
- **`Ext.Osiris.RegisterListener` fires.** `before`, `after`,
  `beforeDelete` and `afterDelete` on any story function, which is how a mod
  watches the game rather than polling it. The two tuple slots are replaced
  in the two node classes that use them — bg3se's `NodeHooks.cpp` does the
  same — and only once a mod subscribes, so until then every node keeps the
  engine's own pointers. Engine-side activity reaches it too: a listener on a
  database sees the fact a procedure's own rule inserts. The engine's own
  calls have no node, so they are seen where upstream sees them, at the DIV
  call handler: `before` and `after` on `SetCanGossip` fire for a call made
  from Lua, and on `TimerLaunch` for the story's own. User queries (`QRY_*`)
  are not callable yet — upstream evaluates them through the Rete node's
  `IsValid` with an identity adapter
- **`Ext.Enums`**, every enum and bitfield bg3se describes, reachable by label
  or by numeric value, under the Lua name the generated metadata gives it —
  `Ext.Enums.ClientGameState.Menu`, not `ecl::GameState`. The entries are the
  labels rather than upstream's `EnumValue` objects, deliberately: bg3le reads
  an enum-typed field as its label, which is what `reference/` verifies
  against the real extender, and a comparison is what a mod does with these.
  Two strings compare equal; a proxy against a string never would, since
  Lua's `__eq` does not fire across types
- **`Ext.Net` crosses between the two contexts.** Upstream's messages ride
  the game's connection as protobuf because on Windows the two sides may be
  two machines; single-player is one process either way, and bg3le has both
  Lua states in it, so a message is queued in the other state and drained on
  its next tick — which is when a real one would have arrived.
  `BroadcastMessage`, `PostMessageToClient`, `PostMessageToUser`,
  `PostMessageToServer` and a `NetChannel`'s `Send`/`Request` all reach the
  other side, and a request's reply comes back to the caller's callback
- **`Ext.Debug.GenerateIdeHelpers`** writes the LuaLS annotations upstream
  writes, to the path upstream writes them to: 20,361 `Osi.*` stubs with
  `@param` and `@return` from the story's own signatures, plus the bare global
  for each of the 1,302 engine functions. The database names far more than the
  bound list does — a procedure and a user query have no dispatch handle and
  are exactly what a mod author wants annotations for — so it is generated
  from the database and the bound list together
- **A client Lua context as well as the server's.** The game is two contexts
  in one process and upstream runs a Lua state for each, so bg3le does too:
  each has its own `Ext`, its own `Mods` table, and runs the bootstrap that
  belongs to it. Mod Configuration Menu loads on both sides and prints its
  `[S]` and `[C]` banners; `Ext.IsClient()`/`Ext.IsServer()` answer for the
  state they are asked in. The console switches with `:client` / `:server` —
  the LuaDebug protocol has carried a context on every request all along.
  Osiris is server-side, as upstream has it, and says so in the client
  context rather than blaming the save. `Ext.Loca.UpdateTranslatedString`
  writes into the index `Ext.Loca` reads — which is bg3le's own, built from
  the game's `.loca` files, since `ls::TranslatedStringRepository` has no
  symbol and did not survive being fingerprinted — so a handle a mod sets
  reads back as it set it. That is what MCM registers every interface label
  through, and refusing it stopped its client script at line five. The
  engine's own repository is still not written, so the game's own interface
  does not show them; bg3le says so once rather than leaving it to be
  discovered
- **The engine's own managers found once and remembered.** Everything located
  by content — `RPGStats`, the mod load order, the four prototype managers —
  has the path from a static pointer to it recorded under the build id, so
  later runs dereference instead of scanning. The search runs backwards:
  what points at the manager, what points at that, until something in the
  executable's own writable data does. Story-load work went from 30.3s to
  0.07s
- **`Ext.Stats.GetCachedSpell`, `GetCachedStatus`, `GetCachedInterrupt` and
  `GetCachedPassive`**, over 8,707 spells, 7,430 statuses, 416 interrupts
  and 2,498 passives. The last two never resolved before: interrupts are
  held in their map rather than behind a pointer, and passives in a chained
  `LegacyRefMap`, and the scan only admitted maps of pointers. Each manager
  is confirmed by the stat type its names belong to. A cached prototype's
  conditions match the stat's for every passive but one and 395 of 416
  interrupts; the rest are the conditions 5eSpells rewrites, which the stat
  shows and the compiled prototype does not until it is synced
- **An entity-valued field is an entity.** Upstream's push for an
  `EntityHandle` or an `EntityRef` makes an entity proxy, or `nil` for the
  null handle — which is `0xFFC0000000000000`, not all ones. bg3le handed back
  the raw integer, so `comp.Owner:GetComponent(...)` failed where upstream's
  works. Entities also compare equal by handle, order by handle and print as
  `Entity (0200000100000086)`, as upstream's do; two reads of one entity used
  to compare unequal. `esv::Character.MyHandle` coming back equal to the
  character it was read from is the check. Entity and GUID fields are
  writable too, converting as upstream's `get` does: an entity or `nil` for
  an entity, anything else refused; a string that parses for a GUID, with
  upstream's own error for one that does not. An `EntityRef` keeps the world
  the engine paired it with, and gets the server world only when it has none.
  An entity is a userdata, as upstream's are, so `type(entity)` is
  `"userdata"` — and so are its components and their arrays and maps, which
  dump, serialise and iterate as before and answer `nil` past the end, as
  upstream's arrays do — and there is one per handle while anything holds it, so it
  works as a table key and compares raw-equal the way upstream's
  value-compared proxies do. `Ext.Types.GetValueType` answers as upstream's
  does: `"Entity"`, a struct's name (base type `"CppObject"`), or Lua's own
  type name
- **An entity has upstream's methods**: `IsAlive`, `GetAllComponents` and
  `GetAllComponentNames` from the entity's own storage (146 components for
  Lae'zel, in 2 ms), `HasRawComponent`, `GetChangedComponents`,
  `MarkChanged`/`WasChanged`, `Get`/`SetReplicationFlags` and `Replicate`
  (upstream's replication test passes: 0, then 7, then all ones), the
  `OnCreate`/`OnDestroy`/`OnChanged` family, and `entity.Vars` for user
  variables. An unknown key raises upstream's error, and
  `Ext.Entity.GetRegisteredComponentTypes` walks the world's registry. The
  last two reference captures are now identical to the Windows output. What
  is left needs the calling thread's entity command buffer, which upstream
  picks with a function this build has no symbol for — `Create`/
  `RemoveComponent`, `WasAdded`/`WasRemoved` and the current-frame lists —
  and `GetNetId`, which needs the server's replication authority; those
  raise and say so
- **`Ext.Entity.OnCreate`/`OnDestroy` and their variants fire.** As
  upstream's `EntityComponentEventHooks` does, bg3le adds a connection to
  the engine's own per-type construct and destroy signals in the world's
  `ComponentCallbacks`. The engine's function objects match bg3se's
  pointer-table `Function`; the signal's `EntityRef` argument, which bg3se
  writes as a pointer because MSVC passes a 16-byte struct by hidden
  reference, arrives by value in two registers under System V. Events are
  queued and delivered on the next server tick, immediate subscribers
  before deferred ones, because the Lua states may only be entered from
  their own thread — so a destroy handler gets `nil` for the component,
  which no longer exists by then. Applying `BLESS` with Osiris reaches an
  `OnCreate("ServerStatus")` handler with the status readable.
  `OnChange`/`Subscribe` fire too, from the replication pools as upstream
  reads them after the world update — the server tick sees each update's
  changes once — with the changed field flags and upstream's optional flag
  filter; a component with no replication index raises upstream's error.
  Damaging the host reaches an `OnChange("Health")` handler with field 1
- **Map keys arrive as upstream pushes them**: an entity key as the entity
  and an enum key as its label. `StatusContainer.Statuses` is keyed by the
  status entities, and each one can be looked up by the entity or followed
  to its own `ServerStatus`; a spell prototype's `MetaConditions` is keyed
  `Target`, not `5`. Maps inside prototypes, templates and static data are
  read too — they came back as `"<unreadable>"` before
- **`TranslatedString:Get()`**, upstream's way to turn a `DisplayName` into
  text: a character template's reads `"Nadira"`, and a companion's
  `DisplayName.Name:Get()` reads `"Shadowheart"`. It resolves through bg3le's
  index of the game's `.loca` files, which `Ext.Loca.UpdateTranslatedString`
  also writes to. What it cannot resolve yet is a string the engine made at
  runtime — the name a player typed for their own character is
  `ResStr_272917352`, which exists only in the engine's live string
  repository, and finding that repository has no symbol to start from
- **Root templates read as upstream presents them.** Most of a template is
  `OverrideableProperty<T>` — a value and a flag saying whether this
  template overrides the one it inherits — and upstream presents each as a
  plain `T`: `push`, `Serialize` and `MakeObjectRef` all go straight to the
  value. So bg3le does the same, and a character template went from 172 of
  its 181 fields reading `<unsupported>` to 4: `Icon`, `Stats`,
  `DisplayName`, `Race`, `VisualTemplate` and the rest read their real
  values. Assigning one marks it overridden, because upstream's setter
  builds `{value, true}`; `Ext.Types.Unserialize` does not, because
  upstream's writes only the value. Both are checked against the flag byte
  in the engine's memory. An empty `FixedString` reads as `""` rather than
  `nil` everywhere, which is upstream's push, and which put twenty-six
  missing fields back on that template
- **The parameters of a pooled stats expression.**
  `StatsExpressionPooled.Params` reads `["Placeholder", 0]` for
  `"Placeholder0"`, matching the real extender's capture, and a 52-character
  expression reads as its fifteen tokens in prefix order. The elements are
  `std::variant`s, and the standard library does not lay one out the way the
  engine does — this libc++ makes `Param` 40 bytes against the engine's 32,
  and keeps four bytes of a nested variant's discriminant where the engine
  keeps one — so the metadata describes the engine's layout, measured from
  its own memory rather than taken from the declaration. How it was measured
  is in [reference/REFERENCE-DIFFS.md](reference/REFERENCE-DIFFS.md)
- **`Ext.IMGUI` draws and its callbacks fire**, which is Mod Configuration
  Menu's menu and the only thing in a 57-mod set known to need it. Upstream's
  own widget tree is compiled into `libbg3le.so`, imgui and its Vulkan
  backend with it, and `BG3LE_IMGUI=1` brings up the seven Vulkan hooks by
  interposition rather than by Detours. All thirty `Add*` kinds, the window
  setters, the style and colour accessors and the per-type methods are bound;
  properties read and write through the same field machinery a component
  does; and `OnClick`, `OnChange` and the rest arrive in Lua with their
  arguments, delivered on the tick of the context that registered them
  because a widget fires on a thread that must not touch a Lua state. Fonts
  come out of the game's own archives, since the engine's file reader cannot
  be called by name here. Off by default: the present hook does real Vulkan
  work every frame, and the default is to leave the engine's rendering
  exactly as it was. See
  [reference/IMGUI-ASSESSMENT.md](reference/IMGUI-ASSESSMENT.md)
- **A line on the main menu**, as upstream has: the localisation string is
  patched from the game's own heap the moment it appears, before the
  interface resolves it into its own copy
- `Ext.Entity` against the live ECS: `Ext.Entity.Get(uuid)`, component reads
  and writes, and `entity:Replicate(name)` that reaches the client. The engine
  names every ECS type index in its symbol table, so the component and
  replication registries come straight out of `.symtab` — the Windows extender
  has to recover the same mapping by scanning the image for byte patterns.
  Fields come from bg3se's own generated metadata rather than from accessors
  written per component, so every component it describes is reachable by name;
  see [What is left](#what-is-left) for the kinds that do not convert yet
- `Ext.StaticData.Get`/`GetAll` against the engine's GUID resource manager.
  It has no symbol, so it is found by fingerprint: the manager is one
  `HashMap<StaticDataTypeIndex, GuidResourceBankBase*>`, and a table whose
  keys are all drawn from the 121 static data type indices the symbol table
  already names is that manager rather than a coincidence. Resources are
  writable, which is what a mod that edits spell lists needs: a resource's
  fields write through, a `HashSet<FixedString>` is replaced whole by
  `Ext.Types.Unserialize` or by plain assignment, and both string kinds can
  be assigned. Replacing a set means rebuilding its hash table, and doing
  that through bg3se's own container methods took the game down twice — the
  offsets, the hash rule and the two things not to call are in
  `reference/STATIC-DATA-WRITES.md`. Types are named by upstream's
  `ExtResourceManagerType` label, which for 17 of them is not the class
  name — `ColorDefinition` is `resource::Color` — and the five
  character-creation default-value managers, whose names bg3se writes the
  MSVC way, resolve too
- `Ext.Stats`: 15,754 stats, enumerable and readable by name, through a
  proxy that reads an attribute when it is asked for, as upstream's does.
  Snapshotting all two hundred of them per fetch made a mod's stats pass
  quadratic — the collector's share grew with a heap the mod keeps, 8ms per
  stat at two thousand and 120ms at ten. Everything a mod hits in a loop is
  indexed rather than scanned: stats by name, names by modifier list,
  resource banks by GUID, and modifier metadata, a list's modifiers and an
  object's properties read once rather than per attribute. The one that
  mattered most was smaller than any of them — `bg3le_fixed_string` cached
  only successful lookups, so an *unset* FixedString field cost three
  system calls every time it was read, which came to 73% of the extender's
  CPU and was the difference between a mod's stats pass finishing in five
  seconds and never finishing at all. `BG3LE_COUNT_READS=1` is how that was
  found and how the next one will be. Every attribute kind is decoded —
  ints, floats, strings, GUIDs, enumerations and flag sets;
  `RPGStats` has no symbol and its layout is not ours (our
  `TreasureRarities` sits at 800 where the engine's is at 3648), so nothing
  is read through a member offset: the anchor is seven consecutive
  `FixedString` indices spelling the treasure rarities, and everything past
  it — the stats array, the modifier lists, the value lists, the string,
  int64, guid and float pools, and `Object`'s own field offsets — is located
  by content and validated before use
- `Ext.Types` over the same metadata. `GetAllTypes` lists all 3,071 reflected
  classes, and a component or resource view reports its own type, so
  `GetObjectType`, `TypeOf` and `IsA` answer for `entity.Health` and for a
  nested struct rather than only for a stat. `AddCustomFunction` and
  `AddCustomProperty` work: upstream grafts them onto the type's property map,
  and bg3le keeps them keyed by type name, which every view of that type
  consults where the property map would have answered
- `Ext.Mod`, all five functions, and `GetModManager` returns all four members
  including `Settings`. Its offset is derived rather than searched for — bg3se
  declares `AvailableMods`, a `HashMap`, a spare word and then
  `ModuleSettings`, which comes to 112 past the load order's array header —
  and then confirmed against the running game before it was trusted: the array
  there holds 29 descriptors naming real mods, and 29 authored mods plus the
  14 base modules is the 43 the load order holds.
  The mod manager has no symbol either, so
  the list is found from the one thing every install shares: the base
  module's UUID is the constant `ed539163-…`, which locates a `Module`
  exactly, and the array holding it is the load order. `ModuleInfo` turns
  out to be the Windows struct with Larian's sixteen-byte string in place of
  `std::string` — which is why `sizeof(Module)` upstream does not match the
  240-byte stride in memory — and every field was confirmed against a module
  whose values the reference already records
- **Verified against the real extender.** `reference/` holds output captured
  from a running Windows install over the debugger, and bg3le matches it:
  the same 15,754 stats in the same order, the same attribute values, the
  same proxy object with its bound methods, floats to the digit, and
  `Ext.Mod.GetMod("ed539163-…")` returning a structure equal key for key and
  value for value to `reference/mod-shape.txt`. The
  public API is a compatibility contract — a mod written against bg3se has
  to work here — so it follows the reference rather than convenience.
  `tools/grab-reference.sh` reproduces the capture
- A Lua debugger server compatible with the
  [bg3lua](https://github.com/lenonk/bg3lua) client (`client/` submodule),
  plus `CreateConsole` parity that opens a terminal on startup
- **A 65-98s level load reduced to ~1s.** The native build spends almost all
  of it in `physx::Sn::ConvX` converting PhysX data whose `TempAllocator`
  serialises on one global mutex; `src/fast_alloc.cpp` replaces it with a
  lock-free thread-local pool. See
  [reference/SLOW-LOAD-DIAGNOSIS.md](reference/SLOW-LOAD-DIAGNOSIS.md).

- **A 30fps endgame save brought to 71fps.** BG3's Vulkan backend streams
  per-frame data through the `DEVICE_LOCAL | HOST_VISIBLE` heap. On an
  external GPU that heap is BAR-mapped VRAM across a Thunderbolt hop, where
  CPU writes run at 0.21 GB/s against 14.68 GB/s to host memory, so the main
  thread sat in `memcpy` while the GPU starved at 57%. `src/vulkan_memory.cpp`
  hides `HOST_VISIBLE` from the device-local types so the engine's own
  selection picks host memory: p99 frametime 184ms to 15.8ms, GPU busy to
  99%, and less total CPU. It measures the hardware at startup and does
  nothing on a machine where those writes are fast. The same file also builds
  on its own as `memsteer.so`, so the steering can be pointed at any native
  Vulkan game rather than only at this one — see [MEMSTEER.md](MEMSTEER.md)
- **The engine's thread pinning undone.** It pins each thread to one logical
  CPU, which makes that core the frame gate; the same game under Proton runs
  every thread on `0-15` because Wine ignores the requests, and that build
  never had the problem. `src/affinity.cpp` widens the masks: startup p99
  frametime 133ms to 77.8ms, and no pegged core

## What is left

- **59 of `Ext.*` refuse rather than answer.** Every name bg3se exposes is
  present — `tools/api-coverage.lua` reports 715 of 715 — but the ones
  needing machinery bg3le does not have raise instead of returning a
  plausible wrong answer. `tools/count-refusals.py` derives the number from
  the source, because this one was stale at 86 for a while: 24 of the 59 are
  `Ext.Level`'s physics and pathfinding, 8 each `Ext.Stats`' creation and
  functor execution and `Ext.StaticData`'s bank writes and atlas, 6
  `Ext.Template`'s local and cache managers, and the rest are singles —
  `Entity.Create`/`Destroy`, `Types.Construct`, and `GlobalSwitches`, whose
  object is findable by its own language string and whose declared layout is
  not this build's. The evidence for that is now a string test rather than a
  boolean one — one of Larian's strings is 128 bits with a length that has to
  agree with its own contents, against a boolean's one bit, and no candidate
  in the process has the other declared strings where bg3se puts them. Since
  the nearest of those is only +48 from the anchor, the drift starts within a
  few members of `Language`. An attempt to solve for it is recorded there as
  a negative result: it fits, with four arbitrary breaks, which is what a
  test with that much freedom does.
  [reference/GLOBAL-SWITCHES.md](reference/GLOBAL-SWITCHES.md).
  `reference/ext-api-surface.txt` lists them with their shapes
- **One session per process, unless asked.** `Ext.Debug.Reset()` works —
  both contexts are torn down and built again and every mod reloads, which is
  what a mod author editing a script wants — but it has to be asked for. The
  story-load work still runs once, so loading a second save without
  restarting leaves Osiris bound to the first story's mappings. Doing it
  automatically means telling a new session from the two or three story loads
  that make up one, which needs the game state machine bg3le does not read
  yet, so it waits to be told rather than resetting at the wrong moment
- **Stat `Sync` and `SetPersistence`.** Every attribute kind upstream
  writes is written, the way its `Object::Set*` writes it: integers and
  enumerations in place; conditions, strings, floats, GUIDs, flag sets and
  translated-string handles into the matching `RPGStats` pool; roll
  conditions (a string or a `{Name = expression}` table) and requirements
  into the stat's own containers; `AIFlags` onto the object. A pool with no
  spare capacity moves to a fresh buffer from the engine's own allocator,
  with the old one left in place. Re-assigning every attribute of a
  sample of 105 stats across seven modifier lists to itself changes none
  of 8,310 values, and bg3se's `TestStatAttributes` fails only on a
  hash-order and a stale functor expectation. (Functor lists are not
  written; upstream's own setter for them is commented out.) An earlier
  version skipped a pool slot per write, one more each time; see
  `pool_slot` in `src/vendor/stats.cpp`. `CopyFrom` works — it is
  upstream's own loop over the indexed properties, and it refuses across
  modifier lists exactly as upstream does. `SetPersistence` still raises,
  and `Sync` reports what it cannot do rather than raising, because a mod
  that writes and then syncs would otherwise lose the write it already
  made — and because the thing `Sync` would rebuild is reachable anyway:
  `Ext.Stats.GetCachedSpell` resolves the compiled prototype and its
  fields are writable.
  [reference/STAT-WRITES.md](reference/STAT-WRITES.md) has the layout and
  the three theories that were tested and eliminated
- **The last 3% of the field kinds.** 3,467 of 3,558 fields convert
  (97.4%, from `tools/meta-check.c`; it was 94.0% before `STDString` was
  given this build's sixteen-byte layout): scalars, enums and bitmasks, nested
  structs, fixed and dynamic arrays, hash sets, hash maps, glm vectors,
  `std::optional`, `std::variant`, `FixedString`, `OverrideableProperty`,
  `ecs::EntityRef`, and the wrappers upstream pushes as what they hold —
  `Path` as its string, `NetId` and `UserId` as integers, a component handle
  as an integer or nil, and a `stats::ConditionId` as its condition's text,
  refusing a write with upstream's message. An `std::optional` is written as
  well as read, through the container's own `emplace()` and `reset()`, and a
  `std::variant` is read by the engine's layout rather than this compiler's
  — the game is libc++ ABI 2, see
  [reference/LIBCXX-ABI.md](reference/LIBCXX-ABI.md). Counting every class
  the metadata describes rather than only components, 1,714 of 21,365 fields
  do not convert yet. Of those, 831 are raw pointers — 386 of them in the
  `aspk` effect timelines — which upstream follows to the object they point
  at; the largest named groups are the Lua registry entries (44),
  `CompactSet<FixedString>` (26) and `StatsExpressionRef` (11); and the
  ImGui widgets' 391 delegate fields are handled by `Ext.IMGUI`'s own
  callbacks rather than the field tables.
  Naming an unsupported
  field raises rather than returning nil, so a mod cannot mistake a missing
  conversion for a missing value
- **The client-side modules.** `Ext.ClientUI` in particular is blocked on the
  placeholder Noesis RTTI — the native game ships no Noesis typeinfo at all,
  so `src/vendor/noesis_rtti_linux.cpp` aliases 19 of them to one real
  placeholder type. That is safe only while no Noesis `dynamic_cast` runs. The
  real fix is keeping Noesis types out of the generated property maps
- **Launching.** See [Running](#running)

## Building

Needs clang, libc++ (including the static archives), CMake, SDL2 and the
Vulkan loader. protobuf and abseil are built from source by
`tools/fetch-externals.sh` rather than taken from the distribution, because the
packaged builds are compiled against libstdc++ and export `std::__cxx11`
symbols that cannot link into a libc++ library.

They are also built with one libc++ ABI flag the whole library shares. The
game is built against libc++ ABI version 2 and bg3le against ABI 1, and the
one place that shows is `std::variant`, whose index ABI 2 keeps in one byte
where ABI 1 keeps four — enough to change the size of every struct holding a
small variant inline. `_LIBCPP_ABI_VARIANT_INDEX_TYPE_OPTIMIZATION` gives
this build the game's layout, and it has to reach protobuf and abseil too,
since one passes a variant across its boundary and both alias it. CMake
refuses to configure against externals built without it; re-run
`tools/fetch-externals.sh` if it says so. See
[reference/LIBCXX-ABI.md](reference/LIBCXX-ABI.md).

    tools/fetch-externals.sh    # Noesis, glm, imgui, lua, rapidjson, Vulkan
    cmake -S . -B build && cmake --build build

Optional checks:

    tools/check-vendor-all.sh      # per-file error counts for vendor/bg3se
    tools/check-vendor-patches.py  # confirms the clang fixes are still applied
    tools/check-prelude.sh         # parses the Lua embedded in lua_host.cpp
    tools/check-views.py           # runs the array and map views against stubs
    cc -o /tmp/mc tools/meta-check.c -ldl && /tmp/mc build/libbg3le.so
                                   # field offsets, the container walks, and
                                   # how much of the surface converts

The first four need no game and no built library (`meta-check` needs the
library but not the game). The Lua prelude is a raw string literal, so a syntax
error in it is a runtime failure rather than a build one — hence
`check-prelude.sh`.

**clang is required, not merely supported.** The vendored bg3se sources need
`-fdeclspec`, `-fms-extensions` and `-fdelayed-template-parsing`, none of which
gcc has; CMake fails the configure step with any other compiler.

**libc++ is required too.** The native game is built against it, so
`std::string` is 24 bytes there as here — libstdc++ would give 32 and silently
shift every field after a string in a component. Everything in the library has
to agree on one standard library, so this applies to bg3le's own sources as
well. It is linked statically, for the same reason Lua is vendored: a shim
loaded inside the Steam runtime container cannot rely on host libraries.

## Running

**There is no install or launch story yet.** bg3le is a shared library that
has to be loaded into `bin/bg3` before the engine starts, and arranging that
is an unsolved problem, not a documented step. It needs to work for both Steam
and non-Steam installs, and ideally without the player editing launch options
by hand. Until that exists, running it means knowing how to preload a library
into a process inside the Steam runtime container.

`run-native.sh` is the development harness rather than that story. It runs the
game inside the Steam runtime container by default, and `SNIPER=0` runs it
straight on the host — the native binary needs only `libssl.so.1.1` and
`libcrypto.so.1.1`, which `compat-libs/` supplies. Running outside the
container matters for debugging: inside it, libraries are recorded under
`/run/host`, which does not resolve from outside the namespace, and `perf`
can symbolize nothing.

Mods live in `~/.local/share/Larian Studios/Baldur's Gate 3/Mods` with the
load order in `PlayerProfiles/Public/modsettings.lsx`, and one thing about
that will waste a day if you do not know it: the game writes a
`ModCrashSanityCheck` directory into the profile while it runs, deletes it on
a clean exit, and **disables every mod when it finds one left behind**. Kill
the game — as any test harness does — and the next run loads no mods. bg3le
removes it at startup, as bg3se does; `BG3LE_KEEP_SANITY_CHECK=1` keeps it,
which is how that was attributed (14 modules with it, 69 without).
[reference/MOD-LOADING.md](reference/MOD-LOADING.md) has the rest, including
that a savegame's module list replaces the load order.

Offsets are pinned to game version `4.8.400.7143220`. `tools/find_slots.py` and
`tools/recover_symbols.py` regenerate them for a new build. The reference
capture in `reference/` was taken against game `v4.73.98.727`, recorded in
`reference/version.txt` so a later mismatch is attributable.

## Contributing

Patches welcome. Five checks want running before a pull request, all of which
work without the game:

    ./tools/check-symbols.sh        # nothing references an undefined bg3le symbol
    ./tools/check-prelude.sh        # the Lua embedded in lua_host.cpp parses
    python3 tools/check-views.py    # the container views, and JSON escaping
    python3 client/tools/check-output.py   # the console's terminal handling
    python3 client/tools/check-prompt.py   # prompt width against readline's idea of it

`check-symbols.sh` is the one that matters most: the library links with
undefined symbols allowed, because it has to interpose the engine's own, so a
missing definition of *ours* builds cleanly and then kills the game at the
first call. That has happened three times.

And two that need the game running with bg3le attached:

    ./tools/check-reference.sh      # bg3le against the real extender's output
    ./tools/run-upstream-tests.sh [bg3se checkout]   # bg3se's own Lua tests

`reference/*.txt` is output captured from the Script Extender on Windows, and
that replays the same queries here and reports how far apart the answers are.
It does not decide pass or fail — most of what differs is that the install is
not the same one — but it is what found three broken entity calls and a key
in every stat dump that upstream does not have. See
[reference/REFERENCE-DIFFS.md](reference/REFERENCE-DIFFS.md).
`run-upstream-tests.sh` runs the server-side tests from a bg3se checkout's
`LuaScripts/Tests` (default `../bg3se`). Some of their expectations predate
the current game, so it prints each failure's reason rather than a verdict.

Two conventions worth knowing. Anything located by content is validated
before use — a structure has to agree about something only the real one could
— and a diagnostic that established a layout stays behind an environment
variable rather than being deleted, so the next game patch can re-run it.
`git log` is written to be read; a commit explains why, not what.

## How it hooks

No instruction-length decoder, and nothing is patched in the middle of a
function. Four primitives, in `src/hook.cpp`, `src/preload.cpp` and
`src/detour_interpose.cpp`:

1. PLT/dynamic-symbol interposition, by mangled name
2. vtable-slot patching — one aligned store, and it verifies the slot's
   current contents first, so a shifted binary is refused rather than corrupted
3. call-site patching — rewrites `call rel32` displacements to a nearby
   trampoline, since rel32 cannot reach a shared library from the executable
4. `DetourAttachEx`, for the vendored code that expects Microsoft Detours. It
   records the target and the replacement rather than patching either, and
   one exported forwarder per hooked function lets the dynamic linker do what
   Detours would have done. That is what brings up bg3se's seven Vulkan hooks
   for the ImGui overlay; the two mistakes it took to get right are in
   [reference/IMGUI-ASSESSMENT.md](reference/IMGUI-ASSESSMENT.md)

Symbols come from the native binary's own `.symtab` (102,920 of them) plus
11,214 recovered from embedded `__PRETTY_FUNCTION__` strings attributed to
their enclosing functions via `.eh_frame_hdr`.

## Licence

bg3le's own code is MIT. `vendor/bg3se/` remains under its upstream MIT +
Commons Clause terms; `vendor/bg3se/LICENSE` applies to it and forbids selling
the software.
