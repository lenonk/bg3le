#!/bin/bash
# Launch the native Linux BG3 build with the extender shim attached.
#
# Shim logs go to $BG3LE_LOG.<pid> (default /tmp/bg3le.log.<pid>); the game's
# own process is the one whose log mentions COsiris.
#
# MangoHud is on by default. It loads as a Vulkan implicit layer rather than
# via its LD_PRELOAD shim, so it cannot collide with ours. MANGOHUD=0 or
# DISABLE_MANGOHUD=1 turns it off.
#
# GameMode is off by default; GAMEMODE=1 turns it on. See the comment further
# down for why it is off.
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
# The Steam install is the native build now, so the game and its Data live in
# one place and the separate tree with a Data symlink is gone.
GAME="$HOME/.local/share/Steam/steamapps/common/Baldurs Gate 3"
SNIPER_DIR="$HOME/.local/share/Steam/steamapps/common/SteamLinuxRuntime_sniper"

export MANGOHUD="${MANGOHUD:-1}"
export BG3LE_DUMP_DB="${BG3LE_DUMP_DB:-1}"  # temporary: structural dump

# Driver tuning, exported so it reaches the game inside the container.
#
# vk_x11_strict_image_count is a Mesa driconf option, and Mesa reads driconf
# options from an environment variable of the same name -- confirmed present in
# this machine's libvulkan_radeon.so.
#
# RADV_PERFOPTS is not a variable that driver reads. It reads RADV_DEBUG and
# RADV_PERFTEST, and "async_compile" appears nowhere in it; the RADV_PERFTEST
# options this build (Mesa 26.2.3) does carry include cswave32, gewave32,
# pswave32, nosam, nircache, transfer_queue, dccmsaa, localbos and
# video_decode. An unrecognised variable is simply ignored, so the line is a
# no-op rather than harmful -- kept as asked, and recorded here so it is not
# later mistaken for something that is doing work.
export RADV_PERFOPTS="${RADV_PERFOPTS:-async_compile}"
export vk_x11_strict_image_count="${vk_x11_strict_image_count:-false}"

# Wayland by default: a native surface rather than XWayland. Measurably
# steadier GPU clocks and slightly better frametimes, and the bundled
# libSDL2.so has the backend compiled in with libwayland-client present in the
# sniper runtime. SDL_VIDEODRIVER=x11 forces XWayland back.
#
# This is a preference, not a fix. A Proton DX11 run and a Proton Vulkan run
# share one windowing path and only one of them stuttered, so the stutter never
# tracked the window system.
export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-wayland}"

cd "$GAME"

# Without steam_appid.txt beside the binary, libsteam_api does not know which
# app this is and asks Steam to launch it properly. Steam then starts the game
# itself -- without our LD_PRELOAD -- and the shell that ran this script is
# told "command line was forwarded" and exits. The game comes up either way,
# which is what makes this worth guarding: the only visible symptom is that
# bg3le is silently absent and bg3lua finds no server.
#
# Written here rather than left as a manual step because a Steam update can
# remove it again.
if [ ! -f steam_appid.txt ]; then
    printf '1086940' > steam_appid.txt
    echo "run-native: wrote steam_appid.txt (Steam install lacked it)" >&2
fi

# SNIPER=0 runs the binary straight on the host, no container.
#
# It needs almost nothing: of the game's 16 shared libraries only two are
# missing here, libssl.so.1.1 and libcrypto.so.1.1, because this system has
# OpenSSL 3. Copies taken from the sniper platform sit in compat-libs and are
# put on the library path. They live outside the runtime tree on purpose --
# Steam's paths carry a version and move when the runtime updates.
#
# Worth having beyond tidiness: inside the container the libraries are recorded
# under /run/host, which does not resolve from outside its namespace, and that
# breaks perf symbolization and DWARF unwinding -- the reason several profiles
# this session could not name a single frame. GameMode also cannot find
# libgamemode.so or the session bus in there.
# CONTINUE=1 loads the last save straight from the launch rather than
# stopping at the main menu. The debugger only reaches the story thread once
# a save is up, so anything driving bg3lua from a script wants this.
args=("$@")
if [ "${CONTINUE:-0}" = "1" ]; then
    args=(-continueGame "${args[@]}")
