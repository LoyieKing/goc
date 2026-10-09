#!/usr/bin/env python3
"""goc-color-report: which color did each source-level pointer get?

Usage (from the repo root; build/ is gitignored):
  scripts/goc-color-report.py build --flavor bellard --out build/regexp-colors/bellard
  scripts/goc-color-report.py report build/regexp-colors/bellard --md bellard.md
  scripts/goc-color-report.py report DIR --funcs lre_exec,push_state
  scripts/goc-color-report.py diff DIR_A DIR_B
  (build: --src FILE --cflags "-I..." for another TU, --no-g, --opt-level N)

Needs: the patched clang next to goc (bin/clang, else
third_party/llvm-19.1.7-clang-build/bin/clang), opt beside that clang or
opt-19 (OPT), frontend/color-escape/build/goc-color-escape (built
with make if missing) and backend/build/pass-out/GocStackMap.so (from
backend/build.sh).  The QuickJS tree (third_party/quickjs-bellard or
quickjs-ng, gitignored) must exist with scripts/qjs-gstack*.patch applied;
scripts/qjs-build.sh applies it.  build/qjs*/shim.o is optional (shim -D
redirects).

Reads the IR that the goc pipeline produces (see `build`) compiled with -g and
maps goc-color-escape's decisions back to C declarations and lines.

  build  : run the cmd/goc front half (clang -g -disable-llvm-passes ->
           goc-color-escape -> goc-inline-gate -> O3 -> goc-stackmap ->
           goc-reanchor) and keep every stage.  No goobj is written; the
           coloring pass and flags are exactly cmd/goc's (see cmd_build).
  report : tables for the named functions.
  diff   : compare !goc.color decisions of two build dirs (e.g. with and
           without -g) to show that debug info does not change coloring.

Stage A (a.color.ll, pre-O3, one alloca per C variable) is where
goc-color-escape decides colors and inserts goc_uptr_from_ptr /
goc_uptr_decode.  Stage B (a.reanchor.ll, post-O3 + stack maps) shows what is
actually left in the machine-code input: inlined uptr helpers (each
`movq %fs:-8` inline-asm is one volatile g load) and GC root slots
(goc.spill.root / goc.arganchor) chosen by the backend GocStackMap pass.

Metadata semantics (from frontend/color-escape/pass/GocColorEscape.cpp):
  * on a load / GEP / call: color of the *value*.
  * on a pointer-typed alloca that got the TU default (-default-ptr-color):
    color of the *contents* of the slot (SlotColor).  When the pass drops that
    default because a stack address is stored there, the alloca shows sptr.
  * on any other alloca: color of the slot's *address* (always sptr).
  * `!goc.color cptr` + `!goc.prov stack` on a load means DynamicUPtr: the
    value was declared cptr but may hold a decoded stack address, so storing
    it into non-stack memory is encoded (goc_uptr_from_ptr).
"""
import argparse
import collections
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

# ----------------------------------------------------------------------------
# build


def sh(cmd, env=None, **kw):
    print("+ " + " ".join(c if len(c) < 200 else c[:60] + "…" for c in cmd), file=sys.stderr)
    subprocess.run(cmd, check=True, env=env, **kw)


def shim_defs(flavor):
    out = os.path.join(ROOT, "build", "qjs" if flavor == "ng" else "qjs-bellard", "shim.o")
    if not os.path.exists(out):
        print("note: %s missing; building without shim -D redirects" % out, file=sys.stderr)
        return []
    nm = subprocess.run(["go", "tool", "nm", out], check=True, capture_output=True, text=True).stdout
    names = set()
    for line in nm.splitlines():
        f = line.split()
        if len(f) >= 3 and f[-2] == "T":
            n = f[-1].split(".")[-1] if not f[-1].endswith(".impl") else f[-1][:-5].split(".")[-1]
            if n.startswith("goc_"):
                names.add(n[4:])
    names -= {"memcpy", "memmove", "memset"}
    return ["-D%s=goc_%s" % (n, n) for n in sorted(names)]


def resolve_clang():
    """Same order as goc_resolve_clang. The patched clang is always in-tree."""
    for c in ("bin/clang",
              "third_party/llvm-19.1.7-clang-build/bin/clang",
              "third_party/llvm-clang-build/bin/clang"):
        c = os.path.join(ROOT, c)
        if os.access(c, os.X_OK):
            return c, True
    sys.exit("no patched clang next to goc. See docs/build-from-source.md")


