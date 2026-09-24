#!/bin/bash
# Runs bg3se's own server-side Lua test suite against bg3le.
#
# The tests are upstream's (BG3Extender/LuaScripts/Tests in a bg3se checkout,
# by Norbyte and the bg3se contributors); this only concatenates the ones
# upstream's ServerTestRunner.lua includes, plus the Osiris tests, and runs
# them through the console. Some expectations are older than the current game
# -- the base module is GustavX now, and entities are tables here rather than
# userdata -- so read the failures rather than counting them.
#
# Usage: tools/run-upstream-tests.sh [path to a bg3se checkout]
# Needs the game running with bg3le attached, past the level load.
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
BG3SE="${1:-$HERE/../../bg3se}"
TESTS="$BG3SE/BG3Extender/LuaScripts/Tests"
CLI="$HERE/../client/bg3lua"

if [ ! -f "$TESTS/TestHelpers.lua" ]; then
    echo "run-upstream-tests: no bg3se tests at $TESTS" >&2
    exit 1
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

for f in TestHelpers ModTests StaticDataTests StatTests ECSTests OsirisTests; do
    cat "$TESTS/$f.lua"
    echo
done > "$work/tests.lua"
echo 'RunTests()' >> "$work/tests.lua"

timeout 600 "$CLI" -f "$work/tests.lua" > "$work/out.txt" 2>&1

# Each result, and for a failure the first line of its error.
awk '/^Test OK: /    { print "  ok    " substr($0, 10) }
     /^Test FAILED: / { name = substr($0, 14); getline; print "  FAIL  " name ": " $0 }' \
    "$work/out.txt"
echo
printf '%s passed, %s failed\n' \
    "$(grep -ac '^Test OK: ' "$work/out.txt")" \
    "$(grep -ac '^Test FAILED: ' "$work/out.txt")"
