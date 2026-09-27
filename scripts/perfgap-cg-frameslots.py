#!/usr/bin/env python3
"""What the extra frame loads/stores ('framemem') are (docs/perf-gap.md, section 4).

For the hottest functions, the llc assembly (asm-verbose, so register
allocator spills carry '# 8-byte Spill' / 'Reload' comments) is aligned
instruction by instruction with the linked binary, and every executed
instruction with an (%rbp)/(%rsp) operand is put in one bucket:
  spill / reload  register-allocator spill code (comment in the .s)
  root            a GC-root slot of goc's stack maps (an %rbp offset listed as
                  a Direct location in this function's llvm.experimental.stackmap
                  records): the volatile goc.spill.root stores/reloads
  local           any other frame access (address-taken locals, structs on
                  the stack, the frame-move guard slot, arguments)
  unmapped        not matched by the alignment
Ir comes from callgrind --dump-instr files (<engine>.<suite>.cg).

(auto: assemble each .s with llvm-mc to read its stack maps; - for native builds)
Usage: perfgap-cg-frameslots.py OUT_JSON CG_DIR ENGINE BINARY auto|- \\
         ASM:FUNC:SYMBOL [ASM:FUNC:SYMBOL ...]
"""
import collections, difflib, importlib.util, json, os, re, subprocess, sys
spec = importlib.util.spec_from_file_location('cls', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'perfgap-cg-classify.py'))
C = importlib.util.module_from_spec(spec); spec.loader.exec_module(C)
SUITES = ['Richards', 'DeltaBlue', 'Crypto', 'RayTrace', 'EarleyBoyer', 'RegExp', 'Splay', 'NavierStokes', 'Parse']
KEEP = ('shl', 'sal', 'sbb', 'sub', 'jl', 'jb', 'setl', 'setb', 'cmovl', 'cmovb', 'ud2', 'cwtl', 'cltq', 'cqto', 'cltd')

def s_func(path, fn):
    out = []; on = False
    for l in open(path):
        if l.startswith(fn + ':'): on = True; continue
        if on:
            if l.startswith('.Lfunc_end'): break
            m = re.match(r'^\t([a-z][a-z0-9]*)\b', l)
            if m:
                out.append((m.group(1), 'spill' if 'Spill' in l else 'reload' if 'Reload' in l else ''))
    return out

def roots(asm, obj):
    """function name -> set of %rbp offsets used as stackmap Direct locations"""
    names = []; txt = open(asm).read()
    i = txt.find('__LLVM_StackMaps:')
    if i < 0 or obj == '-': return {}
    q = re.findall(r'\.quad\s+(\S+)', txt[i:])
    nf = int(re.search(r'__LLVM_StackMaps:\s*\.byte\s+3\s*\.byte\s+0\s*\.short\s+0\s*\.long\s+(\d+)', txt[i:]).group(1))
    for k in range(nf): names.append((q[3 * k], int(q[3 * k + 2])))
    if obj == 'auto':
        obj = asm[:-2] + '.stackmap.o'
        subprocess.run(['llvm-mc-19', '-filetype=obj', asm, '-o', obj], check=True)
    ro = subprocess.run(['llvm-readobj-19', '--stackmap', obj], capture_output=True, text=True).stdout
    recs = re.split(r'\n\s*Record ID:', ro)[1:]
    res = {}; r = 0
    for name, cnt in names:
        s = res.setdefault(name, set())
        for rec in recs[r:r + cnt]:
            for m in re.finditer(r'Direct R#6 \+ (-?\d+)', rec): s.add(int(m.group(1)))
        r += cnt
    return res

def norm_m(x):
    return x if x in KEEP else re.sub(r'(q|l|w|b)$', '', x)

def main():
    out, cgdir, eng, binary, obj = sys.argv[1:6]
    specs = [a.split(':') for a in sys.argv[6:]]
    ins, cls = C.disasm(binary)
    byfn = collections.defaultdict(list)
    for a in sorted(ins): byfn[ins[a][0]].append(a)
    cost = collections.Counter(); real = os.path.realpath(binary)
    for s in SUITES:
        f = os.path.join(cgdir, '%s.%s.cg' % (eng, s))
        if not os.path.exists(f): continue
        c, _ = C.parse_cg(f)
        for (ob, a), ir in c.items():
            if ob and os.path.realpath(ob) == real: cost[a] += ir
    total = 0
    for s in SUITES:
        f = os.path.join(cgdir, '%s.%s.cg' % (eng, s))
        if os.path.exists(f):
            total += sum(C.parse_cg(f)[0].values())
    R = {}
    rootcache = {}
    for asm, fn, sym in specs:
        if asm not in rootcache: rootcache[asm] = roots(asm, obj)
        rs = rootcache[asm].get(fn, set())
        s = s_func(asm, fn)
        o = [(a, ins[a][1]) for a in byfn.get(sym, [])]
        o = [x for x in o if not re.match(r'(nop|data16|cs nop|xchg\s+%ax,%ax)', x[1])]
        sm = difflib.SequenceMatcher(None, [norm_m(x[0]) for x in s], [norm_m(x[1].split()[0]) for x in o], autojunk=False)
        amap = {}
        for bl in sm.get_matching_blocks():
            for k in range(bl.size): amap[o[bl.b + k][0]] = s[bl.a + k][1]
        B = collections.Counter(); ex = collections.defaultdict(list)
        for a, t in o:
            if cls.get(a) != 'framemem': continue
            ir = cost.get(a, 0)
            if a not in amap: b = 'unmapped'
            elif amap[a]: b = amap[a]
            else:
                m = re.search(r'(-?0x[0-9a-f]+)\(%rbp\)', t)
                b = 'root' if m and int(m.group(1), 16) in rs else 'local'
            B[b] += ir
            if ir: ex[b].append((ir, hex(a), t))
        R[fn] = {'buckets_pct_of_total_ir': {k: round(v / total * 100, 3) for k, v in B.items()},
                 'matched': sum(bl.size for bl in sm.get_matching_blocks()), 'asm_ins': len(s), 'bin_ins': len(o),
                 'root_offsets': sorted(rs),
                 'top': {k: sorted(v, reverse=True)[:6] for k, v in ex.items()}}
        print(eng, fn, R[fn]['matched'], len(s), len(o), R[fn]['buckets_pct_of_total_ir'], flush=True)
    old = json.load(open(out)) if os.path.exists(out) else {}
    old[eng] = {'total_ir': total, 'functions': R}
    json.dump(old, open(out, 'w'), indent=1)
main()
