#!/usr/bin/env python3
"""Cross-references in bg3, from the relocations its linker kept.

The Linux executable carries .rela.text and the other .rela.* sections, so
every code or data reference to a string, a global or a function is on
record. That is how the prototype Init functions stat:Sync() calls were found
(reference/STAT-WRITES.md); rerun it against a new build to re-derive them.

Usage:
  relocs-xref.py str NAME...        sites referencing a string literal
  relocs-xref.py addr ADDR...       sites referencing an address (hex)
  relocs-xref.py table START END    functions reading the interned names
                                    of a string-view table (hex vaddrs)

A code site is shown with its containing function: the nearest call target
at or below it. The first run builds a cache next to this script's working
directory (relocs-xref.cache); delete it after a game update.
"""
import bisect, collections, os, pickle, re, struct, subprocess, sys

BIN = os.environ.get("BG3_BIN", os.path.expanduser(
    "~/.local/share/Steam/steamapps/common/Baldurs Gate 3/bin/bg3"))
CACHE = "relocs-xref.cache"
RELA = [".rela.text", ".rela.data.rel.ro", ".rela.data", ".rela.rodata",
        ".rela.init_array"]


def load():
    if os.path.exists(CACHE):
        with open(CACHE, "rb") as f:
            return pickle.load(f)
    data = open(BIN, "rb").read()
    shoff, = struct.unpack_from("<Q", data, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    secs = []
    for i in range(shnum):
        name, _, _, addr, off, size, _, _, _, _ = \
            struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize)
        secs.append(dict(name=name, addr=addr, off=off, size=size))
    names = secs[shstrndx]
    for s in secs:
        end = data.index(b"\0", names["off"] + s["name"])
        s["name"] = data[names["off"] + s["name"]:end].decode()
    by = {s["name"]: s for s in secs}

    sym = by[".symtab"]
    values = [v for (_, _, _, _, v, _) in
              struct.iter_unpack("<IBBHQQ", data[sym["off"]:sym["off"] + sym["size"]])]
    text = by[".text"]
    refs, calls = {}, set()
    for rname in RELA:
        rela = by[rname]
        kind = rname[len(".rela"):]
        for off, info, addend in struct.iter_unpack(
                "<QQq", data[rela["off"]:rela["off"] + rela["size"]]):
            typ = info & 0xffffffff
            s = values[info >> 32]
            if typ in (2, 4, 41, 42):      # PC32, PLT32, GOTPCRELX, REX_GOTPCRELX
                target = s + addend + 4
            elif typ in (1, 10, 11):       # 64, 32, 32S
                target = s + addend
            else:
                continue
            refs.setdefault(target, []).append((kind, off))
            if (kind == ".text" and typ == 4
                    and text["addr"] <= target < text["addr"] + text["size"]):
                calls.add(target)
    ro = by[".rodata"]
    db = dict(refs=refs, calls=sorted(calls), rodata=(ro["addr"], ro["off"], ro["size"]))
    with open(CACHE, "wb") as f:
        pickle.dump(db, f)
    return db


def fn_of(db, p):
    return db["calls"][bisect.bisect_right(db["calls"], p) - 1]


def string_addrs(db, s):
    data = open(BIN, "rb").read()
    addr, off, size = db["rodata"]
    needle, found, at = s.encode() + b"\0", [], off
    while True:
        at = data.find(needle, at, off + size)
        if at < 0:
            return found
        if data[at - 1] == 0:
            found.append(addr + at - off)
        at += 1


def show(db, a, label):
    sites = db["refs"].get(a, [])
    print(f"{label} @ {a:#x}: {len(sites)} refs")
    for kind, p in sites[:40]:
        where = f"  in fn {fn_of(db, p):#x} +{p - fn_of(db, p):#x}" if kind == ".text" else ""
        print(f"    {kind} {p:#x}{where}")


def disasm(start, stop):
    out = subprocess.run(["objdump", "-d", "--no-show-raw-insn", "-M", "intel",
                          f"--start-address={start:#x}", f"--stop-address={stop:#x}", BIN],
                         capture_output=True, text=True).stdout
    return [l for l in out.splitlines() if ":\t" in l]


def table_readers(db, start, end):
    """slot -> per-name static initializer -> its caller, which stores a
    pointer to the name's id into a global -> the functions reading it."""
    refs, hist, traced = db["refs"], collections.Counter(), 0
    for slot in range(start, end, 16):
        for kind, site in refs.get(slot, []):
            if kind != ".text":
                continue
            for k2, callsite in refs.get(fn_of(db, site), []):
                if k2 != ".text":
                    continue
                startup = fn_of(db, callsite)
                for line in disasm(callsite + 4, callsite + 0x20)[:3]:
                    m = re.search(r"mov\s+QWORD PTR \[rip\+0x[0-9a-f]+\],rax\s+# ([0-9a-f]+)", line)
                    if not m:
                        continue
                    traced += 1
                    for k3, p in refs.get(int(m.group(1), 16), []):
                        if k3 == ".text" and fn_of(db, p) != startup:
                            hist[fn_of(db, p)] += 1
                    break
    print(f"{traced} names traced")
    for fn, n in hist.most_common(8):
        print(f"  {fn:#x}: {n}")


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    db = load()
    mode, args = sys.argv[1], sys.argv[2:]
    if mode == "str":
        for s in args:
            for a in string_addrs(db, s):
                show(db, a, s)
    elif mode == "addr":
        for a in args:
            show(db, int(a, 16), a)
    elif mode == "table":
        table_readers(db, int(args[0], 16), int(args[1], 16))


if __name__ == "__main__":
    main()
