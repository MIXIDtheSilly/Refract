"""Skip unnecessary SIMD memory-address copies when TBI is disabled.

Run inside WSL: python3 /mnt/c/<path-to>/Refract/tools/translator/optimize_lite_simd_addresses.py apply|restore
"""
from pathlib import Path
import sys

source = Path('/root/aosp/frameworks/libs/binary_translation/lite_translator/arm64_to_x86_64/lite_translator_simd_fp_misc.inc')
backup = Path(__file__).resolve().parent / 'backups' / 'simd-address-before.inc'
old_imm = '''    // apply TBI mask before using base as memory operand.
    base = ApplyTbi(base);
    // Handle 128-bit SIMD load/store with immediate offset'''
new_imm = '''    // This helper only reads base. Avoid a register copy when TBI is disabled.
    if (IsConfigFlagSet(kTopByteIgnore)) {
      base = ApplyTbi(base);
    }
    // Handle 128-bit SIMD load/store with immediate offset'''
old_pair = '''    // apply TBI mask before using addr as memory operand.
    addr = ApplyTbi(addr);
    // Handle 128-bit pair store/load'''
new_pair = '''    // This helper only reads addr. Avoid a register copy when TBI is disabled.
    if (IsConfigFlagSet(kTopByteIgnore)) {
      addr = ApplyTbi(addr);
    }
    // Handle 128-bit pair store/load'''

mode = sys.argv[1]
current = source.read_text()
if mode == 'apply':
    if backup.exists() or current.count(old_imm) != 1 or current.count(old_pair) != 1:
        raise SystemExit('Unexpected source or backup already exists')
    backup.write_text(current)
    source.write_text(current.replace(old_imm, new_imm).replace(old_pair, new_pair))
elif mode == 'restore':
    if not backup.exists():
        raise SystemExit('Backup missing')
    original = backup.read_text()
    if current != original.replace(old_imm, new_imm).replace(old_pair, new_pair):
        raise SystemExit('Source changed since applying; refusing to restore')
    source.write_text(original)
    backup.unlink()
else:
    raise SystemExit('usage: optimize_lite_simd_addresses.py apply|restore')
