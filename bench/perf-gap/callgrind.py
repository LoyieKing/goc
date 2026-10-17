#!/usr/bin/env python3
"""callgrind analyses for the perf-gap study (frozen research code).

  callgrind.py summarize [CG_DIR] [OUT_JSON]
  callgrind.py classify CG_DIR OUT_JSON engine=binary ...
  callgrind.py decompose [CLASSES_JSON] [OUT_JSON]
  callgrind.py bycount CG_A BIN_A FUNC_A CG_B BIN_B FUNC_B [N] [--list]
  callgrind.py framemem CG_DIR OUT_JSON A=binA B=binB [SUITES...]
  callgrind.py frameslots OUT_JSON CG_DIR ENGINE BINARY auto|- ASM:FUNC:SYMBOL ...

Inputs are callgrind files <engine>.<suite>.cg from perf-gap/callgrind.sh
(classify, bycount, framemem and frameslots need --dump-instr=yes).
`callgrind.py CMD -h` prints the details of one subcommand.
"""
import collections, difflib, glob, json, os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


# ---- classify: instruction classes and function groups ----
CLASSIFY_DOC = r"""Instruction-level decomposition of callgrind Ir (docs/research/perf-gap.md, "where
the extra instructions go").

Input: callgrind files recorded with --dump-instr=yes (scripts in the doc,
"reproduce"), named <engine>.<suite>.cg.  Every executed instruction address is
mapped back to the binary with objdump, so attribution does not depend on
callgrind's function tracking (which gets confused by Go stack switches on goc
binaries and books C code under runtime.main).

Function groups: engine (the C code: quickjs, libregexp, ...), alloc, memstr,
math, time, alloca (goc alloca pool), goruntime (Go runtime + cgo bridge),
other (ld.so, rest of libc).

Instruction classes inside engine code:
  stackcheck  goc morestack prologue: MOVQ FS:-8,R11; LEAQ -N(SP),R10; CMPQ; JBE
  morestack   the per-function stub that saves all registers and calls
              runtime.morestack (only runs when the goroutine stack grows)
  guard       frame-move guard around calls: MOVQ BP,-N(BP) / CMPQ BP,-N(BP) / JNE
  fixup       the JNE targets that re-base spilled stack addresses
  tls         g-based code: other FS:-8 loads, the dependent g.stack.lo/hi
              loads, and the rest of an inlined uptr encode/decode (range
              test up to the sign test of the offset); also Bellard's
              goc_stack_hi() stack-overflow check
  got         loads of global addresses through goc's GOT (goc.got.*)
  pushpop     push/pop (callee-saved registers)
  framemem    other instructions with an (%rbp)/(%rsp) memory operand
              (spills and reloads, but also plain locals such as the
              JSStackFrame fields; only a rough register-pressure proxy)
  call        call instructions
  rest        everything else

Usage: callgrind.py classify CG_DIR OUT_JSON engine=binary ..."""

def parse_cg(f):
    names = {}; obs = {}; fn = None; ob = None; addr = 0; skip = False
    cost = collections.Counter(); meta = {}
    for l in open(f):
        c0 = l[0]
        if c0.isdigit() or c0 in '+-*':
            parts = l.split()
            a = parts[0]
            if a.startswith('0x'): addr = int(a, 16)
            elif a[0] in '+-': addr += int(a)
            if skip: skip = False; continue
            if len(parts) >= 3:
                cost[(ob, addr)] += int(parts[2])
                meta[(ob, addr)] = fn
            continue
        if l.startswith(('fn=', 'cfn=')):
            m = re.match(r'c?fn=\((\d+)\)(?: (.*))?', l.rstrip('\n'))
            if m.group(2): names[m.group(1)] = m.group(2)
            if c0 == 'f': fn = names[m.group(1)]
        elif l.startswith(('ob=', 'cob=')):
            m = re.match(r'c?ob=\((\d+)\)(?: (.*))?', l.rstrip('\n'))
            if m.group(2): obs[m.group(1)] = m.group(2)
            if c0 == 'o': ob = obs[m.group(1)]
        elif l.startswith('calls='):
            skip = True
    return cost, meta

