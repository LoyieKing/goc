#!/usr/bin/env python3
"""Instruction-level decomposition of callgrind Ir (docs/perf-gap.md, "where
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

Usage: perfgap-cg-classify.py CG_DIR OUT_JSON engine=binary ...
"""
import collections, json, os, re, subprocess, sys

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

def main():
    cgdir, out = sys.argv[1], sys.argv[2]
    bins = dict(a.split('=', 1) for a in sys.argv[3:])
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
main()
