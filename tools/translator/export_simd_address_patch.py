"""Export the experimental SIMD address-copy change from the WSL AOSP tree."""
from difflib import unified_diff
from pathlib import Path

here = Path(__file__).resolve().parent
name = 'lite_translator/arm64_to_x86_64/lite_translator_simd_fp_misc.inc'
before = (here / 'backups/simd-address-before.inc').read_text().splitlines(keepends=True)
after = (Path('/root/aosp/frameworks/libs/binary_translation') / name).read_text().splitlines(keepends=True)
patch = ''.join(unified_diff(before, after, fromfile=f'a/{name}', tofile=f'b/{name}'))
(here / 'digitalis-simd-address-candidate.patch').write_text(patch)
print(f'Exported {len(patch.splitlines())} patch lines')
