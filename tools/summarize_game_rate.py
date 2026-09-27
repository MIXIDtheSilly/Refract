"""Summarize five-second xrEndFrame rates from an Refract logcat run.

usage: python tools/summarize_game_rate.py runs/ns-heavy-mask --start 60 --end 120
The interval is seconds after the game's process starts. Compare runs with the
same game, scene, input, clock, render resolution, and interval.
"""
import argparse
from pathlib import Path
import re
import statistics


def seconds(stamp: str) -> float:
    hour, minute, second = stamp.split(':')
    return 3600 * int(hour) + 60 * int(minute) + float(second)


parser = argparse.ArgumentParser()
parser.add_argument('run', type=Path)
parser.add_argument('--package', default='com.meta.samples.NorthStar')
parser.add_argument('--start', type=float, default=60)
parser.add_argument('--end', type=float, default=120)
args = parser.parse_args()

log = (args.run / 'logcat.txt').read_text(errors='replace').splitlines()
start_re = re.compile(r'\bStart proc (\d+):' + re.escape(args.package) + r'/')
perf_re = re.compile(r'Refract\.Perf end-frame: rate=([\d.]+)/s .* samples=(\d+)')
start_time = pid = None
for line in log:
    if (match := start_re.search(line)):
        start_time, pid = seconds(line[6:18]), match.group(1)
        break
if start_time is None:
    raise SystemExit(f'No process start found for {args.package}')

rates = []
frames = 0
for line in log:
    if len(line) < 30 or line[18:24].strip() != pid:
        continue
    match = perf_re.search(line)
    if not match:
        continue
    elapsed = seconds(line[6:18]) - start_time
    if args.start <= elapsed < args.end:
        rates.append(float(match.group(1)))
        frames += int(match.group(2))
if not rates:
    raise SystemExit(f'No end-frame windows in {args.start}..{args.end} s')

sorted_rates = sorted(rates)
def percentile(p: float) -> float:
    return sorted_rates[round((len(sorted_rates) - 1) * p)]

print(f'{args.run}: pid={pid}, elapsed={args.start:g}..{args.end:g}s')
print(f'{len(rates)} windows, {frames} frames; mean={statistics.mean(rates):.1f}/s, '
      f'median={statistics.median(rates):.1f}/s, p10={percentile(0.1):.1f}/s, '
      f'p90={percentile(0.9):.1f}/s')
print('rates:', ' '.join(f'{rate:.1f}' for rate in rates))
