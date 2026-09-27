"""Apply or restore a focused Berberis lite load/store address-copy change."""
from pathlib import Path
import sys

source = Path('/root/aosp/frameworks/libs/binary_translation/lite_translator/arm64_to_x86_64/lite_translator_integer_ctrl.inc')
backup = Path(__file__).resolve().parent / 'backups' / 'lite-load-store-before.inc'
old = '''    // apply TBI mask before using base as memory operand.
    base = ApplyTbi(base);
    AssemblerBase::Label* recovery_label = as_.MakeLabel();'''
new = '''    // This helper only reads base. Without TBI masking, use it directly and
    // avoid one register copy for every ordinary integer load or store.
    if (IsConfigFlagSet(kTopByteIgnore)) {
      base = ApplyTbi(base);
    }
    AssemblerBase::Label* recovery_label = as_.MakeLabel();'''

mode = sys.argv[1]
current = source.read_text()
if mode == 'apply':
    if backup.exists() or current.count(old) != 2:
        raise SystemExit('Unexpected source or backup already exists')
    backup.write_text(current)
    source.write_text(current.replace(old, new))
elif mode == 'restore':
    if not backup.exists():
        raise SystemExit('Backup missing')
    original = backup.read_text()
    if original.count(old) != 2 or current != original.replace(old, new):
        raise SystemExit('Source changed since applying; refusing to restore')
    source.write_text(original)
    backup.unlink()
else:
    raise SystemExit('usage: optimize_lite_load_store.py apply|restore')