_dis = {}
def disasm(binary):
    if binary in _dis: return _dis[binary]
    out = subprocess.run(['objdump', '-d', '--no-show-raw-insn', '-w', binary],
                         capture_output=True, text=True).stdout
    ins = {}; fnstart = {}; cur = None; order = []
    for l in out.splitlines():
        m = re.match(r'^([0-9a-f]+) <(.*)>:$', l)
        if m:
            cur = m.group(2); fnstart[cur] = int(m.group(1), 16); continue
        m = re.match(r'^\s+([0-9a-f]+):\t(.*)$', l)
        if m and cur:
            a = int(m.group(1), 16)
            ins[a] = (cur, m.group(2).strip())
            order.append(a)
    # classify static instruction classes
    cls = {}
    idx = {a: i for i, a in enumerate(order)}
    fixup_targets = set(); stub_targets = set()
    for i, a in enumerate(order):
        fname, t = ins[a]
        if a in cls: continue
        # stack check prologue
        if a == fnstart.get(fname) and t.startswith('mov') and '%fs:0xfffffffffffffff8,%r11' in t:
            j = i
            while j < len(order) and j < i + 4:
                tt = ins[order[j]][1]
                cls[order[j]] = 'stackcheck'
                if tt.startswith('jbe') or tt.startswith('jb '):
                    m = re.search(r'jbe?\s+([0-9a-f]+)', tt)
                    if m: stub_targets.add(int(m.group(1), 16))
                    break
                j += 1
            continue
        m = re.match(r'mov\s+%rbp,(-0x[0-9a-f]+)\(%rbp\)$', t)
        if m and i + 1 < len(order) and ins[order[i + 1]][1].startswith('call'):
            off = m.group(1)
            cls[a] = 'guard'
            k = i + 2
            if k < len(order) and re.match(r'cmp\s+%rbp,' + off + r'\(%rbp\)$', ins[order[k]][1]):
                cls[order[k]] = 'guard'
                if k + 1 < len(order) and ins[order[k + 1]][1].startswith('jne'):
                    cls[order[k + 1]] = 'guard'
                    mm = re.search(r'jne\s+([0-9a-f]+)', ins[order[k + 1]][1])
                    if mm: fixup_targets.add(int(mm.group(1), 16))
            continue
    for targets, name in ((fixup_targets, 'fixup'), (stub_targets, 'morestack')):
        for t0 in targets:
            if t0 not in idx: continue
            i = idx[t0]
            while i < len(order):
                a = order[i]; t = ins[a][1]
                cls.setdefault(a, name)
                if t.startswith('jmp') or t.startswith('ud2'): break
                i += 1
    prev_fs = None; tail = 0; pending = []
    for a in order:
        if a in cls: prev_fs = None; tail = 0; pending = []; continue
        fname, t = ins[a]
        if '%fs:' in t:
            cls[a] = 'tls'; tail = 12
            for p in pending: cls[p] = 'tls'
            pending = []
            m = re.search(r',(%r\w+)$', t); prev_fs = m.group(1) if m else None
            continue
        if prev_fs and ('(' + prev_fs + ')') in t:
            cls[a] = 'tls'; prev_fs = None; continue
        prev_fs = None
        if tail:
            # the rest of an inlined uptr encode/decode: range test against
            # g.stack.lo/hi, ending in the sign test of the offset (js/jns)
            tail -= 1
            if re.match(r'j(s|ns)\s', t):
                cls[a] = 'tls'
                for p in pending: cls[p] = 'tls'
                pending = []; tail = 0; continue
            if re.match(r'(jmp|call|ret)', t) or tail == 0:
                pending = []; tail = 0
            else:
                pending.append(a)
        if a in cls: continue
        if 'goc.got.' in t: cls[a] = 'got'
        elif re.match(r'(push|pop)\s', t): cls[a] = 'pushpop'
        elif t.startswith('call'): cls[a] = 'call'
        elif re.search(r'\(%r[bs]p\)', t): cls[a] = 'framemem'
        else: cls[a] = 'rest'
    _dis[binary] = (ins, cls)
    return _dis[binary]

