"""Pulls every x86_64/arm64 ELF shared library out of a Unity NSIS installer payload (non-solid LZMA),
without parsing the install script: each file is one [u32 size|0x80000000][5-byte LZMA props][stream] block.
usage: nsis_elfs.py <exe> <out dir>"""
import lzma, os, struct, sys

exe, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
data = open(exe, 'rb').read()
fh = data.find(b'\xef\xbe\xad\xdeNullsoftInst') - 4
flags, _, _, header_size, archive_size = struct.unpack_from('<II12sII', data, fh)
pos, end = fh + 28, fh + archive_size
machines = {62: 'x86_64', 183: 'arm64', 40: 'arm', 3: 'x86'}

def unpack(block):
    props = block[0]
    lc, rest = props % 9, props // 9
    lp, pb = rest % 5, rest // 5
    dict_size = struct.unpack_from('<I', block, 1)[0]
    dec = lzma.LZMADecompressor(lzma.FORMAT_RAW, filters=[{'id': lzma.FILTER_LZMA1, 'lc': lc, 'lp': lp, 'pb': pb, 'dict_size': dict_size}])
    return dec.decompress(block[5:])

# Unity 6 installers use a 64-bit NSIS variant: 8 extra bytes after the first header, blocks are
# [u64 size | 1<<63] then sub-streams [u24 length][5-byte LZMA props][stream] (<= 2 MiB output each)
# ending with a zero length.
wide = struct.unpack_from('<Q', data, pos + 8)[0] >> 63 == 1
if wide:
    pos += 8
n = found = 0
while pos + 8 <= end:
    if wide:
        size = struct.unpack_from('<Q', data, pos)[0]
        compressed, size = size >> 63, size & ((1 << 63) - 1)
        block = data[pos + 8:pos + 8 + size]
        pos += 8 + size
    else:
        size = struct.unpack_from('<I', data, pos)[0]
        compressed, size = size & 0x80000000, size & 0x7fffffff
        block = data[pos + 4:pos + 4 + size]
        pos += 4 + size
    n += 1
    try:
        if wide and compressed:
            parts, i = [], 0
            while i + 3 <= len(block):
                length = int.from_bytes(block[i:i + 3], 'little')
                if not length:
                    break
                parts.append(unpack(block[i + 3:i + 3 + length]))
                i += 3 + length
            raw = b''.join(parts)
        else:
            raw = unpack(block) if compressed else block
    except lzma.LZMAError:
        continue
    if raw[:4] != b'\x7fELF':
        continue
    machine = struct.unpack_from('<H', raw, 18)[0]
    arch = machines.get(machine, str(machine))
    if arch not in ('x86_64', 'arm64'):
        continue
    # SONAME is hard to get without a full ELF parse; name by first lib*.so string plus size instead.
    i = raw.find(b'.so\x00')
    name = raw[raw.rfind(b'\x00', 0, i) + 1:i + 3].decode('latin1', 'replace') if i > 0 else 'unknown'
    path = f'{out}/{arch}_{n:05d}_{len(raw)}_{os.path.basename(name) or "unknown"}'
    open(path, 'wb').write(raw)
    found += 1
print(f'{n} blocks, {found} ELF libs written to {out}')
