#!/usr/bin/env python3
"""Compare one function in two builds by execution count (docs/perf-gap.md,
section 5). Every executed instruction's Ir in a callgrind --dump-instr file is
its execution count; instructions of the same basic block share a count, and
the same source block has the same count in both builds (same bytecode run).
Grouping by count therefore pairs the builds' versions of each hot block
without symbol or line information; the Ir delta of a group is
count x (instructions in A - instructions in B).

Usage: perfgap-cg-bycount.py CG_A BIN_A FUNC_A CG_B BIN_B FUNC_B [N] [--list]
"""
import collections, importlib.util, os, sys
spec = importlib.util.spec_from_file_location('cls', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'perfgap-cg-classify.py'))
C = importlib.util.module_from_spec(spec); spec.loader.exec_module(C)

def load(cg, binary, fn):
    cost, _ = C.parse_cg(cg); ins, cls = C.disasm(binary); real = os.path.realpath(binary)
    g = collections.defaultdict(list)
    for (ob, a), ir in cost.items():
        if ob and os.path.realpath(ob) == real and a in ins and fn in ins[a][0]:
            g[ir].append((a, cls.get(a), ins[a][1]))
    return g

args = [a for a in sys.argv[1:] if a != '--list']; lst = '--list' in sys.argv
A = load(*args[0:3]); B = load(*args[3:6]); n = int(args[6]) if len(args) > 6 else 20
ta = sum(k * len(v) for k, v in A.items()); tb = sum(k * len(v) for k, v in B.items())
print('Ir A %d  B %d  delta %+d (%+.1f%% of B)' % (ta, tb, ta - tb, (ta - tb) / tb * 100))
rows = sorted(((k * (len(A.get(k, [])) - len(B.get(k, []))), k) for k in set(A) | set(B)), reverse=True)
cum = 0
for d, k in rows[:n]:
    cum += d
    ca = collections.Counter(c for _, c, _ in A.get(k, []))
    print('count=%10d A=%3d B=%3d delta=%8.1fM cum=%5.1f%% of B  A classes %s' % (k, len(A.get(k, [])), len(B.get(k, [])), d / 1e6, cum / tb * 100, dict(ca)))
    if lst:
        for a, c, t in sorted(A.get(k, [])): print('   A %x %-10s %s' % (a, c, t))
        for a, c, t in sorted(B.get(k, [])): print('   B %x %-10s %s' % (a, c, t))