GROUPS = [
    ('alloca', r'goc_alloca'),
    ('alloc', r'(^|[._])(malloc|free|realloc|calloc|cfree|_int_malloc|_int_free|_int_realloc|malloc_consolidate|unlink_chunk|tcache|goc_malloc|goc_free|goc_realloc|malloc_usable_size|sysmalloc|__libc_malloc|__libc_free|__libc_realloc|__libc_calloc)\b'),
    ('memstr', r'(mem(cpy|move|set|cmp|chr)|str(len|cmp|chr|rchr|ncmp|nlen)|wmem)'),
    ('math', r'(gocGoMath|_Cfunc_|(^|[._])(floor|ceil|trunc|round|lrint|sqrt|sin|cos|tan|asin|acos|atan|atan2|sinh|cosh|tanh|asinh|acosh|atanh|exp|expm1|log|log2|log10|log1p|pow|fmod|hypot|cbrt|fabs|modf|__sin_fma|__cos_fma|__ieee754\w*|__pow\w*|__exp\w*|__log\w*|__fmod\w*|__atan\w*|__tan\w*)\b)'),
    ('time', r'(clock_gettime|gettimeofday|localtime|gocGoLocaltime|__vdso|tzset|__tz|mktime|time\b)'),
    ('goruntime', r'^(runtime\.|_cgo|x_cgo|crosscall|cgo|sync\.|internal/|time\.|syscall\.|aeshash|gosave|setg|gogo|main\.gocGo|main\._Cfunc|main\.main)'),
]
def group(name):
    if name is None: return 'other'
    n = re.sub(r'\.impl$', '', name)
    for g, rx in GROUPS:
        if re.search(rx, n): return g
    return 'engine'

def norm(fn):
    fn = re.sub(r"^main\.", "", fn or '?')
    fn = re.sub(r"^[a-z_]+_c\.", "", fn)
    fn = re.sub(r"\.(impl|isra|constprop|part|cold|llvm)(\.\d+)*", "", fn)
    fn = re.sub(r"^goc_", "", fn)
    fn = re.sub(r"'\d+$", "", fn)
    return fn

def classify(cgfile, binary):
    cost, meta = parse_cg(cgfile)
    ins, cls = disasm(binary)
    real = os.path.realpath(binary)
    groups = collections.Counter(); classes = collections.Counter()
    funcs = collections.Counter(); fclass = collections.defaultdict(collections.Counter)
    calls = 0; ops = 0; ci = 0
    for (ob, a), ir in cost.items():
        if ob and os.path.realpath(ob) == real and a in ins:
            fname = ins[a][0]
            g = group(fname)
            c = cls.get(a, 'rest')
            if c == 'call': calls += ir
            if 'JS_CallInternal' in fname:
                ci += ir
                if ins[a][1].startswith('jmp') and '*' in ins[a][1]: ops += ir
        else:
            fname = meta.get((ob, a)) or '?'
            g = group(fname)
            if g == 'engine': g = 'other'
            c = 'rest'
        groups[g] += ir
        nf = norm(fname)
        funcs[nf] += ir
        if g == 'engine':
            classes[c] += ir
            fclass[nf][c] += ir
    top = [(f, v, dict(fclass.get(f, {}))) for f, v in funcs.most_common(40)]
    return {'total': sum(cost.values()), 'groups': dict(groups), 'classes': dict(classes),
            'calls': calls, 'dispatch_ijmp': ops, 'callinternal_ir': ci, 'top': top}

def classify_main(argv):
    cgdir, out = argv[0], argv[1]
    bins = dict(a.split('=', 1) for a in argv[2:])
    res = {}
    for f in sorted(os.listdir(cgdir)):
        if not f.endswith('.cg') or os.path.getsize(os.path.join(cgdir, f)) == 0: continue
        e, s = f[:-3].rsplit('.', 1)
        if e not in bins: continue
        res.setdefault(e, {})[s] = classify(os.path.join(cgdir, f), bins[e])
        r = res[e][s]
        print(e, s, r['total'], {k: round(v / r['total'] * 100, 1) for k, v in sorted(r['classes'].items())}, flush=True)
    json.dump(res, open(out, 'w'), indent=0)
    print('wrote', out)


# ---- summarize: callgrind totals and top functions ----
SUMMARIZE_DOC = r"""Summarize perf-gap/callgrind.sh output into docs/perf-gap/data/callgrind.json.

For every <engine>.<suite>.cg: the callgrind totals (Ir, Dr, Dw, I1mr, D1mr,
D1mw, ILmr, DLmr, DLmw, Bc, Bcm, Bi, Bim; simulated caches and branch
predictor) and the top self-cost functions (names normalized so the goc
symbol main.quickjs_c.JS_CallInternal and native JS_CallInternal compare).

Usage: callgrind.py summarize [CG_DIR] [OUT_JSON]"""

TOP = 40

def norm_annotate(fn):
    fn = re.sub(r"^main\.", "", fn)
    fn = re.sub(r"^[a-z_]+_c\.", "", fn)       # goc file-local prefix (quickjs_c.)
    fn = re.sub(r"\.(impl|isra|constprop|part|cold)(\.\d+)*$", "", fn)
    fn = re.sub(r"\.(isra|constprop|part|cold)\.\d+", "", fn)
    fn = re.sub(r"\.impl$", "", fn)
    fn = re.sub(r"'\d+$", "", fn)                # callgrind recursion suffix
    return fn

