#!/usr/bin/env python3
"""Checks install.py's launch-option edits and installer/bg3le-launch's
argument rewrite, without Steam or the game."""

import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(HERE))
import install  # noqa: E402

failures = 0


def check(name, got, want):
    global failures
    if got == want:
        print("  OK   " + name)
    else:
        failures += 1
        print("  FAIL %s\n       got  %r\n       want %r" % (name, got, want))


W = "/home/u/.local/share/bg3le/bin/bg3le-launch"
add = lambda o: install.add_wrapper(o, W)

check("empty options", add(""), W + " %command%")
check("options around %command%", add("gamemoderun %command% -continueGame"),
      "gamemoderun " + W + " %command% -continueGame")
check("upper-case %COMMAND%", add("mangohud %COMMAND%"), "mangohud " + W + " %COMMAND%")
check("arguments only", add("-continueGame"), W + " %command% -continueGame")
check("already there", add(W + " %command%"), W + " %command%")
check("path with a space is quoted",
      install.add_wrapper("", "/home/a b/bg3le-launch"), '"/home/a b/bg3le-launch" %command%')
for opts in ("", "gamemoderun %command% -continueGame", "mangohud %COMMAND%", "-continueGame"):
    check("round trip %r" % opts, install.remove_wrapper(add(opts)),
          opts if "%command%" in opts.lower() or opts == "" else "%command% " + opts)
check("quoted path removed",
      install.remove_wrapper('"/home/a b/bg3le-launch" %command%'), "")

VDF = '''"UserLocalConfigStore"
{
\t"Software"
\t{
\t\t"Valve"
\t\t{
\t\t\t"Steam"
\t\t\t{
\t\t\t\t"apps"
\t\t\t\t{
\t\t\t\t\t"%s"
\t\t\t\t\t{
\t\t\t\t\t\t"LastPlayed"\t\t"1"
%s\t\t\t\t\t}
\t\t\t\t}
\t\t\t}
\t\t}
\t}
}
'''
with_opt = VDF % (install.APP_ID, '\t\t\t\t\t\t"LaunchOptions"\t\t"gamemoderun %command%"\n')
text, old, new = install.set_launch_options(with_opt, add)
check("existing LaunchOptions replaced in place",
      text, with_opt.replace('"gamemoderun %command%"', '"gamemoderun %s %%command%%"' % W))

without = VDF % (install.APP_ID, "")
text, old, new = install.set_launch_options(without, add)
check("missing LaunchOptions added in the app block",
      text, VDF % (install.APP_ID, '\t\t\t\t\t\t"LaunchOptions"\t\t"%s %%command%%"\n' % W))

other = VDF % ("228980", "")
text, old, new = install.set_launch_options(other, add)
check("missing app block added", install.parse_vdf(text).child("UserLocalConfigStore")
      .child("Software").child("Valve").child("Steam").child("apps").child(install.APP_ID)
      .child("LaunchOptions").value[2], W + " %command%")
check("other app untouched", text.startswith(other.split('\t\t\t\t}\n\t\t\t}')[0]), True)

quoted = VDF % (install.APP_ID, '\t\t\t\t\t\t"LaunchOptions"\t\t"A=\\"x y\\" %command%"\n')
text, old, new = install.set_launch_options(quoted, add)
check("escaped quotes read", old, 'A="x y" %command%')
check("escaped quotes written", '"A=\\"x y\\" %s %%command%%"' % W in text, True)

# The real file, when there is one: parsed, and unchanged outside the value.
for root in install.steam_roots():
    for path in install.local_configs(root):
        with open(path, encoding="utf-8", errors="surrogateescape", newline="") as f:
            real = f.read()
        # Out and back in when bg3le is already installed there.
        real_add = lambda o: install.add_wrapper(
            o, os.path.join(install.data_home(), "bg3le", "bin", install.WRAPPER_NAME))
        _, old, _ = install.set_launch_options(real, lambda o: o)
        steps = ((install.remove_wrapper, real_add) if install.WRAPPER_NAME in old
                 else (real_add, install.remove_wrapper))
        text, _, _ = install.set_launch_options(real, steps[0])
        back, _, _ = install.set_launch_options(text, steps[1])
        check("round trip leaves %s as it was" % os.path.basename(os.path.dirname(
            os.path.dirname(path))), back == real, True)

# The wrapper: a fake Steam command, and a fake game that prints its preload.
with tempfile.TemporaryDirectory() as tmp:
    root = os.path.join(tmp, "bg3le")
    os.makedirs(os.path.join(root, "bin"))
    wrapper = os.path.join(root, "bin", "bg3le-launch")
    with open(os.path.join(HERE, "..", "installer", "bg3le-launch")) as f:
        src = f.read()
    with open(wrapper, "w") as f:
        f.write(src)
    os.chmod(wrapper, 0o755)
    game = os.path.join(tmp, "Baldurs Gate 3", "bin")
    os.makedirs(game)
    with open(os.path.join(game, "bg3"), "w") as f:
        f.write('#!/bin/sh\necho "preload=$LD_PRELOAD args=$*"\n')
    os.chmod(os.path.join(game, "bg3"), 0o755)
    entry = os.path.join(tmp, "entry")
    with open(entry, "w") as f:  # stands in for reaper and the container
        f.write('#!/bin/sh\necho "entry sees preload=$LD_PRELOAD"\nwhile [ "$1" != "--" ]; do shift; done\nshift\nexec "$@"\n')
    os.chmod(entry, 0o755)

    env = dict(os.environ, LD_PRELOAD="overlay.so")
    out = subprocess.run([wrapper, entry, "--verb=waitforexitandrun", "--",
                          os.path.join(game, "bg3"), "-continueGame"],
                         capture_output=True, text=True, env=env).stdout.splitlines()
    check("the chain before the game is not preloaded", out[0], "entry sees preload=overlay.so")
    check("the game is, after the overlay",
          out[1], "preload=overlay.so:%s/lib/libbg3le.so args=-continueGame" % root)
    with open(os.path.join(root, "launch.log")) as f:
        check("launch.log names the game", f.readline().strip(),
              "bg3le: preloading into " + os.path.join(game, "bg3"))

    out = subprocess.run([wrapper, "/bin/echo", "proton", "LariLauncher.exe"],
                         capture_output=True, text=True).stdout.strip()
    check("a command without bin/bg3 runs unchanged", out, "proton LariLauncher.exe")

print("%d failures" % failures)
sys.exit(1 if failures else 0)
