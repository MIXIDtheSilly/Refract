"""Estimates where translated-code time goes, by instruction category, over all sampled regions.

Each region's samples are spread over its hot-path instructions (inline fault stubs, which the
normal path jumps over, are excluded). Categories are translator overheads we could remove:
  tbi        shl/shr 8 pairs that strip the arm64 pointer tag before each memory access
  stub-skip  the taken jmp over an inline accurate-sigsegv stub
  mov-rr     register-to-register copies
  state      loads/stores of guest registers in ThreadState ([rbp+disp])
  flags      NZCV materialization (lahf/seto/sahf and friends)
  fp-zero    pxor+movss/movsd pairs that zero the upper lanes after scalar FP ops
  dispatch   region-exit dispatch (pending-signal check, table lookup, indirect jmp)
Uses the same inputs as region_annotate.py.

usage: region_overhead.py <dir> [--exit-addr HEX]
"""
import argparse, bisect, collections, os, re

import capstone

ap = argparse.ArgumentParser()
ap.add_argument('dir')
ap.add_argument('--exit-addr', help='kEntryExitGeneratedCode address (auto-detected if omitted)')
args = ap.parse_args()
d = args.dir

samples = collections.Counter()
for m in re.finditer(r'\s+\d+\.\d+%\s+(\d+)\s+0x([0-9a-f]+)', open(f'{d}/um_ips.txt').read()):
    samples[int(m.group(2), 16)] += int(m.group(1))

pat = re.compile(r'^(.*)_(lite|heavy|[a-z]+)_0x([0-9a-f]+)\+(\d+)')
regions = []
for n, line in enumerate(open(f'{d}/um_perf.map')):
    parts = line.split(None, 2)
    if len(parts) == 3 and (m := pat.match(parts[2].strip())):
        start = int(parts[0], 16)
        regions.append((start, start + int(parts[1], 16), n, m.group(2)))
regions.sort()
starts = [r[0] for r in regions]


def region_of(ip):
    i = bisect.bisect_right(starts, ip) - 1
    best = None
    for j in range(i, max(i - 64, -1), -1):
        r = regions[j]
        if r[0] <= ip < r[1] and (best is None or r[2] > best[2]):
            best = r
    return best


chunks = sorted((int(n.split('.')[0], 16), open(f'{d}/code/{n}', 'rb').read()) for n in os.listdir(f'{d}/code'))


def host_bytes(start, end):
    for base, data in chunks:
        if base <= start and end <= base + len(data):
            return data[start - base:end - base]


per_region = collections.Counter()
for ip, n in samples.items():
    if (r := region_of(ip)):
        per_region[r] += n
total = sum(samples.values())

x86 = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
x86.detail = False

# The exit trampoline is the most common absolute jmp target in lite code.
exit_addr = int(args.exit_addr, 16) if args.exit_addr else None
if exit_addr is None:
    targets = collections.Counter()
    for r, _ in per_region.most_common(50):
        for insn in x86.disasm(host_bytes(r[0], r[1]) or b'', r[0]):
            if insn.mnemonic == 'jmp' and insn.op_str.startswith('0x'):
                t = int(insn.op_str, 16)
                if not r[0] <= t < r[1]:
                    targets[t] += 1
    exit_addr = targets.most_common(1)[0][0]
print(f'exit trampoline 0x{exit_addr:x}')

REG = r'(?:r\w+|e\w+|[a-d]x|[sd]il?|[sb]pl?)'
weights = {gear: collections.Counter() for gear in ('lite', 'heavy')}
region_samples = collections.Counter()
for r, n in per_region.items():
    start, end, _, gear = r
    if gear not in weights:
        continue
    data = host_bytes(start, end)
    if data is None:
        continue
    insns = list(x86.disasm(data, start))
    by_addr = {i.address: k for k, i in enumerate(insns)}
    cold = set()
    for k, insn in enumerate(insns):
        if insn.mnemonic == 'jmp' and insn.op_str.startswith('0x'):
            t = int(insn.op_str, 16)
            if start < t < end and t > insn.address and t in by_addr:
                body = insns[k + 1:by_addr[t]]
                if body and body[-1].mnemonic == 'jmp' and body[-1].op_str == hex(exit_addr):
                    cold.update(i.address for i in body)
    hot = [i for i in insns if i.address not in cold]
    cats = []
    for k, i in enumerate(hot):
        m, o = i.mnemonic, i.op_str
        nxt = hot[k + 1] if k + 1 < len(hot) else None
        prv = hot[k - 1] if k else None
        if (m == 'shl' and o.endswith(', 8') and nxt and nxt.mnemonic == 'shr' and nxt.op_str == o) or \
           (m == 'shr' and o.endswith(', 8') and prv and prv.mnemonic == 'shl' and prv.op_str == o):
            c = 'tbi'
        elif m == 'jmp' and o.startswith('0x') and start < int(o, 16) < end and \
                any(a in cold for a in range(i.address + i.size, i.address + i.size + 16)):
            c = 'stub-skip'
        elif m in ('lahf', 'sahf', 'seto', 'setno') or (m == 'and' and '0xc101' in o):
            c = 'flags'
        elif m == 'pxor' and nxt and nxt.mnemonic in ('movss', 'movsd'):
            c = 'fp-zero'
        elif m in ('movss', 'movsd') and prv and prv.mnemonic == 'pxor':
            c = 'fp-zero'
        elif 'rbp + 0x360' in o or (m == 'jmp' and re.fullmatch(REG, o)) or \
                (m == 'jmp' and o == hex(exit_addr)):
            c = 'dispatch'
        elif re.search(r'\[rbp [+-] 0x[0-9a-f]+\]', o) and m.startswith(('mov', 'vmov')):
            c = 'state'
        elif m in ('mov', 'movq') and re.fullmatch(f'{REG}, {REG}', o) and 'rsp' not in o:
            c = 'mov-rr'
        else:
            c = 'work'
        cats.append(c)
    counts = collections.Counter(cats)
    for c, k in counts.items():
        weights[gear][c] += n * k / len(cats)
    region_samples[gear] += n

translated = sum(region_samples.values())
print(f'{translated} of {total} samples in lite/heavy regions '
      f'(lite {region_samples["lite"]}, heavy {region_samples["heavy"]})')
for gear, w in weights.items():
    tot = sum(w.values())
    print(f'\n{gear}: share of that tier\'s time by instruction category (instruction-count estimate)')
    for c, v in sorted(w.items(), key=lambda kv: -kv[1]):
        print(f'  {c:10s} {v / tot:6.1%}')
