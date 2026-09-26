#!/usr/bin/env python3
"""Install bg3le for the Steam copy of Baldur's Gate 3.

Copies the library, the console client and the launch wrapper into
~/.local/share/bg3le and adds the wrapper to the game's Steam launch options,
so the next launch from Steam loads bg3le. Steam has to be closed: it rewrites
localconfig.vdf from memory when it exits, so when it is running the installer
asks before stopping it.

    ./install.py              install, or update an existing install
    ./install.py --uninstall  take the launch option out and remove ~/.local/share/bg3le
    ./install.py --dry-run    show what would change
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

APP_ID = "1086940"
HERE = os.path.dirname(os.path.abspath(__file__))
WRAPPER_NAME = "bg3le-launch"


# ---- KeyValues text, edited in place ----

class Node:
    def __init__(self, key, value=None, children=None):
        self.key = key
        self.value = value          # (start, end, text) of a string value
        self.children = children    # list of Node for a block
        self.close = None           # offset of a block's closing brace

    def child(self, key):
        for c in self.children or []:
            if c.key.lower() == key.lower():
                return c
        return None


_TOKEN = re.compile(r'\s+|//[^\n]*|"((?:[^"\\]|\\.)*)"|[{}]', re.S)


def _unescape(s):
    return re.sub(r'\\(.)', lambda m: {"n": "\n", "t": "\t"}.get(m.group(1), m.group(1)), s)


def _escape(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')


def parse_vdf(text):
    """The root block; string values keep their offsets so they can be replaced."""
    tokens = []
    pos = 0
    while pos < len(text):
        m = _TOKEN.match(text, pos)
        if m is None:
            raise ValueError("unexpected text at offset %d" % pos)
        tok = m.group(0)
        if tok[0] == '"':
            tokens.append(("s", m.start(), m.end(), _unescape(m.group(1))))
        elif tok in "{}":
            tokens.append((tok, m.start(), m.end(), None))
        pos = m.end()

    root = Node("", children=[])
    stack = [root]
    i = 0
    while i < len(tokens):
        kind, start, end, text_ = tokens[i]
        if kind == "}":
            if len(stack) == 1:
                raise ValueError("unbalanced brace at offset %d" % start)
            stack.pop().close = start
            i += 1
            continue
        if kind != "s" or i + 1 >= len(tokens):
            raise ValueError("expected a key at offset %d" % start)
        nkind, nstart, nend, ntext = tokens[i + 1]
        if nkind == "s":
            stack[-1].children.append(Node(text_, value=(nstart, nend, ntext)))
        elif nkind == "{":
            node = Node(text_, children=[])
            stack[-1].children.append(node)
            stack.append(node)
        else:
            raise ValueError("unexpected brace at offset %d" % nstart)
        i += 2
    if len(stack) != 1:
        raise ValueError("unterminated block")
    return root


def _depth_indent(text, close):
    """The indentation of the line holding a block's closing brace."""
    line_start = text.rfind("\n", 0, close) + 1
    return text[line_start:close]


def set_launch_options(text, transform):
    """Apply transform(old) -> new to the game's LaunchOptions; returns (text, old, new)."""
    root = parse_vdf(text)
    path = ["UserLocalConfigStore", "Software", "Valve", "Steam"]
    node = root
    for key in path:
        node = node.child(key)
        if node is None or node.children is None:
            raise ValueError("no %s block" % "/".join(path))
    apps = node.child("apps")
    if apps is None or apps.children is None:
        raise ValueError("no apps block")
    app = apps.child(APP_ID)

    if app is not None and app.children is not None:
        opt = app.child("LaunchOptions")
        old = opt.value[2] if opt is not None else ""
        new = transform(old)
        if new == old:
            return text, old, new
        if opt is not None:
            start, end, _ = opt.value
            return text[:start] + '"%s"' % _escape(new) + text[end:], old, new
        indent = _depth_indent(text, app.close)
        line = '%s\t"LaunchOptions"\t\t"%s"\n' % (indent, _escape(new))
        at = text.rfind("\n", 0, app.close) + 1
        return text[:at] + line + text[at:], old, new

    old = ""
    new = transform(old)
    if new == old:
        return text, old, new
    indent = _depth_indent(text, apps.close)
    block = ('%s\t"%s"\n%s\t{\n%s\t\t"LaunchOptions"\t\t"%s"\n%s\t}\n'
             % (indent, APP_ID, indent, indent, _escape(new), indent))
    at = text.rfind("\n", 0, apps.close) + 1
    return text[:at] + block + text[at:], old, new


