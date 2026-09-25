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
  `Ext.Timer`, `Ext.Utils`, `Ext.Config`, and upstream's `_D`/`_DS`/`_P`/
  `_PW`/`_PE`/`_C`/`_W` helpers and compatibility aliases. The interpreter
  is Norbyte's Lua fork, the same one bg3se uses — see
  [external/lua/README.bg3le](external/lua/README.bg3le) for why that is not
  optional. `Ext.Json` is a port of upstream's `Json.inl`: `Stringify`
  takes its options (`Beautify`, `IterateUserdata`,
  `StringifyInternalTypes`, `AvoidRecursion`, `LimitDepth`,
  `LimitArrayElements`, or the older positional form), sorts object keys
  the way it does, and writes `"*RECURSION*"` and `"*DEPTH LIMIT
  EXCEEDED*"` where it would; `Parse` is upstream's own, through rapidjson
  with its flags — comments, trailing commas, `NaN` — keeping `1.0` a float
  and a `null` in an array a hole. It was a Lua parser, and Mod
  Configuration Menu's settings made it most of the menu's load. So `_D` output matches upstream's line for line, stat
  dumps included (members, then attributes in the modifier list's order).
  A slow event handler is reported as upstream's profiler reports it: a
  warning over `Ext.Config`'s thresholds. `Ext.OnNextTick` is a one-shot
  `Tick` subscription, as upstream's is
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
  its table enters `Mods`. The environment is upstream's `ModLoader`'s: the
  mod's own `Ext` over the real one (a write to it is refused with
  upstream's message), its own `Ext.Log` with `Debug`, `MakePrinter`,
  topics and the `Log` event, `Sandboxed`, `print`/`_P`/`_PW`/`_PE`, `_G`
  as the mod's own table, and an `Ext.Require` that resolves against the mod
  and runs each file once — Mod Configuration Menu watches that assignment,
  so filling it in afterwards makes every mod look anonymous. Loose
  directories still work, via `BG3LE_MOD_PATH`. Of one 57-mod set, all five
  script mods load and run, MCM included (v1.40.1, "SE version 32").
  `Ext.Utils.Include` is upstream's: a mod's script by its UUID or name, a
  game file, or a `builtin://` script from bg3se's own bundle, which is
  embedded at build time from `vendor/bg3se/BG3Extender/LuaScripts`;
  `Ext.Require(mod, path)` loads another mod's file through it.
  `Ext.Utils.LoadTestLibrary` loads upstream's test runner from that bundle,
  and `"DeveloperMode": true` in `ScriptExtenderSettings.json` (off by
  default, as upstream's release builds are) loads it with each state along
  with upstream's development helpers
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
  from Lua, and on `TimerLaunch` for the story's own. A listener on a user
  query attaches to its `__DEF__` node, as upstream's does
- **Osiris user queries (`QRY_*`) are callable.** `Osi.QRY_Foo(...)` takes
  the IN arguments and answers `true` or `false`, or its OUT values (nils when
  it fails), as upstream's `OsiUserQuery` does, by calling the query node's
  `IsValid` with an identity adapter from Osiris' own adapter list.
  `QRY_Bard_GetPerformSpell` fills `DB_QRY_RTN_Bard_GetPerformSpell` as the
  story's own call does
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
  other side, and a request's reply comes back to the caller's callback.
  The receiver always gets its own parsed copy, a missing channel or
  handler warns as upstream's does, a failing request handler sends no
  reply, and `Ext.Net.Version()` is the protocol version, 2
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
  the LuaDebug protocol has carried a context on every request all along —
  and `:client` works at the main menu, where it runs on the client's tick.
  Extender messages go to the log and the console, not to the game's
  stdout, as upstream's go to their own window.
  Osiris is server-side, as upstream has it, and says so in the client
  context rather than blaming the save. Each context runs on its own
  thread, as upstream's do: the client ticks every client frame and gets
  `GameStateChanged` as its state moves, and messages between the two are
  queued. The client context's bootstraps run
  when the game leaves `LoadModule`, before the main menu is built, as
  upstream's do — so a UI mod's menu changes are in place when the menu
  appears. `Ext.Utils.GetGameState()` reports the client's real state there
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
  shows and the compiled prototype does not until it is synced.
  `GetCachedBoost` takes the GUID a `BoostInfo`'s `Prototype` holds, as
  upstream's does, over 4,713 boost prototypes: found from a live boost's
  GUID and followed back to the manager's static, and checked on each read
  by prototypes naming their own `BoostType` (a `Disadvantage` is the
  `Advantage` type under its own name). All 2,363 boosts in a save resolve
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
  `"userdata"` — and so are its components, the objects static data,
  prototypes and templates read as, and their arrays and maps, which
  dump, serialise and iterate as before and answer `nil` past the end, as
  upstream's arrays do — and there is one per handle while anything holds it, so it
  works as a table key and compares raw-equal the way upstream's
  value-compared proxies do. `Ext.Types.GetValueType` answers as upstream's
  does: `"Entity"`, a struct's name (base type `"CppObject"`), or Lua's own
  type name
- **Each context reads its own world.** The client context's entity reads go
  to the client EntityWorld, as upstream's client state's do, and the
  server's to the server's: `_C()` on the client is the controlled
  character from the client world, carrying the `ecl::` components and
  `ClientCharacter`, `GameObjectVisual` and the rest, under its own handle.
  UUID lookups, component lists and `GetAllEntitiesWithComponent` follow the
  same rule
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
  queued per world and delivered on that context's next tick — the client's
  subscribers get the client world's entities — immediate subscribers
  before deferred ones, because the Lua states may only be entered from
  their own thread — so a destroy handler gets `nil` for the component,
  which no longer exists by then. Applying `BLESS` with Osiris reaches an
  `OnCreate("ServerStatus")` handler with the status readable.
  `OnChange`/`Subscribe` fire too, from the replication pools as upstream
  reads them after the world update — the server tick sees each update's
  changes once — with the changed field flags and upstream's optional flag
  filter; a component with no replication index raises upstream's error.
  Damaging the host reaches an `OnChange("Health")` handler with field 1
- **`Ext.Entity.OnSystemUpdate`/`OnSystemPostUpdate` fire**, as upstream's
  `SetSystemUpdateHook` does it: the system's entry in its world's registry
  gets a trampoline in place of its `UpdateProc`, and the handler runs before
  or after the original, on whichever worker thread the scheduler ran it on,
  inside the context's own lock — upstream's `ContextGuardAnyThread`. A
  system is named by bg3se's label (`ServerBoost`, `ClientCharacterManager`)
  or the engine's class, and the context's own world is the one hooked, so a
  server system asked for on the client is "not registered", as upstream
  says. Checked: `ServerBoost` updates 30 times a second and the client's
  `ClientCharacterManager` 60, pre before post, `once` fires once, and
  handlers can read entities
- **Map keys arrive as upstream pushes them**: an entity key as the entity
  and an enum key as its label. `StatusContainer.Statuses` is keyed by the
  status entities, and each one can be looked up by the entity or followed
  to its own `ServerStatus`; a spell prototype's `MetaConditions` is keyed
  `Target`, not `5`. Maps inside prototypes, templates and static data are
  read too — they came back as `"<unreadable>"` before
- **`TranslatedString:Get()`**, upstream's way to turn a `DisplayName` into
  text: a character template's reads `"Nadira"`, and a companion's
  `DisplayName.Name:Get()` reads `"Shadowheart"`. It resolves through the
  engine's string repository, so strings the engine made at runtime resolve
  too: a player-named character's `ResStr_272917352` reads back as the name
  they typed
- **Root templates come from the engine's GlobalTemplateManager**, as
  upstream's `GetRootTemplate` reads them: all 32,911, from its bank's
  `Templates` map, whose keys are checked against each template's own Id. The
  manager's global is recorded for this build and checked before use
- **All of `Ext.Template`, from the engine's managers.** The server's
  `esv::CacheTemplateManager` and level manager were found beside the root
  manager, where the engine's own resolver loads all three; the current
  level's `LocalTemplateManager` and `CacheTemplateManager` hang off the
  level, at the offsets the resolver reads. Each is read live, under the
  rwlock the engine takes, so `GetAllLocalTemplates` returns the level's
  1,846 on the Nautiloid and `GetTemplate` falls through root, local, cache
  and level cache in upstream's order. The client gets upstream's client
  module: `GetTemplate`, `GetRootTemplate` and `GetAllRootTemplates`, root
  only. This replaced a memory scan for template-shaped objects, which took
  seconds on the warming thread and could not say which manager a template
  was in
- **`Ext.Level.GetLevelInfo` and `AddActivePersistentLevelTemplate`**, through
  the same level manager, as upstream: the first reads the level data
  manager's 512 levels live, and the second appends to the parent level's
  `ActiveLevelTemplates`, in place while it has room and otherwise into a
  fresh engine allocation. Like upstream, both exist only on the server
- **`Ext.Level`'s physics queries**, all eleven raycasts, sweeps and overlap
  tests, through the current level's `PhysicsSceneBase` on either side. Its
  virtuals are called as bg3se declares them, which this ABI lays out in the
  same order (two destructor slots, both cylinder sweeps stubbed, as the scene
  is checked for before any call), except that `TestBox` and `TestSphere` are
  one name overloaded in Larian's source and come in the opposite order; those
  two are called by slot. `TestBox` gets its position before its extents, as
  the engine reads them: upstream passes them swapped, which here hands PhysX
  a box with negative half-extents and crashes. Results are upstream's
  thread-local hit objects, reused rather than freed
- **`Ext.Level`'s AI grid tiles**: `GetEntitiesOnTile` (and upstream's
  `Ext.Entity` alias of it), `GetTileDebugInfo` and `GetHeightsAt`, as
  upstream's `Ai.inl` computes them over the current level's grid, whose
  vendored layout matches this build. Under the host they find the tile it
  stands on (its top 0.01m from the character's feet, flagged as blocked by a
  character) and the host as the one entity on it
- **`Ext.Level`'s pathfinding**: `BeginPathfinding`, `BeginPathfindingImmediate`,
  `FindPath`, `ReleasePath`, `GetPathById` and `GetActivePathfindingRequests`,
  as upstream's `PathfindingSystem` and `AiPath` helpers. A request takes a
  path from the grid's pool, fills it in from the source entity the way
  upstream's `SetSourceEntity` does, and goes onto the grid's `Paths` list,
  where the engine searches it; each tick the finished ones are released and
  handed to their callbacks. `FindPath` runs an unfinished search at once
  through the engine's own (`image+0x2646b30`, checked before use). From the
  host to a point 4m away both routes find the goal in 2 nodes, and bg3le's
  path fields match the engine's own path for the host. A path is a live
  view, as upstream's proxy is. Two things are left out: a pooled path whose
  Larian `Function` members are set is not taken, since resetting it would
  mean destroying them, and `FindPath` refuses a path with `IgnoreEntities`
  or `MovedEntities`, which the engine marks on the grid around the search
- **`Ext.Level.CreateSurfaceAction` and `ExecuteSurfaceAction`**, through the
  engine's own surface action factory and `SurfaceManager::AddAction`, found
  where the `CreateSurface` Osiris calls use them (each checked by its
  opening); the factory is handed the ClassDescription bank, as upstream sets
  it. A `CreateSurface` action of water, radius 2, turns the tile it is aimed
  at to `Water`, and executing it twice gets upstream's "already activated".
  A `TransformSurface` action gets its `Init` call as upstream's does, but the
  engine's own callers also seed its cell searcher with an area, which neither
  upstream's `ExecuteSurfaceAction` nor bg3le's does: in a test, freezing
  that water changed nothing
- **`Ext.Stats.ExecuteFunctors`, `ExecuteFunctor` and `PrepareFunctorParams`**,
  through the engine's nine per-context executors
  (`esv::ExecuteStatsFunctor_*Context`). They have no symbols; they were
  found from `DealDamageFunctor::ApplyDamage` (reached through the Osiris
  `ApplyDamage` handler, registered by name), up through the functions that
  merge hit results, and told apart by what each reads of its context: its
  Hit and Attack, or its first entity refs, at bg3se's offsets. Their
  addresses follow `FunctorContextType`'s order. Each is checked by its
  opening before it is called. A context is upstream's static per type;
  `ExecuteFunctor` clones the functor into a container of its own, on the
  engine's `Functors` vtable, since the executors call through it. Fire
  Bolt's `DealDamage`, run on a spawned rat, took it from 5 HP to 1.
  `Functors` views have upstream's `FunctorList`, each functor as its own
  class, and its `AddNew` and `Remove`. `AddNew` builds the functor on the
  engine's vtable for its type, taken from a compiled functor of that type
  and checked against the size the engine's own `Clone` allocates. An
  `ApplyStatus` made that way, given `BURNING` and `TARGET`, burns a spawned
  rat golem when executed
- **`Ext.StaticData.Create`, `ClearResourceBank` and `SyncResourceBank`.**
  `Create` adds the GUID to the bank's map under bg3se's Guid hash (checked
  against a sample of the bank's own keys first), default-constructs the
  resource and gives it the VMT of the bank's first, as upstream does. A
  full bank grows as upstream's does, the old values copied into a fresh
  buffer, but into fresh key and link buffers too, where upstream's frees the
  engine's. The other two are the bank's `ClearInternal` and `PostLoad`,
  called after checking the vtable is the one bg3se declares. In a test, 120
  new ClassDescriptions grew the bank from 104 to 224 and the old entries
  still read
- **`Ext.Entity.Create` and `Destroy`**, through the calling thread's entity
  command buffer as upstream's go. Two layout bugs stood in the way, both
  fixed in the vendored source. bg3se's `EntityHandleGenerator` put its
  per-thread states at +0x40, because its `ThreadState` shares a
  `ProtectedGameObjectBase` with it and the Itanium ABI will not overlap two
  of those; the engine indexes them from +0, so every thread's handles were
  read from its neighbour's. And its `ThreadState::Add` grew an empty state
  by two pages, leaving the first dead, where the engine grows one page with
  every entry free. `IsAlive` now checks the generator, as upstream's does,
  rather than the entity's storage
- **`Ext.Entity`'s tracing**: `SetupTracing`, `EnableTracing`, `GetTrace` and
  `ClearTrace`, logging what upstream's `ECSChangeTracer` logs, into bg3se's
  own `ECSChangeLog`, from a pre-hook on the engine's
  `EntityWorld::FlushECBs` (upstream's hook point; found where the world
  update calls it, and reached by tail call from two more callers, which
  `hook_call_sites` can now redirect too). Replication is logged at the
  update's second flush, since by bg3le's tick the pools have been sent.
  Like upstream's, enabling it needs DeveloperMode. Over two seconds of the
  host walking it logged 1,791 entities, with creates, destroys,
  modifications, one-frame and replicated changes, and an `Ext.Entity.Create`
  entity as `Create Immediate`. `GetTrace` hands back a copy of the log
  taken under its lock, since the engine writes it from its own thread
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
  backend with it, and the seven Vulkan hooks come up by interposition
  rather than by Detours. All thirty `Add*` kinds, the window
  setters, the style and colour accessors and the per-type methods are bound;
  properties read and write through the same field machinery a component
  does; and `OnClick`, `OnChange` and the rest arrive in Lua with their
  arguments, delivered on the tick of the context that registered them
  because a widget fires on a thread that must not touch a Lua state. Fonts
  come out of the game's archives and the mods' own, since the engine's file
  reader cannot be called by name here. When the compositor offers HDR
  the game presents HDR10 (KWin offers it even to a window on an SDR
  monitor), and drawing ImGui's sRGB colours straight into it, as upstream
  does, made the overlay oversaturated and its translucent parts too dark.
  For an HDR swapchain the overlay is drawn into an SDR image exactly as
  upstream draws it, and a fullscreen shader lays it over the game's frame at
  the game's own 300-nit UI white, blending as an SDR swapchain would; an SDR
  swapchain is drawn into directly, as upstream does. `BG3LE_HDR_UI_NITS`
  overrides the white level, and `HDR=1 HEADLESS=1 ./run-native.sh` offers
  HDR to a headless run to check it. On by default, as upstream's is; `BG3LE_IMGUI=0`
  turns it off. Enum properties take their labels, `P_BITMASK` flags such
  as `Window.AlwaysAutoResize` are properties, whole arrays can be assigned,
  `UserData` and `Children` behave as upstream's, struct and container
  properties such as `Table.ColumnDefs` read and write as a component's do,
  and assigning nil to an event clears it. Icons draw: the
  texture atlas map is located, and an atlas's resident texture is handed
  to the renderer as it is. See
  [reference/IMGUI-ASSESSMENT.md](reference/IMGUI-ASSESSMENT.md)
- **`Ext.UI` on the game's Noesis**, so Mod Configuration Menu's main-menu
  button opens its window. Upstream links no Noesis library and reimplements
  the pieces it calls against Windows internals; the Linux game has every
  one of them as a local symbol, so `src/noesis_forward.cpp` sends each call
  to the game's own function, resolved from `.symtab`. The class layouts and
  vtables were checked against the game's: the headers are Noesis 3.1.7, the
  game 3.1.6, and all 38 shared vtables match. The root is the content of the
  one `Noesis::View`, which is caught by hooking its per-frame `Update`.
  Elements are proxies with upstream's `Find`, `Child`, `VisualChild`,
  `GetProperty`/`SetProperty`, `Subscribe` and the rest, over the same class
  cache and value conversions; `RegisterType` and `Instantiate` build custom
  data contexts with upstream's own builder, and a `Command` property's
  `SetHandler` runs when the button is pressed. Commands and routed events
  are delivered on the client tick, so a handler cannot set `Handled` on its
  event; `WriteCallback`, `GetStateMachine` and the picking, cursor and
  drag-and-drop managers are not there yet
- **Sessions, as upstream has them**: when the client unloads a session --
  back to the main menu, or loading another save -- both Lua states are
  rebuilt; the client's mods reload as the menu finishes loading, and the
  server's with the next story, which binds `Osi` again. A new save's
  `PersistentVars`, variables and timers reach the new states.
  `Ext.Debug.Reset()` works at the menu as well as in a session
- **Input events**: `KeyInput`, `MouseButtonInput`, `MouseWheelInput`, the
  controller events and `ViewportResized`, thrown from the game's own
  `SDL_PollEvent` after the overlay has seen the event, as upstream does,
  with keys and buttons as their enum labels. `PreventAction` swallows the
  input. Mod Configuration Menu finds its in-game menu button this way, when
  Escape is pressed
- **`Ext.Loca` on the engine's own `TranslatedStringRepository`**, read and
  written as upstream does, so a string a mod sets is what the game's
  interface shows — Mod Configuration Menu's main-menu button is labelled
  this way. Strings set before the localisation loads are queued and applied
  the moment it does. The key functions read and write the engine's
  `TranslatedStringKeyManager` (11,128 keys), found by upstream's own
  anchor through the executable's relocations
- **`Ext.Utils.GetGlobalSwitches`** returns the engine's `ls::GlobalSwitches`,
  live: language, UI scale, sensitivities, save and timeline switches, the
  twelve sound settings and four camera switch sets (`StartYear` reads 1492).
  The global was found through the executable's relocations -- the settings
  registration reads 102 of the switch names next to 60 loads of it -- and
  pairing each name with the member it reads confirmed bg3se's layout for all
  46 such members, once two size differences on this build were corrected in
  the vendored header. bg3se's own overlay now reads its language from it.
  [reference/GLOBAL-SWITCHES.md](reference/GLOBAL-SWITCHES.md)
- **`Ext.Utils.GetDialogManager`** hands back the server's
  `dlg::DialogManager` from `esv::DialogSystem`, as upstream's does (nil on
  the client). The pointer sits 16 bytes before where bg3se's declared layout
  puts it, so it is only trusted when the manager's `FlagDescriptions` name
  each flag kind the way their keys do. `Ext.Types.Construct` checks the
  type exactly as upstream's does -- unknown, not an object, not
  constructible -- and then, like upstream's, whose body is a TODO, returns
  nothing
- **A line on the main menu**, as upstream has: the copyright string gets
  bg3le's line through the repository as the game leaves `LoadModule`
- **`PersistentVars` in the savegame**, as upstream writes them: a
  `ScriptExtenderSave` region (save version 12) with a `LuaVariables` node
  per mod, visited through the save's own LSF visitor by pre-hooking the
  engine's `OsirisVariableHelper::SavegameVisit`. A save loaded at launch is
  read before any mod exists, so the values are held and restored after the
  bootstraps and before `SessionLoaded`, which is upstream's order. The saves
  read and write in bg3se's format, deprecation warnings included. Persistent
  user and mod variables and persistent timers go into the same region, node
  for node as upstream writes them, so a save moves between bg3se and bg3le:
  tables are written as JSON text, which bg3se reads, and bg3se's binary form
  is read through its own decoder. `Ext.Json` has upstream's binary mode
- `Ext.Entity` against the live ECS: `Ext.Entity.Get(uuid)`, component reads
  and writes, and `entity:Replicate(name)` that reaches the client. The engine
  names every ECS type index in its symbol table, so the component and
  replication registries come straight out of `.symtab` — the Windows extender
  has to recover the same mapping by scanning the image for byte patterns.
  Fields come from bg3se's own generated metadata rather than from accessors
  written per component, so every component it describes is reachable by name;
  see [What is left](#what-is-left) for the kinds that do not convert yet
- `Ext.Resource.Get`/`GetAll` against the engine's `ls::ResourceManager`,
  the other resource system — visuals, textures, materials, effects, sounds
  and dialogs, 34 banks keyed by `ResourceBankType` — read from the current
  bank as upstream's `GetResource` reads it. The manager's global is
  recorded for this build and checked before use, and searched for by
  fingerprint when it disagrees: a ResourceBank's 34 banks each hold their
  own index as `BankTypeId`. The Visual bank's 60,559 ids list in a third
  of a second, and a resource reads through bg3se's metadata like any other
  object. Where Windows has an `SRWLOCK` the Linux engine has a glibc
  `pthread_rwlock_t`, 48 bytes wider, which is what put every field after
  one in the wrong place; bg3le's `SRWLOCK` is that type now
- `Ext.Utils.ShowErrorAndExitGame` shows its message in a dialog the overlay
  draws, in the game's frame, and closes the game when it is dismissed. The
  call does not wait for it, since the frame waits on the game thread; it
  stops the calling script instead, so nothing after it runs, as upstream's
  exit guarantees. Without the overlay it falls back to SDL's message box
- The console refuses a server-context command at the main menu with the
  reason -- the server context runs once a save is loaded -- rather than
  timing out, and a command that does time out is withdrawn rather than run
  whenever the thread next comes round
- `Ext.StaticData.GetSources` and `GetByModId`, from each bank's
  `ResourceGuidsByMod`: which mod defines which resources
- `Ext.StaticData.GetIconUVs`, `GetIconAtlas` and `GetTextureAtlasManager`,
  upstream's reads of `ls::gTextureAtlasMap`: an icon's UVs, its atlas (path,
  texture, sizes and every icon in it) and the whole map, 34 atlases and
  6,406 icons
- `Ext.StaticData.Get`/`GetAll` against the engine's GUID resource manager.
  It has no symbol, so it is found by fingerprint: the manager is one
  `HashMap<StaticDataTypeIndex, GuidResourceBankBase*>`, and a table whose
  keys are all drawn from the 121 static data type indices the symbol table
  already names is that manager rather than a coincidence. Resources are
  writable, which is what a mod that edits spell lists needs: a resource's
  fields, array elements and map values write through (an array also takes
  upstream's `arr[#arr + 1] = v` and `arr[i] = nil`), and a hash set behaves
  as upstream's set proxy — `list.Spells["Target_Light"]` is whether it holds
  that spell, assigning `true` or `nil` adds or removes it, `pairs` and
  `Ext.Types.GetHashSetValueAt` walk it — or is replaced whole by
  `Ext.Types.Unserialize` or by plain assignment. That holds for sets of
  GUIDs, entities, integers and enums (by label or number) as well as
  FixedStrings, on components as well as resources. Every change to a set
  rebuilds its hash table, and doing
  that through bg3se's own container methods took the game down twice — the
  offsets, the hash rule and the two things not to call are in
  `reference/STATIC-DATA-WRITES.md`. Types are named by upstream's
  `ExtResourceManagerType` label, which for 17 of them is not the class
  name — `ColorDefinition` is `resource::Color` — and the five
  character-creation default-value managers, whose names bg3se writes the
  MSVC way, resolve too
- **`Ext.Stats.Create`**, as upstream's `RPGStats::CreateObject`: a new
  entry in the modifier list, copied from a template if one is named, built
  the way the engine's own parser builds one (0xf0 bytes from the engine
  heap, its maps and defaults as the engine sets them) and inserted into
  `RPGStats::Objects` by name. `Sync` then gives it a prototype, as
  upstream's `SyncStat` does for a stat it has not seen: a spell, status or
  interrupt created from a template syncs, and `Osi.AddSpell` puts a created
  spell in a character's spellbook. Engine maps and arrays are grown through
  `src/vendor/engine_containers.h`: in place while the engine's capacity
  allows, as bg3se's own `insert` does, and never by freeing what the
  engine allocated. The stats list itself is now re-read from its header
  rather than kept from the moment it was found -- found mid-load, it had
  27,821 entries where the finished array holds 23,964, and the tail was
  stale. `AddEnumerationValue` appends a label to an enumeration at the
  next value, into the bucket the engine's own nodes say it belongs in, and
  `AddAttribute` extends a modifier list -- refusing, as upstream does, once
  stats objects exist. `LoadStatsFile` is upstream's own Lua
  (`builtin://Libs/Stats.lua`) over those. `SetRawAttribute` takes
  stats-file text as the engine's loader does: numbers as strings, flags
  joined by `;`, `""` resetting an attribute to its default, requirements
  such as `!Immobile`, a `ComboCategory` line appended to the stat's set,
  and roll conditions and functor lists split into `[TextKey]` groups by the
  engine's own splitter. Functor text is compiled by the engine's per-functor
  parser into sets registered as the loader registers them, after
  upstream's `ClearStatsFunctors`. Like upstream's, it reports a bad value
  rather than throwing, so a file loads past it. Reloading seven of the
  game's own stats files over the loaded game changes 634 of 404,093
  values: 340 are armour combo categories appended a second time, as the engine's
  loader appends them, and the rest are the files' own values coming back
  over later mods and patches. `Ext.IO.LoadFile`
  in the `data` context reads the game's own archives now, after loose files
  and mod archives, as the engine's file system layers them
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
  found and how the next one will be; `BG3LE_PROFILE=1` times every call a
  slow event handler makes, C functions included, and logs the most
  expensive. That is what showed 5eSpells' `StatsLoaded` spending 4.4 of its
  6 seconds waiting on the template scan and half a second on the GUID
  resource manager's; with the templates from their manager and the GUID
  manager at the offset it has in every run, the handler takes 0.7 seconds
  and bg3le's share of the level load went from 6.8 seconds to 1.05. Every attribute kind is decoded —
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
  `tools/grab-reference.sh` reproduces the capture, and
  `tools/check-reference.sh` compares against it: 15 of 25 captures are
  identical, and what the rest differ in is the install (mod counts, the
  save loaded, hash-ordered labels), not bg3le
- A Lua debugger server compatible with the
  [bg3lua](https://github.com/lenonk/bg3lua) client (`client/` submodule),
  plus `CreateConsole` parity that opens a terminal on startup
- **A 65-98s level load reduced to ~1s.** The native build spends almost all
  of it in `physx::Sn::ConvX` converting PhysX data whose `TempAllocator`
  serialises on one global mutex; `src/fast_alloc.cpp` replaces it with a
  lock-free thread-local pool. Its free call sites are patched before its
  allocate ones, and it stays off unless both are, since the engine cannot
  free a block it did not allocate. See
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

- **No `Ext.*` function refuses any more.** Every name bg3se exposes is
  present — `tools/api-coverage.lua` reports 715 of 715 — and
  `tools/count-refusals.py`, which counts the ones that raise instead of
  answering, now finds none. What is left is in the smaller gaps below
- **Stat writes and `Sync`, all but a passive's rebuild.** Every attribute kind upstream
  writes is written, the way its `Object::Set*` writes it: integers and
  enumerations in place; conditions, strings, floats, GUIDs, flag sets and
  translated-string handles into the matching `RPGStats` pool; roll
  conditions (a string or a `{Name = expression}` table) and requirements
  into the stat's own containers; `AIFlags` onto the object. A pool with no
  spare capacity moves to a fresh buffer from the engine's own allocator,
  with the old one left in place. Re-assigning every attribute of a
  sample of 105 stats across seven modifier lists to itself changes none
  of 8,310 values, and bg3se's `TestStatAttributes` fails only on a
  hash-order and a stale functor expectation. (Assigning a functor list
  fails as upstream's does; `SetRawAttribute` writes one.) An earlier
  version skipped a pool slot per write, one more each time; see
  `pool_slot` in `src/vendor/stats.cpp`. `CopyFrom` is upstream's: the
  indexed properties, then the functor and roll-condition maps,
  requirements and both combo sets, and it refuses across modifier lists
  exactly as upstream does. `ComboProperties` and `ComboCategories` read
  and assign the stat's own sets. `Sync` rebuilds a spell, status
  or interrupt prototype the way upstream's does, through the engine's own
  `Init` functions — found from the relocations the executable kept
  (`tools/relocs-xref.py`) and checked before every call — so an edited
  stat reaches the game; syncing 400 unchanged spells leaves every
  prototype exactly as the loader built it. Passives are parsed inside
  their loader on this build, with no per-passive rebuild to call, so
  syncing one says so once; `Ext.Stats.GetCachedPassive`'s fields are
  writable. `SetPersistence` warns that it is deprecated, as upstream's
  does. [reference/STAT-WRITES.md](reference/STAT-WRITES.md) has the
  layout, the Init hunt and the three theories that were tested and
  eliminated
- **The last few field kinds.** Every component field converts — 3,566 of
  3,566, from `tools/meta-check.c` (94.0% before `STDString` was given this
  build's sixteen-byte layout): scalars of any integer type (the vendored
  headers' MSVC `__int64` and `__int8` are `long long` and `char` here, which
  hid 178 fields), enums and bitmasks, nested structs, pointers (to a
  described class, read as that object when first touched, so a cycle is only
  walked as far as it is asked about; to anything else — a set, a map, a
  string — read as what it points at; one that could not be an object or
  cannot be read is refused rather than followed; a pointer to a pointer is
  followed twice), fixed and dynamic arrays including `LegacyArray` (resized
  through its `Array` base, as upstream does), `CompactSet` and the sets built
  on it, `StaticArray` and `std::vector` (written in place, and assigned whole
  only at their own length: neither can be grown here), a `Queue` (read-only,
  in order), a `BitArray` (a table of booleans, read and written whole, as
  upstream's Serialize and Unserialize do), hash sets, hash maps and the
  node-chained `LegacyMap`/`LegacyRefMap`, glm vectors and matrices (a matrix
  as its
  floats, as upstream pushes one), `std::optional`, `std::variant`,
  `FixedString`, `OverrideableProperty`, `ecs::EntityRef`, and the types
  upstream pushes as what they hold — `Path` as its string, C strings, string
  views, byte buffers and Noesis strings as strings, `Version` as its four
  numbers, `EntityOrVec3Variant` as a position or an entity, `NetId` and
  `UserId` as integers, a component handle as an integer or nil, and a
  `stats::ConditionId` as its condition's text, a `StatsExpressionRef` as
  its pooled expression with `Code` and `RefCount`, refusing a write with
  upstream's message. An `std::optional` is written as well as read, through
  the container's own `emplace()` and `reset()`, and a `std::variant` is read
  by the engine's layout rather than this compiler's — the game is libc++ ABI
  2, see [reference/LIBCXX-ABI.md](reference/LIBCXX-ABI.md). Across every
  class the metadata describes, 461 of 21,514 fields do not convert; 448 of
  them are the ImGui widgets' Lua delegates and registry entries, which
  `Ext.IMGUI`'s own callbacks handle. The other 13 are left out on purpose:
  `TypeInformation`'s six `TypeInformationRef`s (bg3le's `Ext.Types`
  builds its type information as Lua tables, not as these objects) and seven
  Noesis observable collections, which would need Noesis's object model;
  `meta-check <lib> --unsupported` lists them. A whole array assigned from a
  list is refused, rather than filled with defaults, when its elements are
  structs or pointers; an empty list always clears it. Naming an unsupported field
  raises rather than returning nil, so a mod cannot mistake a missing
  conversion for a missing value
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
Today 11 pass and 6 fail, each for a reason outside bg3le: `TestBaseMod` and
`TestModManager` expect the old base module; `TestStatAttributes` compares
functor objects as JSON, which upstream's own `Stringify` refuses without
`IterateUserdata`; `TestECSComponents` expects `DisplayName.Name` to be a
string, where upstream now returns a TranslatedString; `TestECSFunctions` calls
`GetEntityType`, which upstream has since removed; and `TestGuidResourceLayout`
meets 27 SpellLists (with this mod list) whose engine objects carry a zero
`ResourceUUID` under a real key.

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
   trampoline, since rel32 cannot reach a shared library from the executable.
   Trampolines share pages: with one page each, the free space in range ran
   out on some address layouts
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
