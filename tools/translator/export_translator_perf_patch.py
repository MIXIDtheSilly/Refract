"""Export this session's two AOSP translator edits as a reviewable patch."""
from difflib import unified_diff
from pathlib import Path

here = Path(__file__).resolve().parent
aosp = Path('/root/aosp/frameworks/libs/binary_translation')
files = [
    (here / 'backups/heavy-tbi-probe.frontend.h',
     aosp / 'heavy_optimizer/arm64/frontend.h',
     'heavy_optimizer/arm64/frontend.h'),
    (here / 'backups/lite-load-store-before.inc',
     aosp / 'lite_translator/arm64_to_x86_64/lite_translator_integer_ctrl.inc',
     'lite_translator/arm64_to_x86_64/lite_translator_integer_ctrl.inc'),
]
patch = []
for before, after, name in files:
    if not before.exists() or not after.exists():
        raise SystemExit(f'Missing source or backup: {name}')
    patch.extend(unified_diff(before.read_text().splitlines(keepends=True),
                              after.read_text().splitlines(keepends=True),
                              fromfile=f'a/{name}', tofile=f'b/{name}'))
output = here / 'digitalis-performance-fix.patch'
output.write_text(''.join(patch))
print(f'{output}: {len(patch)} lines')