# ---- the launch option ----

def _quote(path):
    return '"%s"' % path if re.search(r'[\s"\'$`\\]', path) else path


def add_wrapper(options, wrapper):
    """Launch options with the wrapper in front of %command%."""
    if WRAPPER_NAME in options:
        return options
    token = _quote(wrapper)
    m = re.search(r"%command%", options, re.I)
    if m is None:
        # Options without %command% are arguments Steam appends to the game.
        return ("%s %%command%% %s" % (token, options)).strip()
    return options[:m.start()] + token + " " + options[m.start():]


def remove_wrapper(options):
    out = re.sub(r'(?:"[^"]*%s"|\S*%s)\s*' % (WRAPPER_NAME, WRAPPER_NAME), "", options)
    return "" if out.strip().lower() == "%command%" else out.strip()


# ---- Steam ----

def steam_roots():
    home = os.path.expanduser("~")
    seen, roots = set(), []
    for cand in (".local/share/Steam", ".steam/steam", ".steam/root"):
        path = os.path.join(home, cand)
        if os.path.isdir(os.path.join(path, "userdata")):
            real = os.path.realpath(path)
            if real not in seen:
                seen.add(real)
                roots.append(real)
    return roots


def local_configs(root):
    userdata = os.path.join(root, "userdata")
    out = []
    for user in sorted(os.listdir(userdata)):
        path = os.path.join(userdata, user, "config", "localconfig.vdf")
        if user != "0" and os.path.isfile(path):
            out.append(path)
    return out


def steam_running():
    try:
        with open(os.path.expanduser("~/.steam/steam.pid")) as f:
            pid = int(f.read().strip())
        with open("/proc/%d/comm" % pid) as f:
            return f.read().strip() == "steam"
    except (OSError, ValueError):
        return False


def stop_steam(roots):
    """Asks Steam to exit the way `steam -shutdown` does; True once it has."""
    cmd = None
    for root in roots:
        script = os.path.join(root, "steam.sh")
        if os.access(script, os.X_OK):
            cmd = [script, "-shutdown"]
            break
    if cmd is None and shutil.which("steam"):
        cmd = ["steam", "-shutdown"]
    if cmd is None:
        return False
    subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                     stderr=subprocess.DEVNULL, start_new_session=True)
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        if not steam_running():
            return True
        time.sleep(0.5)
    return False


def ensure_steam_stopped(roots):
    """False when Steam stays running; True if it was stopped here."""
    print("Steam is running. It has to be closed first: it rewrites its config "
          "when it exits, which would undo the change.")
    if not sys.stdin.isatty():
        sys.exit("install: exit Steam (Steam > Exit), then run this again.")
    try:
        answer = input("Stop Steam now? [y/N] ").strip().lower()
    except EOFError:
        answer = ""
    if answer not in ("y", "yes"):
        sys.exit("install: nothing changed. Exit Steam, then run this again.")
    print("Stopping Steam...")
    if not stop_steam(roots):
        sys.exit("install: Steam did not exit. Close it (Steam > Exit), then run "
                 "this again.")
    return True


def flatpak_steam():
    return os.path.isdir(os.path.expanduser(
        "~/.var/app/com.valvesoftware.Steam/.local/share/Steam/userdata"))


# ---- files ----

def data_home():
    return os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share")


