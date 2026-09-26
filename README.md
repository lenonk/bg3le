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
to the vendored code — mostly to compile under clang rather than MSVC, plus the
layout fixes this build's ABI needs and the hooks into bg3le. Its component
definitions have been checked against the
native build rather than assumed: `Ext._Internal.SizeAudit()` compares every
component's declared size with the size the engine recorded, and
`tools/meta-check.c` checks field offsets without needing the game.

## What works

bg3le implements the public `Ext` API of bg3se on the native build: every
name upstream exposes is present (`tools/api-coverage.lua`), and
`tools/count-refusals.py` finds none that refuse.
It is checked against output captured from the Windows extender
(`reference/`) and against upstream's own Lua tests. How each engine
structure was found, and what was measured, is in
[reference/IMPLEMENTATION-NOTES.md](reference/IMPLEMENTATION-NOTES.md).

- **Mods run.** Script Extender mods load straight out of their `.pak`s (or
  from loose directories via `BG3LE_MOD_PATH`) with upstream's mod
  environment, bootstraps, `Ext.Require` and `require`. `PersistentVars`
  and persistent variables are saved in the savegame in bg3se's format, so a
  save moves between the two. Mod Configuration Menu works, menu and all.
- **Two Lua contexts**, server and client, each on its own thread and
  reading its own entity world, as upstream has them. `Ext.Net` messages and
  synced mod and user variables cross between them; sessions and
  `Ext.Debug.Reset()` rebuild both.
- **Upstream's Lua environment**: Norbyte's Lua fork, upstream's sandbox,
  its `Ext.Events` library, `Ext.Json`, `Ext.Math`, and `Ext.Types` over
  upstream's own type registry, including its IDE helper generator.
- **Osiris**: the engine's functions, the story's own procedures, databases
  and queries, and `RegisterListener`.
- **Entities**: every component field bg3se describes reads and writes;
  entities and components can be created and removed; `OnCreate`,
  `OnChange` and system-update subscriptions fire; replication, NetIds and
  tracing work.
- **Stats**: every attribute kind reads and writes; `Create`, `Sync` for
  spells, statuses, interrupts and passives; functor execution and editing;
  `LoadStatsFile` and `SetRawAttribute` parse text as the engine's own
  loader does.
- **Game data**: `Ext.StaticData` (read, write, create), `Ext.Resource`,
  `Ext.Template`, `Ext.Loca`, and `Ext.Level`'s physics queries, AI grid,
  pathfinding and surface actions.
- **UI**: the `Ext.IMGUI` overlay (HDR-aware), `Ext.UI` on the game's own
  Noesis, and input events.
- **Files**: `Ext.IO` reads through the engine's own file reader and honours
  path overrides.
- **Achievements with mods active**, as bg3se's `EnableAchievements` does.
  Contributed by Igor Tarasyuk; `BG3LE_ACHIEVEMENTS=0` turns it off, and
  [reference/ACHIEVEMENTS-DIAGNOSIS.md](reference/ACHIEVEMENTS-DIAGNOSIS.md)
  has the addresses a maintainer needs when the binary updates.
- **Fixes for the native build itself**: level loads from 65–98 s to about
  1 s ([reference/SLOW-LOAD-DIAGNOSIS.md](reference/SLOW-LOAD-DIAGNOSIS.md)),
  an eGPU endgame save from 30 to 71 fps (also usable on its own, see
  [MEMSTEER.md](MEMSTEER.md)), and the engine's thread pinning undone.
- A Lua debugger server for the [bg3lua](https://github.com/lenonk/bg3lua)
  client (the `client/` submodule).

## Known gaps

- **No installer yet.** See [Running](#running).
- Two deliberate differences: a `require` after a mod has finished loading
  still works (upstream errors), and `Ext.Enums` entries are labels rather
  than `EnumValue` objects.

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
is an unsolved problem, not a documented step. The native build ships only
through Steam, so that is the one install it has to serve, ideally without the
player editing launch options by hand. Until that exists, running it means
knowing how to preload a library into a process inside the Steam runtime
container.

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
