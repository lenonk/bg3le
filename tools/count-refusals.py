#!/usr/bin/env python3
"""Counts the Ext.* entry points and entity methods that refuse rather than answer.

The README quotes this number, and a number in prose rots: it said 86 for
some time after the real figure had fallen to 59. So it is derived from the
source instead of counted by hand, and re-derived whenever someone wonders.

A refusal is one of three shapes in src/lua_host.cpp:

    Ext.X.Y = needs("...")                    -- the common one
    for _, name in ipairs({...}) do           -- a whole module's worth
        Ext.X[name] = needs("...")
    end
    function Ext.X.Y(...)  error("bg3le ...") -- with no path that returns

The same shapes on entity_methods count too, as entity:Name, and a function
may be indented (inside a do block). A refusal that only happens on some path
has a return path too, so the third rule does not see it.
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.join(HERE, "..", "src", "lua_host.cpp")


def refusing_names(text):
    names = set()

    names |= set(re.findall(r"(Ext\.[A-Za-z_.]+)\s*=\s*needs\(", text))
    names |= set("entity:" + n for n in
                 re.findall(r"entity_methods\.([A-Za-z_]+)\s*=\s*needs\(", text))
    for m in re.finditer(
            r"for _, name in ipairs\(\{([^{}]*?)\}\) do\n"
            r"\s*entity_methods\[name\]\s*=\s*needs\(", text):
        for name in re.findall(r'"([^"]+)"', m.group(1)):
            names.add("entity:" + name)

    # The list body must not contain a brace and the assignment must follow
    # immediately, or the match runs away across the file.
    for m in re.finditer(
            r"for _, name in ipairs\(\{([^{}]*?)\}\) do\n"
            r"\s*Ext\.([A-Za-z]+)\[name\]\s*=\s*needs\(", text):
        listed, module = m.group(1), m.group(2)
        for name in re.findall(r'"([^"]+)"', listed):
            names.add("Ext.%s.%s" % (module, name))

    for m in re.finditer(
            r"^([ \t]*)function (Ext\.[A-Za-z_.]+|entity_methods:[A-Za-z_]+)"
            r"\(([^)]*)\)\n(.*?)\n^\1end$",
            text, re.M | re.S):
        name, body = m.group(2).replace("entity_methods:", "entity:"), m.group(4)
        # An error at the body level, not one behind an argument check.
        if not re.search(r"^" + re.escape(m.group(1)) + r'  error\("bg3le', body, re.M):
            continue
        if re.search(r"^\s*return\b", body, re.M):
            continue
        names.add(name)

    return names


def main():
    with open(SOURCE, encoding="utf-8") as f:
        names = refusing_names(f.read())

    by_module = {}
    for name in sorted(names):
        parts = name.split(".")
        module = "entity" if name.startswith("entity:") else (parts[1] if len(parts) > 2 else "(top level)")
        by_module.setdefault(module, []).append(name.split(":")[-1].split(".")[-1])

    print("%d of Ext.* and entity methods refuse rather than answer" % len(names))
    print()
    for module in sorted(by_module, key=lambda k: (-len(by_module[k]), k)):
        entries = sorted(set(by_module[module]))
        print("  %-12s %2d  %s" % (module, len(entries), ", ".join(entries)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
