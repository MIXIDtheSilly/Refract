"""Scan a raw memory dump for pointers into arm64 (guest) library mappings.

usage: scanptrs.py maps.txt dump.bin base_hex [lib_filter_regex]
Prints offset-in-dump, value, lib, file offset (vaddr-relative for first mapping).
"""
import re
import struct
import sys

maps_path, dump_path, base_hex = sys.argv[1:4]
lib_filter = re.compile(sys.argv[4] if len(sys.argv) > 4 else r"/lib/arm64/")
base = int(base_hex, 16)

ranges = []  # (start, end, lib, load_base)
first_map = {}
for line in open(maps_path):
    parts = line.split()
    if len(parts) < 6:
        continue
    start, end = (int(x, 16) for x in parts[0].split("-"))
    path = parts[5]
    off = int(parts[2], 16)
    if path not in first_map and off == 0:
        first_map[path] = start
    if lib_filter.search(path):
        ranges.append((start, end, path, off))

data = open(dump_path, "rb").read()
for i in range(0, len(data) - 7, 8):
    (v,) = struct.unpack_from("<Q", data, i)
    for start, end, path, off in ranges:
        if start <= v < end:
            lib = path.rsplit("/", 1)[-1]
            print(f"{base + i:#x} +{i:#07x} {v:#x} {lib} {v - first_map.get(path, start):#x}")
            break
