"""Lists the Android framework members a jar references (constant-pool scan).

The output is the API surface the Java framework shim (native/java) must provide
for an app's converted bytecode.

usage: python android_refs.py app.jar [--prefix android/ --prefix dalvik/ ...] [--provided shim.jar]
"""
import argparse
import struct
import sys
import zipfile
from collections import defaultdict


def parse_class(data: bytes):
    """Yields ('class', name) and ('member', owner, name, desc, kind) references,
    plus ('super', name) for superclass/interfaces."""
    pos = 8
    (count,) = struct.unpack_from(">H", data, pos)
    pos += 2
    cp = [None] * count
    i = 1
    while i < count:
        tag = data[pos]
        pos += 1
        if tag == 1:
            (ln,) = struct.unpack_from(">H", data, pos)
            cp[i] = ("utf8", data[pos + 2: pos + 2 + ln].decode("utf-8", "replace"))
            pos += 2 + ln
        elif tag in (3, 4):
            pos += 4
        elif tag in (5, 6):
            pos += 8
            i += 1
        elif tag == 7:
            cp[i] = ("class", struct.unpack_from(">H", data, pos)[0])
            pos += 2
        elif tag in (8, 16, 19, 20):
            pos += 2
        elif tag in (9, 10, 11):
            cp[i] = ({9: "field", 10: "method", 11: "imethod"}[tag],) + struct.unpack_from(">HH", data, pos)
            pos += 4
        elif tag == 12:
            cp[i] = ("nat",) + struct.unpack_from(">HH", data, pos)
            pos += 4
        elif tag == 15:
            pos += 3
        elif tag in (17, 18):
            pos += 4
        else:
            raise ValueError(f"bad constant pool tag {tag}")
        i += 1

    def utf(idx):
        return cp[idx][1]

    def cls(idx):
        return utf(cp[idx][1])

    for e in cp:
        if not e:
            continue
        if e[0] == "class":
            yield ("class", utf(e[1]))
        elif e[0] in ("field", "method", "imethod"):
            nat = cp[e[2]]
            yield ("member", cls(e[1]), utf(nat[1]), utf(nat[2]), e[0])
    # superclass + interfaces
    pos += 2  # access
    this_idx, super_idx = struct.unpack_from(">HH", data, pos)
    pos += 4
    if super_idx:
        yield ("super", cls(super_idx))
    (n,) = struct.unpack_from(">H", data, pos)
    pos += 2
    for k in range(n):
        yield ("super", cls(struct.unpack_from(">H", data, pos + 2 * k)[0]))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("jar")
    ap.add_argument("--prefix", action="append", default=None)
    args = ap.parse_args()
    prefixes = tuple(args.prefix or ["android/", "dalvik/", "com/android/", "androidx/"])
    own = set()
    members = defaultdict(set)
    classes = set()
    supers = set()
    with zipfile.ZipFile(args.jar) as z:
        for n in z.namelist():
            if n.endswith(".class"):
                own.add(n[:-6])
        for n in z.namelist():
            if not n.endswith(".class"):
                continue
            for ref in parse_class(z.read(n)):
                if ref[0] == "class" and ref[1].startswith(prefixes) and ref[1] not in own:
                    classes.add(ref[1])
                elif ref[0] == "super" and ref[1].startswith(prefixes) and ref[1] not in own:
                    supers.add(ref[1])
                elif ref[0] == "member" and ref[1].startswith(prefixes) and ref[1] not in own:
                    members[ref[1]].add((ref[2], ref[3], ref[4]))
    for c in sorted(classes | set(members)):
        tag = " (extended)" if c in supers else ""
        print(f"{c}{tag}")
        for name, desc, kind in sorted(members.get(c, ())):
            print(f"    {kind:7} {name} {desc}")
    print(f"# {len(classes | set(members))} classes, {sum(len(v) for v in members.values())} members",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