def cmd_build(a):
    a.out = os.path.abspath(a.out)
    if a.src:
        a.src = os.path.abspath(a.src)
    os.makedirs(a.out, exist_ok=True)
    flavor = a.flavor
    qjs = os.path.join(ROOT, "third_party", "quickjs-ng" if flavor == "ng" else "quickjs-bellard")
    src = a.src or os.path.join(qjs, "libregexp.c")
    if not os.path.exists(src):
        sys.exit("missing %s (check out QuickJS under third_party/, see scripts/qjs-build.sh)" % src)
    if not a.src and "GOC_QJS_GSTACK" not in open(src, errors="replace").read():
        print("warning: %s lacks the gstack patch (scripts/qjs-build.sh applies it)" % src, file=sys.stderr)
    clang, intree = resolve_clang()
    opt = os.environ.get("OPT", "opt-19")
    for tool in (clang, opt):
        if not shutil.which(tool):
            sys.exit("missing %s (patched clang and opt ship with goc; see docs/build-from-source.md)" % tool)
    lvl = str(a.opt_level)
    # scripts/qjs-build.sh QJS_DEFS + FLAVOR_DEFS
    if flavor == "ng":
        defs = ["-DJS_NAN_BOXING=0"]
    else:
        ver = open(os.path.join(qjs, "VERSION")).read().strip() if os.path.exists(os.path.join(qjs, "VERSION")) else ""
        defs = ["-DGOC_QJS_BELLARD=1", '-DCONFIG_VERSION="%s"' % ver]
    defs += ["-D_GNU_SOURCE", "-DGOC_QJS_GSTACK=1", "-DNDEBUG"] + shim_defs(flavor)
    # cmd/goc cmd_build, in-tree clang branch, OPT_LEVEL != 0
    cargs = (["-DGOC_USE_INTREE_ATTRS"] if intree else []) + ["-mno-red-zone", "-fno-stack-protector",
             "-fno-asynchronous-unwind-tables", "-I", os.path.join(ROOT, "include"),
             "-emit-llvm", "-S", "-O" + lvl, "-fwrapv", "-fno-strict-aliasing",
             "-fno-omit-frame-pointer", "-mno-omit-leaf-frame-pointer",
             "-fno-optimize-sibling-calls", "-Xclang", "-disable-llvm-passes"]
    if not a.no_g:
        cargs.append("-g")
    o = lambda n: os.path.join(a.out, n)
    sh([clang] + cargs + defs + a.cflags.split() + ["-o", o("a.ll"), src], cwd=ROOT)
    esc = os.path.join(ROOT, "frontend/color-escape/build/goc-color-escape")
    if not os.access(esc, os.X_OK):
        if not shutil.which("make"):
            sys.exit("missing %s and no make to build it" % esc)
        sh(["make", "-C", os.path.join(ROOT, "frontend/color-escape/pass"), "all"])
    with open(o("color-escape.log"), "w") as log:
        subprocess.run([esc, o("a.ll"), "-o", o("a.color.ll"), "-fatal-errors=true",
                        "-default-ptr-color=" + a.default_color], check=True, stderr=log)
    print(open(o("color-escape.log")).read().strip(), file=sys.stderr)
    sm = os.path.join(ROOT, "backend/build/pass-out/GocStackMap.so")
    if not os.path.exists(sm):
        sys.exit("missing %s (run backend/build.sh first)" % sm)
    env = dict(os.environ, GOC_INLINE_DYNALLOC="1", GOC_SPTR_MAPS="1")
    sh([opt, "-load-pass-plugin=" + sm, "-passes=goc-inline-gate", "-S", o("a.color.ll"), "-o", o("a.gate.ll")], env=env)
    pipe = subprocess.run([opt, "-passes=default<O%s>" % lvl, "-print-pipeline-passes", "-disable-output", "/dev/null"],
                          check=True, capture_output=True, text=True).stdout.strip()
    pipe = pipe.replace(",argpromotion", "").replace(",globalopt", "")
    sh([opt, "-passes=" + pipe, "-S", o("a.gate.ll"), "-o", o("a.opt.ll")], env=env)
    sh([opt, "-load-pass-plugin=" + sm, "-passes=goc-stackmap", "-S", o("a.opt.ll"), "-o", o("a.sm.ll")], env=env)
    with open(o("reanchor.log"), "w") as log:
        subprocess.run([opt, "-load-pass-plugin=" + sm, "-passes=goc-reanchor", "-S", o("a.sm.ll"),
                        "-o", o("a.reanchor.ll")], check=True, env=env, stderr=log)
    with open(o("BUILD.txt"), "w") as f:
        f.write("flavor=%s src=%s O%s default-ptr-color=%s g=%s\n" % (flavor, src, lvl, a.default_color, not a.no_g))
        f.write("clang %s\n" % " ".join(cargs + defs))
    print("stages in " + a.out, file=sys.stderr)


# ----------------------------------------------------------------------------
# IR parsing (textual LLVM 19)

ATTACH_RE = re.compile(r",\s*!([A-Za-z_][\w.]*)\s+(!\d+)")
DEF_RE = re.compile(r"^define\s.*?@([\w.$]+)\((.*)\).*\{\s*$")


class Instr:
    __slots__ = ("name", "text", "op", "md", "fn", "idx", "block")

    def __init__(self, text, fn, idx, block):
        self.text = text
        self.fn = fn
        self.idx = idx
        self.block = block
        m = re.match(r"\s*(%[\w.$-]+)\s*=\s*(.*)$", text)
        body = m.group(2) if m else text.strip()
        self.name = m.group(1) if m else None
        self.md = {k: v for k, v in ATTACH_RE.findall(text)}
        # strip attachments for operand parsing
        cut = ATTACH_RE.search(body)
        self.op = body[:cut.start()] if cut else body

    @property
    def opcode(self):
        w = self.op.split()
        if not w:
            return ""
        if w[0] in ("tail", "musttail", "notail"):
            return w[1]
        return w[0]


class Func:
    def __init__(self, name, header):
        self.name = name
        self.header = header
        self.args = []  # names without %
        self.instrs = []
        self.defs = {}
        self.records = []  # (kind, value, var, loc)
        self.users = collections.defaultdict(list)


