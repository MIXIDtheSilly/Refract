"""Make a multi-core copy of the Android Emulator's qemu (emulator 37.1.11 only).

On this Intel + Windows Hypervisor Platform PC the emulator decides that "not all
modern X86 virtualization features" are present and silently runs Android on a
single vCPU, whatever the AVD asks for. This copies qemu-system-x86_64.exe to
qemu-system-x86_64-multicore.exe and turns that one check's jump into NOPs, so
the AVD's hw.cpu.ncore (capped at 6 by the emulator) is used. The original
file is not modified; scripts/start_emulator.ps1 -Cores N starts the copy.

Run:  python tools\\patch_emulator_cores.py
"""
import hashlib
import sys
from pathlib import Path

import pefile

QEMU_DIR = Path(r'C:\Users\mixid\Android\Sdk\emulator\qemu\windows-x86_64')
SOURCE = QEMU_DIR / 'qemu-system-x86_64.exe'
TARGET = QEMU_DIR / 'qemu-system-x86_64-multicore.exe'
# In emulator 37.1.11 (build 15917651):
#   call hasModernX86VirtualizationFeatures; test al, al; je <1-vCPU fallback>
CHECK_RVA = 0x17a40c
EXPECTED = bytes.fromhex('e85f3f3a00' '84c0' '7466')


def main():
    pe = pefile.PE(str(SOURCE), fast_load=True)
    text = next(s for s in pe.sections if s.Name.startswith(b'.text'))
    offset = CHECK_RVA - text.VirtualAddress + text.PointerToRawData
    data = bytearray(SOURCE.read_bytes())
    found = bytes(data[offset:offset + len(EXPECTED)])
    if found != EXPECTED:
        sys.exit(f'Unexpected bytes {found.hex()} (different emulator version?); nothing written.')
    data[offset + 7:offset + 9] = b'\x90\x90'  # je -> nop nop: always take the "modern features" path.
    TARGET.write_bytes(data)
    print(f'wrote {TARGET} (sha256 {hashlib.sha256(data).hexdigest()[:16]}...)')


if __name__ == '__main__':
    main()