fi

# BG3LE_EXTRA_PRELOAD appends another library to the preload list, for
# experiments that do not belong in the extender. Currently used to test
# thread affinity: the engine pins each of its threads to one logical cpu,
# while the same game under Proton runs with every thread on 0-15 because
# Wine does not pass the affinity requests through -- and that build keeps the
# gpu at 80% busy where the native one manages 38%.
# BG3LE_PRELOAD replaces the extender in the preload list rather than adding
# to it, which is how memsteer.so is tested against a real game: it is built
# from the same src/vulkan_memory.cpp, so running both would have two copies
# of the same interposition in one process.
preload="${BG3LE_PRELOAD:-$HERE/build/libbg3le.so}"
if [ -n "${BG3LE_EXTRA_PRELOAD:-}" ]; then
    preload="$preload:$BG3LE_EXTRA_PRELOAD"
fi

# The preload goes on the game and nothing else. It used to be exported, so
# every wrapper inherited it -- which broke gamescope outright, because
# src/vulkan_memory.cpp hid the device-local host-visible memory types that
# gamescope's own renderer needs ("findMemoryType failed", no backend).
# NOPRELOAD=1 runs the game without the extender at all. The control for
# any question of the form "is bg3le causing this?" -- the shim is the only
# instrument inside the process, so the comparison has to be made from
# outside it, with tools/memgrep.
if [ "${NOPRELOAD:-0}" = "1" ]; then
    game=(./bin/bg3 "${args[@]}")
else
    game=(env "LD_PRELOAD=$preload"
          "BG3LE_LOG=${BG3LE_LOG:-/tmp/bg3le.log}"
          ./bin/bg3 "${args[@]}")
fi

# HEADLESS=1 runs the game inside gamescope's headless backend: a real GPU
# and a real Vulkan swapchain, but no window on the desktop. For scripted
# runs that only talk to the debugger. SDL_VIDEODRIVER=offscreen does not
# work -- the game initialises Vulkan and then exits with no surface.
if [ "${HEADLESS:-0}" = "1" ]; then
    export SDL_VIDEODRIVER=wayland
fi

if [ "${SNIPER:-1}" = "0" ]; then
    COMPAT="$(cd "$HERE/.." && pwd)/compat-libs"
    if [ ! -f "$COMPAT/libssl.so.1.1" ]; then
        echo "run-native: SNIPER=0 needs $COMPAT/libssl.so.1.1" >&2
        exit 1
    fi
    export LD_LIBRARY_PATH="$COMPAT${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    launch=("${game[@]}")
else
    launch=("$SNIPER_DIR/run" -- "${game[@]}")
fi

# HDR=1 with HEADLESS=1 offers the game an HDR10 swapchain, as KWin does on
# the desktop, so the overlay's HDR encoding can be checked headless.
if [ "${HEADLESS:-0}" = "1" ]; then
    hdr=()
    [ "${HDR:-0}" = "1" ] && hdr=(--hdr-enabled --hdr-debug-force-support)
    launch=(gamescope --backend headless -W 1280 -H 720 "${hdr[@]}" -- "${launch[@]}")
fi

# GameMode is off unless asked for, because it does not work here and says so
# loudly. Inside the sniper container libgamemodeauto cannot dlopen
# libgamemode.so -- the container has its own /usr/lib, and the host's copy is
# not in it -- and cannot reach the session bus. The result was around 350
# lines of "dlopen failed" and "Could not connect to bus" per launch, and
# `gamemoded -s` still reporting "gamemode is inactive": it was never applying
# anything. An earlier note in this repo said otherwise on the strength of
# libgamemodeauto appearing in /proc/<pid>/maps; being mapped is not the same
# as working.
#
# Little is lost. What GameMode mainly does is set the CPU governor, and this
# machine already runs governor and energy_performance_preference at
# performance with the firmware profile at performance too. Its GPU
# optimisations need explicit opt-in and are not configured.
if [ "${GAMEMODE:-0}" != "0" ] && command -v gamemoderun >/dev/null 2>&1; then
    launch=(gamemoderun "${launch[@]}")
fi

exec "${launch[@]}"
