"""Write the symlink list refract_native needs for a firmware dump.

Extracting the dump's partition tars on Windows drops symlinks (for example
/system/lib64/libc.so -> /apex/com.android.runtime/lib64/bionic/libc.so), so
refract_native reads them from a text file: one "guest_path<TAB>target" per line.

usage: python native_sysroot_symlinks.py <dump>/tar <dump>/fs/refract_symlinks.txt
"""
import sys
import tarfile
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    tar_dir, out_path = Path(sys.argv[1]), Path(sys.argv[2])
    links = {}
    for tar_path in sorted(tar_dir.glob("*.tar")):
        try:
            with tarfile.open(tar_path) as tar:
                for member in tar:
                    if member.issym():
                        name = "/" + member.name.strip("/")
                        links[name] = member.linkname
        except (tarfile.TarError, OSError) as exc:
            print(f"skipping {tar_path.name}: {exc}", file=sys.stderr)
    with open(out_path, "w", encoding="utf-8", newline="\n") as out:
        for name in sorted(links):
            out.write(f"{name}\t{links[name]}\n")
    print(f"wrote {len(links)} symlinks to {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