class Module:
    def __init__(self, path):
        self.path = path
        self.md = {}
        self.types = {}
        self.funcs = {}
        self._parse(open(path).read().splitlines())
        self._index_di()

    def _parse(self, lines):
        cur = None
        block = "entry"
        for line in lines:
            if cur is None:
                m = DEF_RE.match(line)
                if m:
                    cur = Func(m.group(1), line)
                    for p in split_top(m.group(2)):
                        mm = re.search(r"%([\w.$-]+)\s*$", p.strip())
                        cur.args.append(mm.group(1) if mm else None)
                    block = "entry"
                    continue
                mm = re.match(r"^!(\d+) = (?:distinct )?(.*)$", line)
                if mm:
                    self.md["!" + mm.group(1)] = mm.group(2)
                    continue
                mm = re.match(r"^(%[\w.$]+) = type (.*)$", line)
                if mm:
                    self.types[mm.group(1)] = mm.group(2)
                continue
            if line.startswith("}"):
                self.funcs[cur.name] = cur
                cur = None
                continue
            s = line.strip()
            if not s or s.startswith(";"):
                continue
            if re.match(r"^[\w.$-]+:", s):
                block = s.split(":")[0]
                continue
            if s.startswith("#dbg_"):
                mm = re.match(r"#dbg_(\w+)\((.*)\)\s*$", s)
                parts = split_top(mm.group(2))
                cur.records.append((mm.group(1), parts[0].strip(), parts[1].strip(), parts[-1].strip()))
                continue
            ins = Instr(line, cur, len(cur.instrs), block)
            cur.instrs.append(ins)
            if ins.name:
                cur.defs[ins.name] = ins
            for v in set(re.findall(r"%[\w.$-]+", ins.op)):
                if v != ins.name:
                    cur.users[v].append(ins)

    # --- DI
    def _index_di(self):
        self.fields = {}
        for k, v in self.md.items():
            if v.startswith("!DI"):
                kind = v[1:v.index("(")]
                f = di_fields(v)
                f["_kind"] = kind
                self.md[k] = f

    def di(self, ref):
        v = self.md.get(ref)
        return v if isinstance(v, dict) else None

    def loc(self, ref):
        """!DILocation -> [(line, col, subprogram-name), ... outer]"""
        out = []
        seen = 0
        while ref and seen < 20:
            seen += 1
            d = self.di(ref)
            if not d or d["_kind"] != "DILocation":
                break
            out.append((int(d.get("line", 0)), int(d.get("column", 0)), self.scope_fn(d.get("scope"))))
            ref = d.get("inlinedAt")
        return out

    def scope_fn(self, ref):
        for _ in range(50):
            d = self.di(ref)
            if not d:
                return "?"
            if d["_kind"] == "DISubprogram":
                return d.get("name", "?").strip('"')
            ref = d.get("scope")
        return "?"

    def file_of(self, ref):
        d = self.di(ref)
        if not d:
            return None
        return os.path.join(d.get("directory", '""').strip('"'), d.get("filename", '""').strip('"'))

    def di_struct_of_ptr(self, tref):
        for _ in range(10):
            d = self.di(tref)
            if not d:
                return None
            if d.get("tag") in ("DW_TAG_pointer_type", "DW_TAG_const_type", "DW_TAG_volatile_type"):
                tref = d.get("baseType")
                if d.get("tag") == "DW_TAG_pointer_type":
                    break
                continue
            return None
        for _ in range(10):
            d = self.di(tref)
            if not d:
                return None
            n = d.get("name", "").strip('"')
            for cand in ("%struct." + n, "%union." + n):
                if n and cand in self.types:
                    return cand
            tref = d.get("baseType")
        return None

    def field_at(self, tyname, off):
        names = self.struct_fields(tyname)
        elems = parse_struct_body(self.types.get(tyname, ""))
        o = 0
        for k, e in enumerate(elems):
            sz, al = type_size_align(e, self.types)
            o = (o + al - 1) // al * al
            if o == off:
                return names[k]
            if o < off < o + sz:
                return "%s+%d" % (names[k], off - o)
            o += sz
        return None

    def struct_fields(self, tyname):
        """IR struct name -> list of member names per LLVM element index."""
        if tyname in self.fields:
            return self.fields[tyname]
        body = self.types.get(tyname, "")
        elems = parse_struct_body(body)
        offs = []
        off = 0
        packed = body.startswith("<{")
        for e in elems:
            sz, al = type_size_align(e, self.types)
            if not packed:
                off = (off + al - 1) // al * al
            offs.append(off)
            off += sz
        # find DI composite with that name (struct.X or union.X / typedef X)
        base = tyname.split(".", 1)[1] if "." in tyname else tyname[1:]
        base = re.sub(r"\.\d+$", "", base)
        comp = None
        for k, d in self.md.items():
            if not isinstance(d, dict):
                continue
            if d["_kind"] == "DICompositeType" and d.get("name", "").strip('"') == base and "elements" in d:
                comp = d
                break
            if d["_kind"] == "DIDerivedType" and d.get("tag") == "DW_TAG_typedef" and d.get("name", "").strip('"') == base:
                c = self.di(d.get("baseType"))
                if c and c["_kind"] == "DICompositeType" and "elements" in c:
                    comp = c
        names = []
        members = []
        if comp:
            el = self.md.get(comp["elements"])
            if isinstance(el, str):
                for r in re.findall(r"!\d+", el):
                    m = self.di(r)
                    if m and m.get("tag") == "DW_TAG_member" and "DIFlagBitField" not in m.get("flags", ""):
                        members.append((int(m.get("offset", 0)) // 8, m.get("name", "?").strip('"')))
        for o in offs:
            n = [nm for (mo, nm) in members if mo == o]
            names.append("/".join(n) if n else "#%d" % len(names))
        self.fields[tyname] = names
        return names


def split_top(s):
    out, depth, cur, q = [], 0, "", False
    for ch in s:
        if q:
            cur += ch
            if ch == '"':
                q = False
            continue
        if ch == '"':
            q = True
        elif ch in "([{<":
            depth += 1
        elif ch in ")]}>":
            depth -= 1
        elif ch == "," and depth == 0:
            out.append(cur)
            cur = ""
            continue
        cur += ch
    if cur.strip():
        out.append(cur)
    return out


def di_fields(v):
    inner = v[v.index("(") + 1:v.rindex(")")]
    f = {}
    for p in split_top(inner):
        if ":" in p:
            k, val = p.split(":", 1)
            f[k.strip()] = val.strip()
    return f


def parse_struct_body(body):
    body = body.strip()
    if body.startswith("<{"):
        body = body[2:-2]
    elif body.startswith("{"):
        body = body[1:-1]
    else:
        return []
    return [e.strip() for e in split_top(body) if e.strip()]


def type_size_align(t, types):
    t = t.strip()
    if t == "ptr":
        return 8, 8
    m = re.match(r"^i(\d+)$", t)
    if m:
        b = int(m.group(1))
        n = max(1, (b + 7) // 8)
        n = 1 << (n - 1).bit_length()
        return n, min(n, 16 if b > 64 else 8)
    if t in ("float",):
        return 4, 4
    if t in ("double",):
        return 8, 8
    if t == "x86_fp80":
        return 16, 16
    m = re.match(r"^\[(\d+) x (.*)\]$", t)
    if m:
        s, a = type_size_align(m.group(2), types)
        return s * int(m.group(1)), a
    if t.startswith("%"):
        elems = parse_struct_body(types.get(t, "{}"))
        packed = types.get(t, "").startswith("<{")
        off, al = 0, 1
        for e in elems:
            s, a = type_size_align(e, types)
            if not packed:
                off = (off + a - 1) // a * a
                al = max(al, a)
            off += s
        return (off + al - 1) // al * al, al
    if t.startswith("{") or t.startswith("<{"):
        elems = parse_struct_body(t)
        off, al = 0, 1
        for e in elems:
            s, a = type_size_align(e, types)
            off = (off + a - 1) // a * a
            al = max(al, a)
            off += s
        return (off + al - 1) // al * al, al
    return 8, 8


# ----------------------------------------------------------------------------
# helpers over one function


def operands(ins):
    """typed operand list of a call/load/store/gep (strings).
    load  -> [addr]            store -> [value, addr]"""
    op = ins.op
    oc = ins.opcode
    if oc == "call":
        m = re.search(r"@([\w.$]+)\((.*)\)", op)
        return split_top(m.group(2)) if m else []
    body = op.split(None, 1)[1] if " " in op else ""
    parts = [x.strip() for x in split_top(body)]
    parts = [x for x in parts if not re.match(r"^(align|!)", x)]
    if parts and parts[0].startswith("volatile "):
        parts[0] = parts[0][len("volatile "):]
    if oc == "load":
        return parts[1:2]
    if oc == "store":
        return parts[:2]
    if oc == "getelementptr":
        if parts and re.match(r"^(inbounds|nuw|nusw)\b", parts[0]):
            parts[0] = re.sub(r"^((inbounds|nuw|nusw)\s+)+", "", parts[0])
    return parts


def load_ty(ins):
    m = re.match(r"load\s+(?:volatile\s+|atomic\s+)*([^,]+),", ins.op)
    return m.group(1).strip() if m else "?"


def val_of(typed):
    """'ptr noundef %x' -> '%x' ; 'i64 3' -> '3'"""
    w = typed.strip().split()
    return w[-1] if w else ""


def callee(ins):
    m = re.search(r"call\s[^@]*@([\w.$]+)\(", ins.op)
    return m.group(1) if m else None


class FnView:
    def __init__(self, mod, fn, srccache):
        self.m = mod
        self.f = fn
        self.src = srccache
        self.var_of = {}   # alloca/SSA name -> DILocalVariable dict
        self.vars_ssa = collections.defaultdict(set)
        for kind, val, var, loc in fn.records:
            d = mod.di(var)
            if not d:
                continue
            v = val.split()[-1]
            if kind == "declare":
                self.var_of[v] = d
            elif kind == "value":
                self.vars_ssa[v].add(d.get("name", "?").strip('"'))

    # ---- naming
    def varname(self, a):
        d = self.var_of.get(a)
        return d.get("name", "?").strip('"') if d else None

    def ins(self, v):
        return self.f.defs.get(v)

    def rv(self, v, depth=0):
        """render an rvalue"""
        v = v.strip()
        if depth > 7:
            return "…"
        if v == "null":
            return "NULL"
        if not v.startswith("%"):
            return v
        if v[1:] in self.f.args:
            return v[1:]
        i = self.ins(v)
        if i is None:
            return v
        oc = i.opcode
        if oc == "alloca":
            n = self.varname(v)
            return "&" + (n or v)
        if oc == "load":
            ops = operands(i)
            addr = val_of(ops[-1]) if ops else ""
            return self.lv(addr, load_ty(i), depth + 1)
        if oc == "getelementptr":
            return self.gep(i, depth, as_lvalue=False)
        if oc in ("sext", "zext", "trunc", "inttoptr", "ptrtoint", "bitcast", "freeze"):
            ops = operands(i)
            src = ops[0].split(" to ")[0]
            return self.rv(val_of(src), depth + 1)
        if oc == "call":
            c = callee(i)
            args = [self.rv(val_of(x), depth + 1) for x in operands(i)]
            return "%s(%s)" % (c, ", ".join(args))
        if oc in ("add", "sub", "mul", "shl", "and", "or", "xor", "lshr", "ashr", "sdiv", "udiv"):
            ops = operands(i)
            sym = {"add": "+", "sub": "-", "mul": "*", "shl": "<<", "and": "&", "or": "|", "xor": "^",
                   "lshr": ">>", "ashr": ">>", "sdiv": "/", "udiv": "/"}[oc]
            a0 = val_of(ops[0]) if ops else "?"
            a1 = val_of(ops[1]) if len(ops) > 1 else "?"
            return "(%s %s %s)" % (self.rv(a0, depth + 1), sym, self.rv(a1, depth + 1))
        if oc in ("phi", "select"):
            return "%s(…)" % oc
        return v

    def gep(self, i, depth, as_lvalue):
        ops = operands(i)
        m = re.match(r"getelementptr\s+(?:inbounds\s+|nuw\s+|nusw\s+)*([^,]+),", i.op)
        sty = m.group(1).strip() if m else "?"
        base = val_of(ops[1]) if len(ops) > 1 else "?"
        idx = [val_of(x) for x in ops[2:]]
        b = self.rv(base, depth + 1)
        if sty.startswith("%") and len(idx) >= 2 and idx[0] == "0" and not sty.startswith("%union"):
            names = self.m.struct_fields(sty)
            try:
                fld = names[int(idx[1])]
            except (ValueError, IndexError):
                fld = "#" + idx[1]
            s = "%s->%s" % (b, fld)
            for extra in idx[2:]:
                s += "[%s]" % self.rv(extra, depth + 1)
            return s if as_lvalue else "&" + s
        if len(idx) == 1:
            k = self.rv(idx[0], depth + 1)
            st = self.pointee_struct(base)
            if sty == "i8" and st and re.match(r"^-?\d+$", idx[0]):
                fld = self.m.field_at(st, int(idx[0]))
                if fld:
                    bb = b[1:] + "." if b.startswith("&") else b + "->"
                    return (bb + fld) if as_lvalue else "&" + bb + fld
            if sty == "i8":
                return ("*(%s + %s)" if as_lvalue else "(%s + %s)") % (b, k)
            return ("%s[%s]" if as_lvalue else "(%s + %s)") % (b, k)
        return ("*" if as_lvalue else "") + "gep(%s,%s)" % (b, ",".join(idx))

    def pointee_struct(self, v):
        """IR struct type that pointer value v points to (alloca type or DI type of a param)."""
        v = v.strip()
        i = self.ins(v) if v.startswith("%") else None
        if i is not None and i.opcode == "alloca":
            t = alloca_type(i)
            return t if t.startswith("%") else None
        if i is not None and i.opcode == "load" and "goc." in (i.name or ""):
            # reload of an anchor/root: look through to the stored value
            addr = val_of(operands(i)[0])
            for st in self.stores_to(addr):
                return self.pointee_struct(val_of(operands(st)[0]))
        if v.startswith("%") and v[1:] in self.f.args:
            for kind, val, var, loc in self.f.records:
                if val.split()[-1] == v:
                    return self.m.di_struct_of_ptr(self.m.di(var).get("type") if self.m.di(var) else None)
        return None

    def lv(self, addr, ty, depth=0):
        """render the lvalue at address `addr` accessed with type ty"""
        if addr.startswith("%") and addr[1:] not in self.f.args:
            i = self.ins(addr)
            if i is not None:
                if i.opcode == "alloca":
                    return self.varname(addr) or addr
                if i.opcode == "getelementptr":
                    s = self.gep(i, depth, as_lvalue=True)
                    if "%union." in i.op:
                        s += ".ptr" if ty == "ptr" else ".val"
                    return s
        return "*" + self.rv(addr, depth + 1)

    # ---- origin classification for stored values
    def origins(self, v, seen=None, depth=0):
        seen = seen if seen is not None else set()
        v = v.strip()
        out = set()
        if depth > 12 or v in seen:
            return out
        seen.add(v)
        if v == "null":
            return {"NULL"}
        if not v.startswith("%"):
            return {"const"}
        if v[1:] in self.f.args:
            return {"param " + v[1:]}
        i = self.ins(v)
        if i is None:
            return {v}
        oc = i.opcode
        if oc == "alloca":
            return {"&" + (self.varname(v) or v) + " (stack object)"}
        if oc == "call":
            c = callee(i)
            if c == "goc_uptr_decode":
                return {"goc_uptr_decode() @L%s" % self.line(i)}
            return {"%s() result" % c}
        if oc in ("getelementptr", "bitcast", "inttoptr", "ptrtoint", "freeze"):
            ops = operands(i)
            base = val_of(ops[1]) if oc == "getelementptr" else val_of(ops[0].split(" to ")[0])
            return self.origins(base, seen, depth + 1)
        if oc == "load":
            addr = val_of(operands(i)[-1])
            ai = self.ins(addr) if addr.startswith("%") else None
            if ai is not None and ai.opcode == "alloca":
                name = self.varname(addr) or addr
                res = set()
                for st in self.stores_to(addr):
                    for o in self.origins(val_of(operands(st)[0]), seen, depth + 1):
                        res.add(o if o.startswith("via ") else "via %s: %s" % (name, o))
                return res or {"var " + name}
            col = i.md.get("goc.color")
            return {"load %s [%s]" % (self.lv(addr, "ptr"), self.color_str(i))}
        if oc in ("phi", "select"):
            for x in re.findall(r"%[\w.$-]+", i.op):
                out |= self.origins(x, seen, depth + 1)
            return out
        return {oc}

    def stores_to(self, alloca):
        res = []
        for u in self.f.users.get(alloca, []):
            if u.opcode == "store":
                ops = operands(u)
                if len(ops) >= 2 and val_of(ops[1]) == alloca:
                    res.append(u)
        return res

    def loads_from(self, alloca):
        res = []
        for u in self.f.users.get(alloca, []):
            if u.opcode == "load" and val_of(operands(u)[-1]) == alloca:
                res.append(u)
        return res

    def color_str(self, i):
        c = md_str(self.m, i.md.get("goc.color"))
        p = md_str(self.m, i.md.get("goc.prov"))
        if c is None:
            return "-"
        if c == "cptr" and p == "stack":
            return "cptr+stackprov"
        return c

    def line(self, i):
        l = self.m.loc(i.md.get("dbg"))
        return l[0][0] if l else "?"

    def srcline(self, ln, fileref=None):
        path = self.m.file_of(fileref) if fileref else None
        return self.src.get(path, ln)


def md_str(mod, ref):
    if not ref:
        return None
    v = mod.md.get(ref)
    if isinstance(v, str):
        m = re.match(r'!\{!"(.*)"\}', v)
        return m.group(1) if m else v
    return None


class SrcCache:
    def __init__(self):
        self.c = {}

    def get(self, path, ln):
        if not path or not isinstance(ln, int):
            return ""
        if path not in self.c:
            try:
                self.c[path] = open(path, errors="replace").read().splitlines()
            except OSError:
                self.c[path] = []
        L = self.c[path]
        return L[ln - 1].strip() if 0 < ln <= len(L) else ""


# ----------------------------------------------------------------------------
# stage A report


def is_ptr_alloca(i):
    m = re.match(r"alloca\s+([^,]+)", i.op)
    return m and m.group(1).strip() == "ptr"


def alloca_type(i):
    m = re.match(r"alloca\s+([^,]+)", i.op)
    return m.group(1).strip() if m else "?"


def type_has_ptr(t, types, depth=0):
    if depth > 6:
        return False
    if t == "ptr":
        return True
    m = re.match(r"^\[(\d+) x (.*)\]$", t)
    if m:
        return type_has_ptr(m.group(2), types, depth + 1)
    if t.startswith("%"):
        return any(type_has_ptr(e, types, depth + 1) for e in parse_struct_body(types.get(t, "")))
    return False


def summarize_colors(cs):
    c = collections.Counter(cs)
    return ", ".join("%s×%d" % (k, n) for k, n in sorted(c.items())) or "-"


def resolve(load_colors, slot_color):
    s = set(load_colors)
    if not s:
        return slot_color or "-"
    if s == {"sptr"}:
        return "sptr"
    if s == {"cptr"}:
        return "cptr"
    if s == {"cptr+stackprov"}:
        return "cptr (stack-prov: may-be-stack)"
    if s == {"uptr"}:
        return "uptr"
    return "/".join(sorted(s))


def stack_only_callsites(mod, fname, argno):
    """actual args at every call site of fname (for the stack-only-formal proof)."""
    out = []
    for f in mod.funcs.values():
        for i in f.instrs:
            if i.opcode == "call" and callee(i) == fname:
                ops = operands(i)
                if argno < len(ops):
                    fv = FnView(mod, f, SrcCache())
                    out.append("%s: %s" % (f.name, fv.rv(val_of(ops[argno]))))
    return out


def reason_for(fv, alloca, var, resolved, origins):
    f = fv.f
    arg = var.get("arg") if var else None
    o = sorted(origins)
    if resolved.startswith("sptr"):
        if arg:
            internal = "internal" in f.header.split("@")[0]
            sites = stack_only_callsites(fv.m, f.name, int(arg) - 1)
            return ("stack-only formal (proveStackOnlyFormals: file-local fn, every call site passes a stack "
                    "address: %s)" % "; ".join(sites)) if internal else "param colored sptr"
        if any("stack object" in x for x in o):
            return "stores address of a stack object (%s) -> TU default dropped, raw sptr slot" % "; ".join(
                x for x in o if "stack object" in x)
        if any("param " in x for x in o):
            return ("stores a value derived from a stack-only formal (borrowedFromStackFormal treats `s->field` "
                    "loaded through such a formal as borrowed stack): %s" % "; ".join(o))
        return "stack provenance: %s" % "; ".join(o)
    if resolved.startswith("cptr (stack-prov"):
        dec = [x for x in o if "goc_uptr_decode" in x]
        if dec:
            return ("declared cptr (TU default) but receives goc_uptr_decode() results -> DynamicUPtr, treated as "
                    "may-be-stack; every store of it to non-stack memory is encoded: %s" % "; ".join(dec))
        return "declared cptr but carries stack provenance (%s)" % "; ".join(o)
    if resolved == "cptr":
        st = [x for x in o if "[sptr]" in x or "stack object" in x]
        if st:
            return ("colored cptr although stack-provenance values are stored into it (%s); the pass keeps the TU "
                    "default because mayDirectlyNameStack/borrowedFromStackFormal cannot prove the stored value "
                    "names the stack (e.g. the sp = sp +/- n self-cycle) [inferred]" % "; ".join(st))
        return "TU default -default-ptr-color=cptr; no stack-provenance value reaches it"
    if resolved == "uptr":
        return "encoded storage"
    return "; ".join(o)


def report_function_A(fv, out, entities, enc_rows, dec_rows, field_rows):
    f = fv.f
    m = fv.m
    for i in f.instrs:
        if i.opcode != "alloca":
            continue
        ty = alloca_type(i)
        if not type_has_ptr(ty, m.types):
            continue
        var = fv.var_of.get(i.name)
        name = fv.varname(i.name) or i.name
        if name.startswith("%goc.") or name == "%retval":
            continue
        ln = int(var.get("line", 0)) if var else "?"
        kind = ("param #%s" % var["arg"]) if var and var.get("arg") else "local"
        if ty != "ptr":
            kind += " (%s)" % ("struct" if ty.startswith("%") else "array")
        slot = md_str(m, i.md.get("goc.color"))
        loads = fv.loads_from(i.name)
        lc = [fv.color_str(l) for l in loads if load_ty(l) == "ptr"]
        origins = set()
        for st in fv.stores_to(i.name):
            v = val_of(operands(st)[0])
            if operands(st)[0].strip().startswith("ptr"):
                origins |= fv.origins(v)
        if ty == "ptr":
            res = resolve(lc, slot)
            reason = reason_for(fv, i.name, var, res, origins)
        else:
            res = "address: sptr (stack object)"
            reason = "alloca; pointer members are stack storage (stores into it are never encoded)"
        entities.append(dict(fn=f.name, kind=kind, name=name, line=ln,
                             decl=fv.srcline(ln, var.get("file") if var else None) if var else "",
                             slot=slot or "-", loads=summarize_colors(lc), resolved=res, reason=reason))
    # struct-field pointer accesses (non-alloca base)
    fld = collections.OrderedDict()
    for i in f.instrs:
        if i.opcode not in ("load", "store"):
            continue
        ops = operands(i)
        if i.opcode == "load":
            if load_ty(i) != "ptr":
                continue
            addr = val_of(ops[-1])
        else:
            if not ops[0].strip().startswith("ptr"):
                continue
            addr = val_of(ops[1])
        g = fv.ins(addr) if addr.startswith("%") else None
        if g is None or g.opcode != "getelementptr":
            continue
        mm = re.match(r"getelementptr\s+(?:inbounds\s+)?([^,]+),", g.op)
        sty = mm.group(1).strip()
        if not sty.startswith("%"):
            continue
        gops = operands(g)
        idx = [val_of(x) for x in gops[2:]]
        if sty.startswith("%union"):
            key = "%s[] .ptr (%s)" % (sty, "union member")
        elif len(idx) >= 2 and idx[0] == "0":
            names = m.struct_fields(sty)
            try:
                key = "%s.%s" % (sty, names[int(idx[1])])
            except (ValueError, IndexError):
                key = "%s.#%s" % (sty, idx[1])
        else:
            continue
        r = fld.setdefault(key, dict(loads=[], stores=[], enc=0, lines=set()))
        r["lines"].add(fv.line(i))
        if i.opcode == "load":
            r["loads"].append(fv.color_str(i))
        else:
            v = val_of(ops[0])
            vi = fv.ins(v) if v.startswith("%") else None
            if vi is not None and vi.opcode == "call" and callee(vi) == "goc_uptr_from_ptr":
                r["enc"] += 1
                r["stores"].append("uptr(encoded)")
            elif v == "null":
                r["stores"].append("NULL")
            elif vi is not None:
                r["stores"].append(fv.color_str(vi))
            else:
                r["stores"].append("param" if v[1:] in f.args else v)
    for k, r in fld.items():
        field_rows.append(dict(fn=f.name, field=k, loads=summarize_colors(r["loads"]),
                               stores=summarize_colors(r["stores"]), enc=r["enc"],
                               lines=",".join(str(x) for x in sorted(r["lines"], key=lambda z: (str(type(z)), z)))))
    # encode / decode sites
    for i in f.instrs:
        if i.opcode != "call":
            continue
        c = callee(i)
        if c == "goc_uptr_from_ptr":
            val = val_of(operands(i)[0])
            vi = fv.ins(val)
            dest = "?"
            for u in f.users.get(i.name, []):
                if u.opcode == "store":
                    dest = fv.lv(val_of(operands(u)[1]), "ptr")
            ln = fv.line(i)
            enc_rows.append(dict(fn=f.name, line=ln, src=fv.srcline(ln, fv.m.di(i.md.get("dbg")) and
                                                                    fn_file(fv, i)),
                                 dest=dest, value=fv.rv(val),
                                 vcolor=fv.color_str(vi) if vi is not None else ("param" if val[1:] in f.args else "?"),
                                 why="; ".join(sorted(fv.origins(val)))))
        elif c in ("goc_uptr_decode", "goc_uptr_as_cptr", "goc_uptr_require_cptr", "goc_uptr_as_sptr"):
            ln = fv.line(i)
            dec_rows.append(dict(fn=f.name, line=ln, helper=c, src=fv.srcline(ln, fn_file(fv, i)),
                                 arg=fv.rv(val_of(operands(i)[0])),
                                 inserted="pass" if i.name and i.name.startswith("%goc.") else "source"))


def fn_file(fv, i):
    d = fv.m.di(i.md.get("dbg"))
    if not d:
        return None
    sc = d.get("scope")
    for _ in range(50):
        s = fv.m.di(sc)
        if not s:
            return None
        if "file" in s:
            return s["file"]
        sc = s.get("scope")
    return None


# ----------------------------------------------------------------------------
# stage B report (post-O3)


def report_function_B(mod, fn, srcc, uptr_rows, root_rows):
    fv = FnView(mod, fn, srcc)
    # inline-asm g loads
    sites = collections.OrderedDict()
    for i in fn.instrs:
        if "movq %fs:-8" not in i.op:
            continue
        off = None
        for u in fn.users.get(i.name, []):
            if u.opcode == "inttoptr":
                for g in fn.users.get(u.name, []):
                    mm = re.search(r"i64 (-?\d+)\s*$", g.op)
                    if g.opcode == "getelementptr" and mm:
                        off = int(mm.group(1))
                    elif g.opcode == "load":
                        off = 0
        chain = mod.loc(i.md.get("dbg"))
        key = (i.block, tuple(chain))
        s = sites.setdefault(key, dict(offs=[], chain=chain, block=i.block))
        s["offs"].append(off)
    for key, s in sites.items():
        offs = set(s["offs"])
        kind = "encode (from_ptr: lo+hi)" if 0 in offs and 8 in offs else (
            "decode (hi)" if offs == {8} else "g load off=%s" % sorted(offs, key=str))
        ch = s["chain"]
        inner = ch[0] if ch else ("?", 0, "?")
        where = " <- ".join("%s:%s" % (c[2], c[0]) for c in ch)
        uptr_rows.append(dict(fn=fn.name, kind=kind, reads=len(s["offs"]), line=inner[0], where=where,
                              src=srcc.get(fn_src_path(mod, fn, inner), inner[0]) if ch else "",
                              block=s["block"]))
    # roots
    for i in fn.instrs:
        if i.opcode != "alloca" or not re.match(r"%goc\.(spill\.root|arganchor)", i.name or ""):
            continue
        stores = [u for u in fn.users.get(i.name, []) if u.opcode == "store" and val_of(operands(u)[1]) == i.name]
        loads = [u for u in fn.users.get(i.name, []) if u.opcode == "load"]
        vals = []
        colors = set()
        exprs = []
        vars_ = set()
        lines = set()
        for st in stores:
            v = val_of(operands(st)[0])
            vals.append(v)
            vars_ |= fv.vars_ssa.get(v, set())
            if v[1:] in fn.args:
                vars_.add("param " + v[1:])
            vi = fv.ins(v)
            if vi is not None:
                colors.add(fv.color_str(vi))
                exprs.append(fv.rv(v))
                l = mod.loc(vi.md.get("dbg"))
                if l:
                    lines.add(l[0][0])
                # phi: look at incoming for names
                if vi.opcode == "phi":
                    for x in re.findall(r"%[\w.$-]+", vi.op):
                        vars_ |= fv.vars_ssa.get(x, set())
        for ld in loads:
            vars_ |= fv.vars_ssa.get(ld.name, set())
        root_rows.append(dict(fn=fn.name, slot=i.name, vars=", ".join(sorted(vars_)) or "?",
                              color=",".join(sorted(colors)) or "-", expr="; ".join(exprs)[:120],
                              values=", ".join(vals[:3]), lines=",".join(map(str, sorted(lines))),
                              stores=len(stores), reloads=len(loads)))


def fn_src_path(mod, fn, inner):
    # choose the file of the innermost location's subprogram
    for k, d in mod.md.items():
        if isinstance(d, dict) and d["_kind"] == "DISubprogram" and d.get("name", "").strip('"') == inner[2]:
            return mod.file_of(d.get("file"))
    return None


# ----------------------------------------------------------------------------


def md_table(rows, cols, heads=None):
    heads = heads or cols
    esc = lambda s: str(s).replace("|", "\\|").replace("\n", " ")
    out = ["| " + " | ".join(heads) + " |", "|" + "---|" * len(cols)]
    for r in rows:
        out.append("| " + " | ".join(esc(r.get(c, "")) for c in cols) + " |")
    return "\n".join(out)


def cmd_report(a):
    srcc = SrcCache()
    modA = Module(os.path.join(a.dir, "a.color.ll"))
    funcs = a.funcs.split(",")
    ent, enc, dec, fld = [], [], [], []
    for fn in funcs:
        if fn in modA.funcs:
            report_function_A(FnView(modA, modA.funcs[fn], srcc), None, ent, enc, dec, fld)
    out = []
    title = a.title or a.dir
    out.append("## %s\n" % title)
    bt = os.path.join(a.dir, "BUILD.txt")
    if os.path.exists(bt):
        out.append("Build: `%s`\n" % open(bt).readline().strip())
    log = os.path.join(a.dir, "color-escape.log")
    if os.path.exists(log):
        out.append("goc-color-escape: `%s`\n" % open(log).read().strip().splitlines()[-1])
    out.append("### A. Per-declaration colors (goc-color-escape output, pre-O3)\n")
    out.append(md_table(ent, ["fn", "kind", "name", "line", "decl", "slot", "loads", "resolved", "reason"],
                        ["function", "kind", "name", "line", "source declaration", "alloca !goc.color",
                         "loads (value colors)", "resolved", "reason (derived from IR + pass rules)"]))
    out.append("\n### B. Pointer loads/stores through struct/union fields\n")
    out.append(md_table(fld, ["fn", "field", "loads", "stores", "enc", "lines"],
                        ["function", "field", "loaded value colors", "stored value colors", "# encoded stores",
                         "lines"]))
    out.append("\n### C. uptr encode sites inserted by goc-color-escape (goc_uptr_from_ptr)\n")
    out.append(md_table(enc, ["fn", "line", "src", "dest", "value", "vcolor", "why"],
                        ["function", "line", "source", "destination", "stored value", "value color",
                         "value origins (why may-be-stack)"]))
    out.append("\n### D. uptr decode / check calls\n")
    out.append(md_table(dec, ["fn", "line", "helper", "inserted", "src", "arg"],
                        ["function", "line", "helper", "inserted by", "source", "argument"]))
    rpath = os.path.join(a.dir, "a.reanchor.ll")
    if os.path.exists(rpath):
        modB = Module(rpath)
        up, roots = [], []
        for fn in funcs:
            if fn in modB.funcs:
                report_function_B(modB, modB.funcs[fn], srcc, up, roots)
        out.append("\n### E. Post-O3 machine-code input: inlined uptr helpers (volatile FS:-8 g loads)\n")
        out.append("Functions present after O3: %s\n" % ", ".join(f for f in funcs if f in modB.funcs))
        out.append(md_table(up, ["fn", "kind", "reads", "line", "where", "src"],
                            ["function", "helper", "# g loads", "line", "inline chain", "source"]))
        out.append("\n### F. Post-O3 GC root slots (GocStackMap: goc.spill.root / goc.arganchor)\n")
        out.append(md_table(roots, ["fn", "slot", "vars", "values", "color", "expr", "lines", "stores", "reloads"],
                            ["function", "slot", "source variable(s) (dbg.value)", "SSA value",
                             "value !goc.color (dropped by O3 = -)", "value expression", "def line",
                             "# volatile stores", "# volatile reloads"]))
    text = "\n".join(out) + "\n"
    if a.md:
        with open(a.md, "w") as f:
            f.write(text)
    sys.stdout.write(text)


def color_sig(path, funcs=None):
    m = Module(path)
    sig = {}
    for fn in m.funcs.values():
        if funcs and fn.name not in funcs:
            continue
        rows = []
        for i in fn.instrs:
            c = md_str(m, i.md.get("goc.color"))
            if c is None and callee(i) not in ("goc_uptr_from_ptr", "goc_uptr_decode"):
                continue
            rows.append((i.name or callee(i), c, md_str(m, i.md.get("goc.prov")), callee(i)))
        sig[fn.name] = rows
    return sig


def cmd_diff(a):
    s1 = color_sig(os.path.join(a.dir1, "a.color.ll"))
    s2 = color_sig(os.path.join(a.dir2, "a.color.ll"))
    bad = 0
    for fn in sorted(set(s1) | set(s2)):
        if s1.get(fn) != s2.get(fn):
            bad += 1
            print("DIFF %s: %d vs %d colored values" % (fn, len(s1.get(fn, [])), len(s2.get(fn, []))))
    n = sum(len(v) for v in s1.values())
    print("compared %d functions, %d colored values; %d functions differ" % (len(s1), n, bad))
    return 1 if bad else 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = p.add_subparsers(dest="cmd", required=True)
    b = sp.add_parser("build")
    b.add_argument("--flavor", choices=["ng", "bellard"], default="bellard")
    b.add_argument("--src", help="C file (default: <flavor>/libregexp.c)")
    b.add_argument("--out", required=True)
    b.add_argument("--opt-level", default=3, type=int)
    b.add_argument("--default-color", default="cptr")
    b.add_argument("--no-g", action="store_true", help="build without -g (for `diff`)")
    b.add_argument("--cflags", default="", help="extra clang flags, e.g. '-I dir' for a patched copy")
    r = sp.add_parser("report")
    r.add_argument("dir")
    r.add_argument("--funcs", default="lre_exec,lre_exec_backtrack,lre_poll_timeout,stack_realloc,push_state,"
                                      "lre_check_stack_overflow,get_u16,get_u32,lre_get_flags")
    r.add_argument("--md")
    r.add_argument("--title")
    d = sp.add_parser("diff")
    d.add_argument("dir1")
    d.add_argument("dir2")
    a = p.parse_args()
    if a.cmd == "build":
        cmd_build(a)
    elif a.cmd == "report":
        cmd_report(a)
    else:
        sys.exit(cmd_diff(a))


if __name__ == "__main__":
    main()
