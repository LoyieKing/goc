#!/usr/bin/env python3
"""Split the 'framemem' Ir delta (instructions with an (%rbp)/(%rsp) operand,
engine code only) between two builds by kind and by function
(docs/perf-gap.md, section 4).

Kinds: lea    address of a frame slot (no memory access; goc-reanchor
              re-derives stack addresses from %rbp after calls)
       load   mov/movz/movs from a frame slot into a register
       store  mov to a frame slot
       rmw    arithmetic/cmp with a frame operand
Position: 'after call' = within 4 instructions after a call in the same
function, 'before call' = within 4 before one, else 'other'.

Usage: perfgap-cg-framemem.py CG_DIR OUT_JSON A=binA B=binB [SUITES...]
"""
import collections, importlib.util, json, os, re, sys
spec = importlib.util.spec_from_file_location('cls', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'perfgap-cg-classify.py'))
C = importlib.util.module_from_spec(spec); spec.loader.exec_module(C)

def kind(t):
    if t.startswith('lea'): return 'lea'
    m = re.match(r'(\w+)\s+(.*)$', t)
    op, args = m.group(1), m.group(2)
    if op.startswith('mov') or op.startswith('vmov'):
        dst = args.rsplit(',', 1)[-1]
        return 'store' if re.search(r'\(%r[bs]p\)', dst) else 'load'
    return 'rmw'

def analyse(cgdir, eng, binary, suites):
    ins, cls = C.disasm(binary)
    order = sorted(ins); pos = {}
    for i, a in enumerate(order):
        if cls.get(a) != 'framemem': continue
        f = ins[a][0]; p = 'other'
        for j in range(1, 5):
            if i - j >= 0 and ins[order[i - j]][0] == f and ins[order[i - j]][1].startswith('call'): p = 'after call'; break
        else:
            for j in range(1, 5):
                if i + j < len(order) and ins[order[i + j]][0] == f and ins[order[i + j]][1].startswith('call'): p = 'before call'; break
        pos[a] = p
    real = os.path.realpath(binary)
    tot = 0; K = collections.Counter(); P = collections.Counter(); F = collections.Counter(); KP = collections.Counter()
    for s in suites:
        cost, meta = C.parse_cg(os.path.join(cgdir, '%s.%s.cg' % (eng, s)))
        tot += sum(cost.values())
        for (ob, a), ir in cost.items():
            if not (ob and os.path.realpath(ob) == real and a in pos): continue
            if C.group(ins[a][0]) != 'engine': continue
            k = kind(ins[a][1]); K[k] += ir; P[pos[a]] += ir; KP[k + '/' + pos[a]] += ir
            F[C.norm(ins[a][0])] += ir
    return tot, K, P, KP, F

def main():
    cgdir, out = sys.argv[1], sys.argv[2]
    (a, ba), (b, bb) = [x.split('=', 1) for x in sys.argv[3:5]]
    suites = sys.argv[5:] or ['Richards', 'DeltaBlue', 'Crypto', 'RayTrace', 'EarleyBoyer', 'RegExp', 'Splay', 'NavierStokes', 'Parse']
    ta, Ka, Pa, KPa, Fa = analyse(cgdir, a, ba, suites)
    tb, Kb, Pb, KPb, Fb = analyse(cgdir, b, bb, suites)
    pct = lambda x, y: round((x - y) / tb * 100, 3)
    res = {'a': a, 'b': b, 'suites': suites, 'ir_a': ta, 'ir_b': tb,
           'framemem_delta': pct(sum(Ka.values()), sum(Kb.values())),
           'kind': {k: pct(Ka[k], Kb[k]) for k in sorted(set(Ka) | set(Kb))},
           'position': {k: pct(Pa[k], Pb[k]) for k in sorted(set(Pa) | set(Pb))},
           'kind_position': {k: pct(KPa[k], KPb[k]) for k in sorted(set(KPa) | set(KPb))},
           'functions': sorted(((f, pct(Fa[f], Fb[f])) for f in set(Fa) | set(Fb)), key=lambda x: -abs(x[1]))[:15]}
    old = json.load(open(out)) if os.path.exists(out) else {}
    old['%s|%s|%s' % (a, b, ','.join(suites))] = res
    json.dump(old, open(out, 'w'), indent=1)
    print(json.dumps(res, indent=1))
main()
