"""Attribute simpleperf's unknown-JIT samples to Berberis perf-map regions."""
import argparse
import bisect
import collections
from pathlib import Path
import re


parser = argparse.ArgumentParser()
parser.add_argument('profile', type=Path)
parser.add_argument('--top', type=int, default=12)
args = parser.parse_args()

profile = args.profile
samples = collections.Counter()
for line in (profile / 'um_ips.txt').read_text().splitlines():
    match = re.match(r'\s*([\d.]+)%\s+(?:(\d+)\s+)?0x([0-9a-f]+)\s*$', line)
    if match:
        weight = float(match.group(2) or match.group(1))
        samples[int(match.group(3), 16)] += weight

pattern = re.compile(r'^(.*?)_(lite|heavy|[a-z]+)_0x([0-9a-f]+)\+(\d+)')
regions = []
for order, line in enumerate((profile / 'um_perf.map').read_text().splitlines()):
    fields = line.split(None, 2)
    if len(fields) != 3 or not (match := pattern.match(fields[2])):
        continue
    begin = int(fields[0], 16)
    regions.append((begin, begin + int(fields[1], 16), order,
                    match.group(1), match.group(2), int(match.group(3), 16)))
regions.sort()
starts = [region[0] for region in regions]


def region_of(ip):
    index = bisect.bisect_right(starts, ip) - 1
    best = None
    for j in range(index, max(index - 64, -1), -1):
        candidate = regions[j]
        if candidate[0] <= ip < candidate[1] and (best is None or candidate[2] > best[2]):
            best = candidate
    return best


by_library = collections.Counter()
by_tier = collections.Counter()
by_region = collections.Counter()
unmapped = 0.0
for ip, weight in samples.items():
    region = region_of(ip)
    if region is None:
        unmapped += weight
        continue
    by_library[region[3]] += weight
    by_tier[region[4]] += weight
    by_region[region] += weight

total = sum(samples.values())
print(f'{profile}: {len(samples)} JIT IPs, {total:.1f} report units, '
      f'{unmapped / total:.1%} unmapped')
print('libraries:', ', '.join(f'{lib} {weight / total:.1%}'
                            for lib, weight in by_library.most_common()))
print('tiers:', ', '.join(f'{tier} {weight / total:.1%}'
                        for tier, weight in by_tier.most_common()))
print('hot regions:')
for region, weight in by_region.most_common(args.top):
    print(f'  {weight / total:5.1%} {region[3]} {region[4]} guest=0x{region[5]:x} '
          f'host=0x{region[0]:x}..0x{region[1]:x}')