def summarize_main(argv):
    CG = argv[0] if len(argv) > 0 else "/workspace/perf-study/cg-gap"
    OUT = argv[1] if len(argv) > 1 else os.path.join(ROOT, "docs/perf-gap/data/callgrind.json")
    res = {}
    for f in sorted(glob.glob(os.path.join(CG, "*.cg"))):
        e, s = os.path.basename(f)[:-3].rsplit(".", 1)
        ev = tot = None
        for l in open(f):
            if l.startswith("events:"):
                ev = l.split()[1:]
            elif l.startswith("totals:"):
                tot = [int(x) for x in l.split()[1:]]
        if not ev or not tot:
            continue
        d = dict(zip(ev, tot))
        agg = {}
        try:
            a = subprocess.run(["callgrind_annotate", "--threshold=99.9", "--inclusive=no", "--auto=no", f],
                               capture_output=True, text=True, timeout=600).stdout
            body = a.split("file:function", 1)[-1]
            for l in body.splitlines():
                m = re.match(r"^\s*([\d,]+) \(", l)
                if not m or "PROGRAM TOTALS" in l:
                    continue
                tok = l.split(" [")[0].split()[-1]
                name = tok.split(":", 1)[1] if ":" in tok else tok
                k = norm_annotate(name)
                agg[k] = agg.get(k, 0) + int(m.group(1).replace(",", ""))
        except Exception as ex:  # keep totals even if annotate fails
            agg = {"annotate failed: %s" % ex: 0}
        top = sorted(agg.items(), key=lambda kv: -kv[1])[:TOP]
        res.setdefault(e, {})[s] = {"totals": d, "top": top}
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    json.dump(res, open(OUT, "w"), indent=0)
    print("wrote", OUT, len(res), "engines")


# ---- decompose: goc's extra instructions vs the gocflags native build ----
DECOMPOSE_DOC = r"""Decompose goc's extra instructions vs the gocflags native build, per suite,
from docs/perf-gap/data/callgrind-classes.json (callgrind.py classify).

Output: docs/perf-gap/data/ir-decomposition.json and a markdown table on stdout.
Each row: total Ir ratio goc/native, then the Ir delta split into
  goc-only instruction classes inside engine code (stackcheck, guard,
  fixup+morestack, tls, got), deltas of pushpop / framemem / rest+call in
  engine code (codegen differences), and function-group deltas
  (memstr, alloc, math, time, goruntime+alloca, other).
All deltas in % of the native total Ir."""

