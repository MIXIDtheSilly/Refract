"""Writes guest/lists/<lib>.txt export lists from a firmware's real libraries.

HLE stub libraries must export everything the real library exports (with the
same symbol versions), or the guest linker refuses to load apps linked against
them. Each line is "name[@VERSION]" for functions or "data:name:size[@VERSION]"
for objects.

usage: python gen_lists.py <sysroot>/system/lib64
"""
import struct
import sys
from pathlib import Path

LIBS = ["libandroid.so", "libEGL.so", "libGLESv1_CM.so", "libGLESv2.so", "libGLESv3.so", "libOpenSLES.so",
        "libaaudio.so", "libmediandk.so", "libnativewindow.so", "libcamera2ndk.so", "libjnigraphics.so",
        "libsync.so", "libbinder_ndk.so", "libamidi.so"]
OUT = Path(__file__).resolve().parent.parent / "guest" / "lists"


def exports(path: Path):
    data = path.read_bytes()
    shoff, = struct.unpack_from("<Q", data, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize) for i in range(shnum)]
    by_type = {}
    for idx, s in enumerate(sections):
        by_type.setdefault(s[1], []).append((idx, s))
    dynsym_idx, dynsym = by_type[11][0]  # SHT_DYNSYM
    strtab = sections[dynsym[6]]

    def string(off, tab=strtab):
        end = data.index(b"\0", tab[4] + off)
        return data[tab[4] + off:end].decode()

    # versions: SHT_GNU_versym (0x6fffffff), SHT_GNU_verdef (0x6ffffffd)
    versym = by_type.get(0x6FFFFFFF, [(None, None)])[0][1]
    verdef = by_type.get(0x6FFFFFFD, [(None, None)])[0][1]
    names = {}
    if verdef:
        off = verdef[4]
        vstr = sections[verdef[6]]
        while True:
            vd_version, vd_flags, vd_ndx, vd_cnt, vd_hash, vd_aux, vd_next = struct.unpack_from("<HHHHIII", data, off)
            vda_name, _ = struct.unpack_from("<II", data, off + vd_aux)
            if not (vd_flags & 1):  # skip the file's base version
                names[vd_ndx] = string(vda_name, vstr)
            if not vd_next:
                break
            off += vd_next
    count = dynsym[5] // 24
    out = []
    for i in range(1, count):
        st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from("<IBBHQQ", data, dynsym[4] + i * 24)
        bind, typ = st_info >> 4, st_info & 0xF
        if st_shndx == 0 or bind not in (1, 2) or (st_other & 3) not in (0,):
            continue
        if typ not in (1, 2):  # OBJECT, FUNC
            continue
        name = string(st_name)
        ver = ""
        if versym:
            v, = struct.unpack_from("<H", data, versym[4] + i * 2)
            if v & 0x8000:
                continue  # hidden (non-default) version
            if (v & 0x7FFF) in names:
                ver = "@" + names[v & 0x7FFF]
        out.append(f"data:{name}:{max(st_size, 8)}{ver}" if typ == 1 else f"{name}{ver}")
    return sorted(set(out))


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    lib_dir = Path(sys.argv[1])
    OUT.mkdir(parents=True, exist_ok=True)
    for lib in LIBS:
        src = lib_dir / lib
        if not src.exists():
            print(f"skipping {lib} (not in the firmware)")
            continue
        entries = exports(src)
        text = f"# Exports of the firmware's {lib} (tools/gen_lists.py).\n" + "\n".join(entries) + "\n"
        (OUT / (Path(lib).stem + ".txt")).write_text(text, encoding="utf-8", newline="\n")
        print(f"{lib}: {len(entries)} exports")
    return 0


if __name__ == "__main__":
    sys.exit(main())
