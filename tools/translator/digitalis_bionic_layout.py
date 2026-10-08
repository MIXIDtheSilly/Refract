"""Which Android 16 bionic layout a Digitalis bundle's ARM64 bionic was built with.

Digitalis' ARM64 linker copies bionic_tls out of the host's pthread_internal_t, whose layout changed in
Android 16 QPR2 (stack_bottom): it reads offset 0x2f8 when built for QPR0/QPR1 (the Windows emulator, BE2A)
and 0x300 for QPR2 (Waydroid's LineageOS 23.2). Disassembles system/bin/arm64/linker64 with the NDK's
llvm-objdump and finds that read: the thread pointer's slot 1 (TLS_SLOT_THREAD_ID), then a load from it,
stored to slot -1 (TLS_SLOT_BIONIC_TLS).

usage: digitalis_bionic_layout.py BUNDLE [--expect qpr0|qpr2]   (exit status 1 when it differs)
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

LAYOUTS = {0x2f8: 'qpr0', 0x300: 'qpr2'}


def objdump():
    if os.environ.get('LLVM_OBJDUMP'):  # e.g. an AOSP tree's prebuilts/clang (digitalis_build_qpr2.sh)
        return Path(os.environ['LLVM_OBJDUMP'])
    sdk = Path(os.environ.get('ANDROID_HOME', Path.home() / 'Android/Sdk'))
    found = sorted(sdk.glob('ndk/*/toolchains/llvm/prebuilt/*/bin/llvm-objdump*'))
    if not found:
        sys.exit('llvm-objdump not found: install the Android NDK (ANDROID_HOME/ndk)')
    return found[-1]


def bionic_tls_offsets(linker):
    lines = subprocess.run([str(objdump()), '-d', '--no-show-raw-insn', str(linker)], check=True,
                           capture_output=True, text=True).stdout.splitlines()
    offsets = set()
    for i, line in enumerate(lines):
        tp = re.search(r'mrs\s+(x\d+), TPIDR_EL0', line)
        if not tp:
            continue
        thread = loaded = None
        for following in lines[i + 1:i + 10]:
            slot = re.search(r'ldr\s+(x\d+), \[' + tp[1] + r', #0x8\]$', following)
            if slot:
                thread = slot[1]
                continue
            load = thread and re.search(r'ldr\s+(x\d+), \[' + thread + r', #(0x[0-9a-f]+)\]$', following)
            if load:
                loaded = (load[1], int(load[2], 16))
                continue
            if loaded and re.search(r'stur\s+' + loaded[0] + r', \[' + tp[1] + r', #-0x8\]$', following):
                offsets.add(loaded[1])
                break
    return offsets


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('bundle', type=Path, help='the bundle directory (holding system/)')
    parser.add_argument('--expect', choices=sorted(set(LAYOUTS.values())))
    args = parser.parse_args()
    offsets = bionic_tls_offsets(args.bundle / 'system/bin/arm64/linker64')
    layouts = {LAYOUTS.get(offset, hex(offset)) for offset in offsets}
    if len(layouts) != 1:
        sys.exit(f'cannot tell the layout: bionic_tls read at {sorted(map(hex, offsets)) or "no place found"}')
    layout = layouts.pop()
    print(f'{args.bundle}: {layout} (bionic_tls at {", ".join(map(hex, sorted(offsets)))})')
    if args.expect and layout != args.expect:
        sys.exit(f'expected the {args.expect} layout')


if __name__ == '__main__':
    main()