def decompose_main(argv):
    D = json.load(open(argv[0] if len(argv) > 0 else os.path.join(ROOT, 'docs/perf-gap/data/callgrind-classes.json')))
    OUT = argv[1] if len(argv) > 1 else os.path.join(ROOT, 'docs/perf-gap/data/ir-decomposition.json')
    PAIRS = [('goc-bellard', 'bellard-clang-O3-gocflags'), ('goc-ng', 'ng-clang-O3-gocflags'),
             ('bellard-clang-O3-gocflags', 'bellard-clang-O3'), ('ng-clang-O3-gocflags', 'ng-clang-O3'),
             # layer split through goc's pipeline built natively
             ('bellard-gocpipe', 'bellard-clang-O3-gocflags'), ('bellard-gocpipe-sm', 'bellard-gocpipe'),
             ('goc-bellard', 'bellard-gocpipe-sm'),
             # the inliner-threshold fix
             ('bellard-gocpipe-inl250', 'bellard-gocpipe'), ('bellard-gocpipe-sm-inl250', 'bellard-gocpipe-sm'),
             ('goc-bellard-inl250', 'goc-bellard'), ('goc-ng-inl250', 'goc-ng'),
             ('goc-bellard-inl250', 'bellard-clang-O3-gocflags'),
             ('goc-bellard-taildup', 'goc-bellard')]
    SUITES_D = ['Richards', 'DeltaBlue', 'Crypto', 'RayTrace', 'EarleyBoyer', 'RegExp', 'Splay', 'NavierStokes', 'Parse']
    COLS = ['stackcheck', 'guard', 'fixup+morestack', 'tls', 'got', 'pushpop', 'framemem', 'rest+call',
            'memstr', 'alloc', 'math', 'time', 'goruntime', 'other']
    def comp(r):
        c = r['classes']; g = r['groups']
        return {'stackcheck': c.get('stackcheck', 0), 'guard': c.get('guard', 0),
                'fixup+morestack': c.get('fixup', 0) + c.get('morestack', 0), 'tls': c.get('tls', 0),
                'got': c.get('got', 0), 'pushpop': c.get('pushpop', 0), 'framemem': c.get('framemem', 0),
                'rest+call': c.get('rest', 0) + c.get('call', 0),
                'memstr': g.get('memstr', 0), 'alloc': g.get('alloc', 0), 'math': g.get('math', 0),
                'time': g.get('time', 0), 'goruntime': g.get('goruntime', 0) + g.get('alloca', 0),
                'other': g.get('other', 0)}
    res = {}
    for a, b in PAIRS:
        if a not in D or b not in D: continue
        print(f"\n### {a} vs {b} (Ir delta, % of {b} total)\n")
        print('| suite | Ir ratio | ' + ' | '.join(COLS) + ' | ops ratio | Ir/op (a) | Ir/op (b) |')
        print('|' + '---|' * (len(COLS) + 5))
        agg_a = {k: 0 for k in COLS}; agg_b = {k: 0 for k in COLS}; ta = tb_ = 0
        for s in SUITES_D:
            if s not in D[a] or s not in D[b]: continue
            if s != 'Parse':
                for k in COLS:
                    agg_a[k] += comp(D[a][s])[k]; agg_b[k] += comp(D[b][s])[k]
                ta += D[a][s]['total']; tb_ += D[b][s]['total']
            ra, rb = D[a][s], D[b][s]
            ca, cb = comp(ra), comp(rb)
            tot = rb['total']
            row = {k: (ca[k] - cb[k]) / tot * 100 for k in COLS}
            row['ratio'] = ra['total'] / tot
            row['ops_a'] = ra.get('dispatch_ijmp', 0); row['ops_b'] = rb.get('dispatch_ijmp', 0)
            row['ci_a'] = ra.get('callinternal_ir', 0); row['ci_b'] = rb.get('callinternal_ir', 0)
            res.setdefault(f'{a}|{b}', {})[s] = row
            ipa = row['ci_a'] / row['ops_a'] if row['ops_a'] else 0
            ipb = row['ci_b'] / row['ops_b'] if row['ops_b'] else 0
            opr = row['ops_a'] / row['ops_b'] if row['ops_b'] else 0
            print(f"| {s} | {row['ratio']:.3f} | " + ' | '.join(f"{row[k]:+.2f}" for k in COLS) +
                  f" | {opr:.2f} | {ipa:.1f} | {ipb:.1f} |")
        if tb_:
            row = {k: (agg_a[k] - agg_b[k]) / tb_ * 100 for k in COLS}; row['ratio'] = ta / tb_
            res[f'{a}|{b}']['V8sum'] = row
            print(f"| V8 sum | {row['ratio']:.3f} | " + ' | '.join(f"{row[k]:+.2f}" for k in COLS) + " | | | |")
    json.dump(res, open(OUT, 'w'), indent=1)
    print('\nwrote', OUT)


# ---- bycount: one function in two builds, paired by execution count ----
BYCOUNT_DOC = r"""Compare one function in two builds by execution count (docs/research/perf-gap.md,
section 5). Every executed instruction's Ir in a callgrind --dump-instr file is
its execution count; instructions of the same basic block share a count, and
the same source block has the same count in both builds (same bytecode run).
Grouping by count therefore pairs the builds' versions of each hot block
without symbol or line information; the Ir delta of a group is
count x (instructions in A - instructions in B).

Usage: callgrind.py bycount CG_A BIN_A FUNC_A CG_B BIN_B FUNC_B [N] [--list]"""

def bycount_load(cg, binary, fn):
    cost, _ = parse_cg(cg); ins, cls = disasm(binary); real = os.path.realpath(binary)
    g = collections.defaultdict(list)
    for (ob, a), ir in cost.items():
        if ob and os.path.realpath(ob) == real and a in ins and fn in ins[a][0]:
            g[ir].append((a, cls.get(a), ins[a][1]))
    return g

def bycount_main(argv):
    args = [a for a in argv if a != '--list']; lst = '--list' in argv
    A = bycount_load(*args[0:3]); B = bycount_load(*args[3:6]); n = int(args[6]) if len(args) > 6 else 20
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


