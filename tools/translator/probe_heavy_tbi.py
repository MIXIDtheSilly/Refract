"""Temporarily isolate the heavy optimizer's optional TBI removal in WSL AOSP."""
from pathlib import Path
import sys

source = Path('/root/aosp/frameworks/libs/binary_translation/heavy_optimizer/arm64/frontend.h')
backup = Path(__file__).resolve().parent / 'backups' / 'heavy-tbi-probe.frontend.h'
optimized = '''  [[nodiscard]] Register ApplyTbi(Register base) {
    if (!IsConfigFlagSet(kTopByteIgnore)) {
      Register tbi = AllocTempReg();
      if (success()) {
        builder_.Gen<x86_64::MovqRegReg>(tbi, base);
      }
      return tbi;
    }
    Register tbi = Copy(base);
    tbi = std::get<0>(Gen<x86_64::ShlqRegImm, kNoSSA>(tbi, int8_t{8}));
    tbi = std::get<0>(Gen<x86_64::ShrqRegImm, kNoSSA>(tbi, int8_t{8}));
    return tbi;
  }'''
masked = '''  [[nodiscard]] Register ApplyTbi(Register base) {
    Register tbi = Copy(base);
    tbi = std::get<0>(Gen<x86_64::ShlqRegImm, kNoSSA>(tbi, int8_t{8}));
    tbi = std::get<0>(Gen<x86_64::ShrqRegImm, kNoSSA>(tbi, int8_t{8}));
    return tbi;
  }'''
old_comment = '''  // ARM64 TBI (Top Byte Ignore): clear the top 8 bits of an address register
  // before using it as a host x86 memory operand. ARM64 ignores the top byte of
  // pointers in load/store; x86 does not, so we mask it ourselves. Returns a
  // fresh register holding (base & 0x00FF'FFFF'FFFF'FFFF). Mirrors
  // lite_translator.h::ApplyTbi (movq; shlq 8; shrq 8).
  // region digitalis: masking only with ro.berberis.flags=top-byte-ignore (see
  // lite_translator's ApplyTbi); still a copy, since callers may modify it.
  // Without masking the copy is still a temp (callers keep it live across the
  // blocks of their retry loops) but filled by a real GP move, not a PseudoCopy:
  // the shifts used to pin the register class, and a bare PseudoCopy leaves the
  // uses with no common class (lifetime.h reg_class_ CHECK).
'''
new_comment = '''  // ARM64 TBI: heavy-tier memory addresses must clear their top byte.
  // Removing the shifts broke register lifetime analysis for LDADD and crashed
  // North Star while compiling the atomic helper in libc. Keep this path
  // correct for tagged pointers and optimize it only with an allocator-safe
  // lowering backed by a multi-instruction regression test.
'''

mode = sys.argv[1]
current = source.read_text()
if mode == 'mask':
    if current.count(optimized) != 1:
        raise SystemExit('Expected optimized block is absent or ambiguous')
    if backup.exists():
        raise SystemExit('Backup already exists; restore first')
    backup.write_text(current)
    source.write_text(current.replace(optimized, masked))
elif mode == 'restore':
    if not backup.exists():
        raise SystemExit('Probe block or backup missing')
    original = backup.read_text()
    if original.count(optimized) != 1:
        raise SystemExit('Backup is unexpected')
    probe = original.replace(optimized, masked)
    final = original.replace(old_comment + optimized, new_comment + masked)
    if current not in (probe, final):
        raise SystemExit('Source changed since probe; refusing to restore')
    source.write_text(original)
    backup.unlink()
elif mode == 'finalize':
    if not backup.exists():
        raise SystemExit('Backup missing')
    original = backup.read_text()
    if original.count(old_comment + optimized) != 1:
        raise SystemExit('Backup is unexpected')
    if current != original.replace(optimized, masked):
        raise SystemExit('Source changed since probe; refusing to finalize')
    source.write_text(original.replace(old_comment + optimized, new_comment + masked))
else:
    raise SystemExit('usage: probe_heavy_tbi.py mask|finalize|restore')
