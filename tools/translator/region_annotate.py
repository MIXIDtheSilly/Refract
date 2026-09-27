"""Annotates the hottest berberis-translated regions with per-instruction sample counts.

Inputs (one profile directory):
  um_ips.txt   simpleperf report --dsos unknown --sort vaddr_in_file -n
  um_perf.map  /data/data/<pkg>/perf-<pid>.map (setprop berberis.profiling <pkg> before launch)
  um_maps.txt  /proc/<pid>/maps
  code/<hex start>.bin  dumps of the executable memfd chunks (dd from /proc/<pid>/mem)
Optional: --guest-lib-dir with the app's arm64 libs, to show the guest instructions too.

usage: region_annotate.py <dir> [--top N] [--guest-lib-dir DIR]
"""
import argparse, bisect, collections, os, re

import capstone

ap = argparse.ArgumentParser()
ap.add_argument('dir')
ap.add_argument('--top', type=int, default=12)
ap.add_argument('--guest-lib-dir')
args = ap.parse_args()
d = args.dir

samples = collections.Counter()
for m in re.finditer(r'\s+\d+\.\d+%\s+(\d+)\s+0x([0-9a-f]+)', open(f'{d}/um_ips.txt').read()):
    samples[int(m.group(2), 16)] += int(m.group(1))
total = sum(samples.values())

pat = re.compile(r'^(.*)_(lite|heavy|[a-z]+)_0x([0-9a-f]+)\+(\d+)')
regions = []  # (host start, host end, map line, gear, guest start, guest size)
for n, line in enumerate(open(f'{d}/um_perf.map')):
    parts = line.split(None, 2)
    if len(parts) == 3 and (m := pat.match(parts[2].strip())):
        start = int(parts[0], 16)
        regions.append((start, start + int(parts[1], 16), n, m.group(2), int(m.group(3), 16), int(m.group(4))))
regions.sort()
region_starts = [r[0] for r in regions]


def region_of(ip):
    """Latest-installed region covering ip (code pool space gets reused)."""
    i = bisect.bisect_right(region_starts, ip) - 1
    best = None
    for j in range(i, max(i - 64, -1), -1):
        r = regions[j]
        if r[0] <= ip < r[1] and (best is None or r[2] > best[2]):
            best = r
    return best


chunks = []
for name in os.listdir(f'{d}/code'):
    start = int(name.split('.')[0], 16)
    chunks.append((start, open(f'{d}/code/{name}', 'rb').read()))
chunks.sort()


def host_bytes(start, end):
    for base, data in chunks:
        if base <= start and end <= base + len(data):
            return data[start - base:end - base]
    return None


maps = []
for line in open(f'{d}/um_maps.txt'):
    f = line.split()
    if len(f) >= 6:
        a, b = (int(x, 16) for x in f[0].split('-'))
        maps.append((a, b, int(f[2], 16), f[5]))
maps.sort()


def guest_location(addr):
    for a, b, off, path in maps:
        if a <= addr < b:
            return path, addr - a + off
    return None, None


by_region = collections.Counter()
for ip, n in samples.items():
    r = region_of(ip)
    by_region[r] += n

x86 = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
arm = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
print(f'{total} samples; {by_region[None]} outside known regions')
for rank, (r, n) in enumerate(by_region.most_common(args.top + 1)):
    if r is None:
        continue
    start, end, _, gear, guest, guest_size = r
    path, off = guest_location(guest)
    print(f'\n=== {n} samples ({n / total:.1%}) {gear} guest 0x{guest:x} (+{guest_size} B) '
          f'{os.path.basename(path or "?")}+0x{(off or 0):x}, host {end - start} B')
    if args.guest_lib_dir and path:
        lib = os.path.join(args.guest_lib_dir, os.path.basename(path))
        if os.path.exists(lib):
            code = open(lib, 'rb').read()[off:off + guest_size]
            for insn in arm.disasm(code, guest):
                print(f'        guest {insn.address:x}: {insn.mnemonic} {insn.op_str}')
    data = host_bytes(start, end)
    if data is None:
        print('    (host code not in dump)')
        continue
    for insn in x86.disasm(data, start):
        hits = sum(samples.get(a, 0) for a in range(insn.address, insn.address + insn.size))
        mark = f'{hits:5d}' if hits else '     '
        print(f'  {mark}  {insn.address:x}: {insn.mnemonic} {insn.op_str}')