# ---- framemem: the framemem delta by kind and position ----
FRAMEMEM_DOC = r"""Split the 'framemem' Ir delta (instructions with an (%rbp)/(%rsp) operand,
engine code only) between two builds by kind and by function
(docs/research/perf-gap.md, section 4).

Kinds: lea    address of a frame slot (no memory access; goc-reanchor
              re-derives stack addresses from %rbp after calls)
       load   mov/movz/movs from a frame slot into a register
       store  mov to a frame slot
       rmw    arithmetic/cmp with a frame operand
Position: 'after call' = within 4 instructions after a call in the same
function, 'before call' = within 4 before one, else 'other'.

Usage: callgrind.py framemem CG_DIR OUT_JSON A=binA B=binB [SUITES...]"""

def framemem_kind(t):
    if t.startswith('lea'): return 'lea'
    m = re.match(r'(\w+)\s+(.*)$', t)
    op, args = m.group(1), m.group(2)
    if op.startswith('mov') or op.startswith('vmov'):
        dst = args.rsplit(',', 1)[-1]
        return 'store' if re.search(r'\(%r[bs]p\)', dst) else 'load'
    return 'rmw'

def framemem_analyse(cgdir, eng, binary, suites):
    ins, cls = disasm(binary)
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
        cost, meta = parse_cg(os.path.join(cgdir, '%s.%s.cg' % (eng, s)))
        tot += sum(cost.values())
        for (ob, a), ir in cost.items():
            if not (ob and os.path.realpath(ob) == real and a in pos): continue
            if group(ins[a][0]) != 'engine': continue
            k = framemem_kind(ins[a][1]); K[k] += ir; P[pos[a]] += ir; KP[k + '/' + pos[a]] += ir
            F[norm(ins[a][0])] += ir
    return tot, K, P, KP, F

def framemem_main(argv):
    cgdir, out = argv[0], argv[1]
    (a, ba), (b, bb) = [x.split('=', 1) for x in argv[2:4]]
    suites = argv[4:] or ['Richards', 'DeltaBlue', 'Crypto', 'RayTrace', 'EarleyBoyer', 'RegExp', 'Splay', 'NavierStokes', 'Parse']
    ta, Ka, Pa, KPa, Fa = framemem_analyse(cgdir, a, ba, suites)
    tb, Kb, Pb, KPb, Fb = framemem_analyse(cgdir, b, bb, suites)
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


# ---- frameslots: what the extra frame accesses are ----
FRAMESLOTS_DOC = r"""What the extra frame loads/stores ('framemem') are (docs/research/perf-gap.md, section 4).

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
Usage: callgrind.py frameslots OUT_JSON CG_DIR ENGINE BINARY auto|- \\
         ASM:FUNC:SYMBOL [ASM:FUNC:SYMBOL ...]"""

V8_SUITES = ['Richards', 'DeltaBlue', 'Crypto', 'RayTrace', 'EarleyBoyer', 'RegExp', 'Splay', 'NavierStokes', 'Parse']
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

def frameslots_main(argv):
    out, cgdir, eng, binary, obj = argv[0:5]
    specs = [a.split(':') for a in argv[5:]]
    ins, cls = disasm(binary)
    byfn = collections.defaultdict(list)
    for a in sorted(ins): byfn[ins[a][0]].append(a)
    cost = collections.Counter(); real = os.path.realpath(binary)
    for s in V8_SUITES:
        f = os.path.join(cgdir, '%s.%s.cg' % (eng, s))
        if not os.path.exists(f): continue
        c, _ = parse_cg(f)
        for (ob, a), ir in c.items():
            if ob and os.path.realpath(ob) == real: cost[a] += ir
    total = 0
    for s in V8_SUITES:
        f = os.path.join(cgdir, '%s.%s.cg' % (eng, s))
        if os.path.exists(f):
            total += sum(parse_cg(f)[0].values())
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


COMMANDS = {
    "summarize": (summarize_main, SUMMARIZE_DOC), "classify": (classify_main, CLASSIFY_DOC),
    "decompose": (decompose_main, DECOMPOSE_DOC), "bycount": (bycount_main, BYCOUNT_DOC),
    "framemem": (framemem_main, FRAMEMEM_DOC), "frameslots": (frameslots_main, FRAMESLOTS_DOC),
}

if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] not in COMMANDS:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    fn, d = COMMANDS[sys.argv[1]]
    if sys.argv[2:3] in (["-h"], ["--help"]):
        print(d)
        sys.exit(0)
    fn(sys.argv[2:])