def copy_atomic(src, dst, mode):
    """Copied beside the target and renamed over it, so a running game keeps
    the old library mapped."""
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(dst), prefix=".bg3le-")
    os.close(fd)
    shutil.copyfile(src, tmp)
    os.chmod(tmp, mode)
    os.replace(tmp, dst)


def write_atomic(path, text):
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(path), prefix=".bg3le-")
    with os.fdopen(fd, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
        f.write(text)
    shutil.copymode(path, tmp)
    os.replace(tmp, path)


def configs_need_edit(configs, transform):
    for path in configs:
        with open(path, encoding="utf-8", errors="surrogateescape", newline="") as f:
            text = f.read()
        try:
            if set_launch_options(text, transform)[0] != text:
                return True
        except ValueError:
            pass
    return False


def edit_configs(configs, transform, dry_run):
    for path in configs:
        with open(path, encoding="utf-8", errors="surrogateescape", newline="") as f:
            text = f.read()
        try:
            new_text, old, new = set_launch_options(text, transform)
        except ValueError as e:
            print("  %s: not changed (%s)" % (path, e))
            continue
        if new_text == text:
            print("  %s: nothing to change" % path)
            continue
        print("  %s\n    launch options: %r -> %r" % (path, old, new))
        if dry_run:
            continue
        backup = path + ".bg3le-backup"
        if not os.path.exists(backup):
            shutil.copy2(path, backup)
        write_atomic(path, new_text)


def main():
    ap = argparse.ArgumentParser(description="Install bg3le for Steam's Baldur's Gate 3.")
    ap.add_argument("--uninstall", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--lib", default=os.path.join(HERE, "build", "libbg3le.so"),
                    help="the library to install (default: build/libbg3le.so)")
    args = ap.parse_args()

    target = os.path.join(data_home(), "bg3le")
    wrapper = os.path.join(target, "bin", WRAPPER_NAME)

    roots = steam_roots()
    if not roots:
        if flatpak_steam():
            sys.exit("install: Flatpak Steam is not supported yet: its sandbox cannot "
                     "see %s" % target)
        sys.exit("install: no Steam installation found")
    configs = [c for r in roots for c in local_configs(r)]
    if not configs:
        sys.exit("install: no Steam user has a localconfig.vdf yet; start Steam once "
                 "and log in")
    transform = remove_wrapper if args.uninstall else (lambda old: add_wrapper(old, wrapper))
    # Only a config edit needs Steam closed; updating the files does not.
    stopped = False
    if (not args.dry_run and configs_need_edit(configs, transform)
            and steam_running()):
        stopped = ensure_steam_stopped(roots)
    restart = "\nSteam was stopped; start it again." if stopped else ""

    if args.uninstall:
        print("Steam launch options:")
        edit_configs(configs, transform, args.dry_run)
        # The directory is bg3le's alone: the install plus its offset caches.
        print("Removing %s" % target)
        if not args.dry_run:
            shutil.rmtree(target, ignore_errors=True)
        print(("bg3le removed." + restart) if not args.dry_run else "Dry run: nothing changed.")
        return

    files = [
        (args.lib, os.path.join(target, "lib", "libbg3le.so"), 0o755),
        (os.path.join(HERE, "installer", WRAPPER_NAME), wrapper, 0o755),
        (os.path.join(HERE, "client", "bg3lua"), os.path.join(target, "client", "bg3lua"), 0o755),
    ]
    for src, _, _ in files:
        if not os.path.isfile(src):
            sys.exit("install: missing %s%s" % (src, " (build first)" if src == args.lib else ""))

    print("Files, into %s:" % target)
    for src, dst, mode in files:
        print("  %s" % os.path.relpath(dst, target))
        if not args.dry_run:
            copy_atomic(src, dst, mode)
    print("Steam launch options:")
    edit_configs(configs, transform, args.dry_run)
    print(("Done: the next launch from Steam loads bg3le." + restart) if not args.dry_run
          else "Dry run: nothing changed.")


if __name__ == "__main__":
    main()
