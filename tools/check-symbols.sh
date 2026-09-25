#!/bin/bash
# Fails if libbg3le.so references one of its own symbols that nothing
# defines.
#
# The library is linked with undefined symbols allowed -- it has to be,
# since the engine's own symbols are resolved at load time -- so a missing
# definition is not a build error. It is a crash at the first call, and it
# has happened twice: a source file left out of the target, and a function
# defined outside the anonymous namespace it was declared in.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
LIB="${1:-$HERE/../build/libbg3le.so}"

missing=$(nm -D --undefined-only "$LIB" | grep -E " U (bg3le|_ZN5bg3le)" || true)
if [ -n "$missing" ]; then
    echo "undefined bg3le symbols in $LIB:" >&2
    echo "$missing" >&2
    exit 1
fi
echo "no undefined bg3le symbols"

# Noesis is local to the game executable, so nothing resolves these at load
# time; each needs an entry in src/noesis_forward.cpp.
noesis=$(nm -D --undefined-only "$LIB" | grep -E " U _Z.*6Noesis" || true)
if [ -n "$noesis" ]; then
    echo "Noesis symbols missing from src/noesis_forward.cpp:" >&2
    echo "$noesis" >&2
    exit 1
fi
echo "no undefined Noesis symbols"
