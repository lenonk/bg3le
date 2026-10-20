"""Test the actual SDL forwarder with a fake SDL library; no game/display needed."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "sdl2"], text=True))
compiler = shlex.split(os.environ.get("CXX", "c++"))
with tempfile.TemporaryDirectory(prefix="bg3le-input-ownership-") as tmp:
    build = Path(tmp)
    fake = build / "fake-sdl.cpp"
    fake.write_text('''
#include <SDL.h>
#include <deque>
static std::deque<SDL_Event> events;
extern "C" void fixture_queue(SDL_Event const* event) { events.push_back(*event); }
extern "C" int SDL_PollEvent(SDL_Event* event) {
    if (events.empty()) return 0;
    if (event) { *event = events.front(); events.pop_front(); }
    return 1;
}
extern "C" Uint32 SDL_GetTicks() { return 1; }
''')
    common = [*compiler, "-std=c++23", "-Wall", "-Wextra", "-Werror", *flags]
    subprocess.run([*common, "-shared", "-fPIC", str(fake), "-o", str(build / "libfakeSDL.so")], check=True)
    binary = build / "test-input-ownership"
    subprocess.run([
        *common, "-I", str(root / "src"), str(root / "src/sdl_forward.cpp"),
        str(root / "tools/test-sdl-input-ownership.cpp"), "-L", str(build),
        "-lfakeSDL", "-ldl", "-pthread", "-Wl,-rpath," + str(build), "-o", str(binary)
    ], check=True)
    subprocess.run([str(binary)], check=True)
