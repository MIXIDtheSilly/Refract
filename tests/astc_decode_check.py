"""Checks tools/astc_decode.h against texture2ddecoder.decode_astc (pip install texture2ddecoder numpy).

Usage: python tests/astc_decode_check.py <astc_decode_check.exe> [real blocks: <bw>x<bh>:<file> ...]
Random blocks cover every block mode, partition count and endpoint mode. Most random blocks are illegal (weight
grid larger than the block, HDR endpoints, ...), which the spec decodes to the error colour while texture2ddecoder
decodes them anyway, so only blocks we accept are compared. Real blocks dumped from a game must all be valid.
"""
import os
import subprocess
import sys
import tempfile

import numpy as np
import texture2ddecoder as t2d

FOOTPRINTS = [(4, 4), (5, 5), (6, 6), (8, 5), (8, 8), (10, 10), (12, 12)]


def decode_ours(tool, bw, bh, blocks, tmp):
    nx = 64
    ny = (len(blocks) + nx - 1) // nx
    padded = np.zeros((nx * ny, 16), np.uint8)
    padded[:len(blocks)] = blocks
    padded[len(blocks):] = [0xFC, 0xFD] + [0xFF] * 6 + [0] * 8  # constant-colour (void extent) filler
    width, height = nx * bw, ny * bh
    paths = [os.path.join(tmp, 'blocks.bin'), os.path.join(tmp, 'decoded.rgba')]
    padded.tofile(paths[0])
    result = subprocess.run([tool, str(bw), str(bh), str(width), str(height)] + paths, capture_output=True, text=True, check=True)
    ours = np.fromfile(paths[1], np.uint8).reshape(ny, bh, nx, bw, 4).transpose(0, 2, 1, 3, 4).reshape(-1, bh * bw, 4)
    return padded, width, height, ours[:len(blocks)].astype(np.int32), result.stdout.strip()


def check(tool, bw, bh, blocks, label, tmp):
    padded, width, height, ours, stats = decode_ours(tool, bw, bh, blocks, tmp)
    error = (ours == [255, 0, 255, 255]).all(axis=(1, 2))
    if label == 'random':  # keep the legal blocks only (the reference decoder can crash on illegal ones)
        blocks = blocks[~error]
        padded, width, height, ours, stats = decode_ours(tool, bw, bh, blocks, tmp)
        errors = 0
    else:
        errors = int(error.sum())
    ny, nx = height // bh, width // bw
    ref = np.frombuffer(t2d.decode_astc(padded.tobytes(), width, height, bw, bh), np.uint8)
    ref = ref.reshape(ny, bh, nx, bw, 4)[..., [2, 1, 0, 3]].transpose(0, 2, 1, 3, 4).reshape(-1, bh * bw, 4)
    diff = np.abs(ours - ref[:len(blocks)].astype(np.int32)).max(axis=(1, 2))
    bad = int((diff > 1).sum())
    print(f"{label:8s} {bw:2d}x{bh:<2d} {len(blocks):6d} blocks: {bad} differ, {errors} error blocks, "
          f"max diff {diff.max(initial=0)}  ({stats})")
    if bad:
        print('   first differing block', bytes(blocks[int(np.argmax(diff > 1))]).hex())
    return bad == 0 and errors == 0


def main():
    tool = sys.argv[1]
    rng = np.random.default_rng(3)
    ok = True
    with tempfile.TemporaryDirectory() as tmp:
        for bw, bh in FOOTPRINTS:
            ok &= check(tool, bw, bh, rng.integers(0, 256, (200000, 16), dtype=np.uint8), 'random', tmp)
        for spec in sys.argv[2:]:
            dims, path = spec.split(':', 1)
            bw, bh = map(int, dims.split('x'))
            ok &= check(tool, bw, bh, np.fromfile(path, np.uint8).reshape(-1, 16), os.path.basename(path)[:8], tmp)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
