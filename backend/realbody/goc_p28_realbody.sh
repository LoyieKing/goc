#!/usr/bin/env bash
# P28/P29: real Clang IR body → llc ISel → elfpack goobj (NO P21 seedMIR).
# Usage: goc_p28_realbody.sh <input.ll|.c> <out.goobj.o> [fn_name] [go_sym] [--all] [--goabi]
#   --all:   emit TEXT for every defined function in the TU (multi-function, P29);
#            frames derived from each llc prologue, per-function CALL lists.
#            Real-MF pointer maps are not available yet → stackmap_index=-1.
#   --goabi: Go amd64 ABIInternal entry thunks (int/ptr subset, <=6 args): each
#            eligible F is renamed F.impl (SysV body, internal calls unchanged)
#            and gains a Go-ABI thunk F (Go arg regs -> SysV regs, jmp F.impl).
set -euo pipefail
SELF="$(cd "$(dirname "$0")" && pwd)"
ROOT="${GOC_ROOT:-$(cd "$SELF/../.." && pwd)}"
export GOC_ROOT="$ROOT"
P5="$ROOT/backend"

ALL=0
GOABI=0
ABI0=0
POS=()
for a in "$@"; do
  case "$a" in
    --all) ALL=1 ;;
    --goabi) GOABI=1; ALL=1 ;;
    --abi0) ABI0=1 ;;
    *) POS+=("$a") ;;
  esac
done
set -- ${POS[@]+"${POS[@]}"}

IN="${1:?input .ll or .c}"
OUT_O="${2:?output .o}"
FN_NAME="${3:-p28_real_body}"
GO_SYM="${4:-main.P28RealBody}"
if [[ $ALL -eq 1 && -z "${4:-}" ]]; then
  GO_SYM="main"   # --all: GO_SYM is the go_sym prefix for every function
fi
if [[ $ALL -eq 1 && -n "${4:-}" && "$GO_SYM" != *.* ]]; then
  GO_SYM="${4}"   # explicit prefix (no dot): keep as-is
fi
PKG="${GO_SYM%%.*}"

CLANG="${GOC_CLANG:-${CLANG:-}}"
if [[ -z "$CLANG" ]]; then
  for c in \
    "$ROOT/third_party/llvm-19.1.7-clang-build/bin/clang" \
    "$ROOT/third_party/llvm-clang-build/bin/clang"; do
    if [[ -x "$c" ]]; then CLANG="$c"; break; fi
  done
  [[ -n "$CLANG" ]] || CLANG="${CLANG_FALLBACK:-clang-19}"
fi
# GOC_FRAMEADDR_MODE=gep (default): goc-reanchor rematerializes frame
# addresses as plain GEPs, which is only safe with goc-llc's post-RA
# GocFrameAddrFix pass (backend/pass/goc_llc.cpp). asm: the old opaque leaq
# per use; stock llc is enough.
GOC_LLC_BIN="$ROOT/backend/build/pass-out/goc-llc"
if [[ "${GOC_FRAMEADDR_MODE:-gep}" != "asm" ]]; then
  if [[ -z "${LLC:-}" && ! -x "$GOC_LLC_BIN" ]]; then
    make -C "$ROOT/backend/pass" "$GOC_LLC_BIN" >&2 || true
  fi
  if [[ -z "${LLC:-}" && ! -x "$GOC_LLC_BIN" ]]; then
    echo "realbody: FATAL missing $GOC_LLC_BIN; GOC_FRAMEADDR_MODE=gep needs its" \
         "frame-address fix (build it, or set GOC_FRAMEADDR_MODE=asm)" >&2
    exit 1
  fi
  LLC="${LLC:-$GOC_LLC_BIN}"
else
  LLC="${LLC:-llc-19}"
fi
MC="${LLVM_MC:-llvm-mc-19}"
OBJDUMP="${OBJDUMP:-llvm-objdump-19}"
OPT="${OPT:-opt-19}"
command -v "$OPT" >/dev/null 2>&1 || OPT="$HOME/tools/LLVM-19.1.7-Linux-X64/bin/opt"
LDLLD="${LDLLD:-ld.lld-19}"
INC="$ROOT/include"
OPT_LEVEL="${GOC_OPT_LEVEL:-0}"
case "$OPT_LEVEL" in
  0|1|2|3) ;;
  *) echo "realbody: GOC_OPT_LEVEL must be 0, 1, 2, or 3" >&2; exit 1 ;;
esac

TMP="${GOC_KEEP_TMP:-$(mktemp -d)}"
mkdir -p "$TMP"
cleanup() { [[ -n "${GOC_KEEP_TMP:-}" ]] || rm -rf "$TMP"; }
trap cleanup EXIT

LL="$TMP/body.ll"
if [[ "$IN" == *.ll ]]; then
  cp "$IN" "$LL"
else
  NATIVE_DEFS=(-DGOC_USE_INTREE_ATTRS)
  if ! "$CLANG" --version 2>/dev/null | rg -q 'clang version 19'; then
    NATIVE_DEFS=()
  fi
  # Go-stack compatible codegen: no red zone (morestack copies the frame),
  # no stack protector, no async unwind tables.
  OPT_FLAGS=("-O$OPT_LEVEL")
  if [[ "$OPT_LEVEL" == 0 ]]; then
    OPT_FLAGS+=(-Xclang -disable-O0-optnone)
  else
    # Unoptimized IR here. The opt step below inlines before stack maps.
    # -fno-inline would stamp noinline on every function and survive that step.
    OPT_FLAGS+=(-fno-omit-frame-pointer -mno-omit-leaf-frame-pointer
                -fno-optimize-sibling-calls -Xclang -disable-llvm-passes)
  fi
  "$CLANG" "${NATIVE_DEFS[@]}" -mno-red-zone -fno-stack-protector \
    -fno-asynchronous-unwind-tables -I "$INC" \
    -emit-llvm -S "${OPT_FLAGS[@]}" \
    -o "$LL" "$IN"
fi

# Go's ABI only guarantees 8-byte stack alignment at a call, while SysV codegen
# assumes 16 and emits aligned SSE accesses (movaps on a 16-byte JSValue copy ->
# #GP on a misaligned slot). Tell the backend the truth: clang spells this
# "override-stack-alignment" (i32 1 = override). Unlike -mstackrealign or an
# aligning thunk, the frame geometry stays fixed, so pcsp tables remain exact.
# A 16-byte-aligned alloca would still make LLVM realign the frame dynamically
# (`andq $-16, %rsp`), so the SP delta and every RSP-relative slot would depend
# on the caller's alignment -- pcsp and stack maps cannot express that. Every
# defined function therefore gets "no-realign-stack" (objects are clamped to
# the 8-byte stack alignment); the few aligned SSE moves LLVM still selects are
# made unaligned after instruction selection (see ALIGNFIX below).
python3 - "$LL" <<'PYFIX'
import re, sys
path = sys.argv[1]
ll = open(path).read()
if 'override-stack-alignment' not in ll:
    if '!llvm.module.flags = !{' in ll:
        ll = ll.replace('!llvm.module.flags = !{', '!llvm.module.flags = !{!9000, ', 1)
    else:
        ll += '\n!llvm.module.flags = !{!9000}\n'
    ll += '\n!9000 = !{i32 1, !"override-stack-alignment", i32 8}\n'
    print('realbody: override-stack-alignment=8 (no aligned SSE on the C stack)')
ll = re.sub(r'^(define [^\n]*)\{$',
            lambda m: m.group(1) if '"no-realign-stack"' in m.group(1)
            else m.group(1) + '"no-realign-stack" {', ll, flags=re.M)
open(path, 'w').write(ll)
PYFIX

MAGIC_OK=0
rg -q '28C0DE42|0x28c0de42|683728450' "$LL" && MAGIC_OK=1 || true

mkdir -p "$TMP/maps"
cp "$SELF/args_map.bin" "$TMP/maps/args_map.bin"
cp "$SELF/locals_map.bin" "$TMP/maps/locals_map.bin"

# Optional IR-level symbol redirects for compiler-emitted libc calls that -D
# macros cannot reach (e.g. @memcpy from struct copies):
#   GOC_IR_RENAMES="memcpy:goc_memcpy memset:goc_memset"
if [[ -n "${GOC_IR_RENAMES:-}" ]]; then
  python3 - "$LL" "$GOC_IR_RENAMES" <<'PYRENAME'
import re, sys
path, spec = sys.argv[1], sys.argv[2]
text = open(path).read()
for pair in spec.split():
    old, new = pair.split(":")
    text = re.sub(r"@" + re.escape(old) + r"(?![A-Za-z0-9_.$])", "@" + new, text)
open(path, "w").write(text)
PYRENAME
fi

GOABI_JSON="$TMP/goabi.json"
: > "$GOABI_JSON"
if [[ "$IN" != *.ll && "$OPT_LEVEL" != 0 ]]; then
  # Same contract as cmd/goc: inline first, record maps after. Strip the two
  # passes that rewrite internal SysV calls before those maps are taken.
  SMPASS="$ROOT/backend/build/pass-out/GocStackMap.so"
  [[ -f "$SMPASS" ]] || { echo "realbody: FATAL missing $SMPASS" >&2; exit 1; }
  "$OPT" -load-pass-plugin="$SMPASS" -passes=goc-inline-gate -S "$LL" -o "$TMP/body.gate.ll"
  cp "$TMP/body.gate.ll" "$LL"
  PIPELINE="$("$OPT" "-passes=default<O$OPT_LEVEL>" -print-pipeline-passes -disable-output /dev/null)"
  PIPELINE="${PIPELINE//,argpromotion/}"
  PIPELINE="${PIPELINE//,globalopt/}"
  "$OPT" "-passes=$PIPELINE" -S "$LL" -o "$TMP/body.opt.ll"
  cp "$TMP/body.opt.ll" "$LL"
  echo "realbody: O$OPT_LEVEL inlined before stack maps" >&2
fi
if [[ $GOABI -eq 1 ]]; then
  python3 "$SELF/goc_goabi.py" "$LL" "$TMP/body.impl.ll" "$TMP/thunks.s" "$GOABI_JSON"
  cp "$TMP/body.impl.ll" "$LL"
fi

# The MIR, object and assembly must come from the *same* IR. A stackmap pass
# after object emission changes neither its .llvm_stackmaps section nor the
# register allocation; silently continuing without real locations is unsafe.
if [[ $ALL -eq 1 && "${GOC_SPTR_MAPS:-0}" == "1" &&
      "${GOC_STACKMAP_PREPARED:-0}" != "1" ]]; then
  SMPASS="$ROOT/backend/build/pass-out/GocStackMap.so"
  [[ -f "$SMPASS" ]] || { echo "realbody: FATAL missing $SMPASS" >&2; exit 1; }
  "$OPT" -load-pass-plugin="$SMPASS" -passes=goc-stackmap -S "$LL" \
    -o "$TMP/body.sm.ll"
  [[ -s "$TMP/body.sm.ll" ]] || { echo "realbody: FATAL empty stackmap IR" >&2; exit 1; }
  LL="$TMP/body.sm.ll"
fi

LLC_OPT_LEVEL="$OPT_LEVEL"
if [[ -n "${GOC_LLC_OPT:-}" ]]; then
  LLC_OPT_LEVEL="$GOC_LLC_OPT"
fi
# Per-call Direct locations are the roots. Keep RBP valid at every call, and
# do not share a pointer slot with a later scalar: the previous call's map
# stays active until the next safepoint. Call-frame opt turns a reserved
# outgoing area into PUSH/POP around a call, so SP is not the constant pcsp
# claims and the unwinder reads g as a return PC.
# Tail merging would hoist a CALL shared by two blocks into a common tail and
# leave each stackmap record before a JMP; elfpack attaches a record to the
# next CALL in layout order, so the merged CALL would get another path's roots.
LLC_ARGS=("-O$LLC_OPT_LEVEL" -relocation-model=pic -march=x86-64
          -frame-pointer=all -enable-shrink-wrap=false -disable-tail-calls
          -no-stack-slot-sharing -no-x86-call-frame-opt -enable-tail-merge=false)
if [[ "${GOC_FIXED_G:-0}" == "1" ]]; then
  LLC_ARGS+=(-reserve-goc-r14)
  echo "realbody: GOC_FIXED_G=1 llc=$LLC" >&2
fi
"$LLC" "${LLC_ARGS[@]}" -filetype=asm \
  -o "$TMP/body.s" "$LL"

# ALIGNFIX: without realignment a frame is only 8-byte aligned, so aligned SSE
# memory moves (va_start's XMM save area, 16-byte JSValue copies) would #GP.
# The unaligned forms have identical encodings lengths and semantics. Any other
# packed SSE instruction with a non-constant-pool memory operand also demands
# alignment; refuse it instead of emitting code that faults at run time.
python3 - "$TMP/body.s" <<'ALIGNFIX'
import re, sys
path = sys.argv[1]
text = open(path).read()
for old, new in (("movaps", "movups"), ("movapd", "movupd"), ("movdqa", "movdqu")):
    text = re.sub(r'(?m)^(\s*)' + old + r'(\s[^\n]*\()', r'\g<1>' + new + r'\g<2>', text)
ok = re.compile(r'^(movs[sd]|mov[lh]p[sd]|movu|movq|movd|cvt|ucomis|comis|cmp[a-z]*s[sd]$|(add|sub|mul|div|sqrt|min|max)s[sd]$|pinsr|pextr)')
for line in text.splitlines():
    s = line.strip()
    if '%xmm' in s and '(' in s and '(%rip)' not in s and not s.startswith(('#', '.')):
        if not ok.match(s.split()[0]):
            raise SystemExit("realbody: FATAL alignment-requiring SSE memory operand "
                             "on an 8-byte-aligned Go stack: " + s)
open(path, 'w').write(text)
ALIGNFIX
"$MC" -filetype=obj -triple=x86_64-unknown-linux-gnu \
  -o "$TMP/body.llc.o" "$TMP/body.s"

ELF="$TMP/body.llc.o"
if [[ $GOABI -eq 1 ]]; then
  "$CLANG" -c "$TMP/thunks.s" -o "$TMP/thunks.o"
  "$LDLLD" -r -o "$TMP/merged.o" "$TMP/body.llc.o" "$TMP/thunks.o"
  ELF="$TMP/merged.o"
fi

if [[ $ALL -eq 1 ]]; then
  "$OBJDUMP" -dr "$ELF" > "$TMP/body.dis"
  # GOC_SPTR_MAPS: also dump MIR after PEI so the meta can carry the frame
  # offsets of the allocas that hold sptr values (see the report: the color
  # metadata marks *values*, not slots, so slots are derived from the stores
  # of sptr values, and their offsets from these stack objects).
  MIR="$TMP/body.mir"
  if [[ "${GOC_SPTR_MAPS:-0}" == "1" ]]; then
    "$LLC" "${LLC_ARGS[@]}" -stop-after=prologepilog \
      -o "$MIR" "$LL"
    [[ -s "$MIR" ]] || { echo "realbody: FATAL empty post-PEI MIR" >&2; exit 1; }
  fi
  python3 - "$LL" "$TMP/body.s" "$TMP/body.dis" "$TMP/meta.json" "$GO_SYM" "$GOABI_JSON" "$ABI0" "$TMP/body.mir" <<'PY'
import json, os, re, sys
sys.path.insert(0, os.path.join(os.environ["GOC_ROOT"], "backend", "realbody"))
from goc_goabi import aggregate_layout, parse_functions
ll_path, s_path, dis_path, out_path, sym_prefix, goabi_path, abi0_arg, mir_path = sys.argv[1:9]
abi0 = abi0_arg == "1"
no_nosplit = __import__("os").environ.get("GOC_NO_NOSPLIT") == "1"
ll = open(ll_path).read()
funcs = [m for m in re.findall(r'^define [^@]*@([A-Za-z_][A-Za-z0-9_.$]*)', ll, re.M)
         if not m.startswith('llvm.')]
internal = set()
for m in re.finditer(r'^define\s+([^@]*)@([A-Za-z_][A-Za-z0-9_.$]*)\(', ll, re.M):
    if re.search(r'\binternal\b', m.group(1)):
        internal.add(m.group(2))
tu = "tu"
sm = re.search(r'^source_filename\s*=\s*"([^"]+)"', ll, re.M)
if sm:
    tu = re.sub(r'[^A-Za-z0-9_]', '_', sm.group(1).rsplit('/', 1)[-1])
    tu = re.sub(r'\.(c|goc|ll|bc)$', '', tu)
goabi = {}
try:
    gj = json.load(open(goabi_path))
    for f in gj.get("functions", []):
        goabi[f["name"]] = f
except Exception:
    pass


def prologue_delta(text_lines):
    """RSP delta of the prologue: push rbp (+8) + optional pushes (+8) + sub N."""
    total, in_pro = 0, False
    for line in text_lines:
        t = line.strip()
        if not t or t.startswith('.') or t.startswith('#'):
            continue
        if re.match(r'^pushq\s+%', t):
            total += 8
        elif re.match(r'^movq\s+%rsp,\s*%rbp', t):
            pass
        elif re.match(r'^subq\s+\$', t) and '%rsp' in t:
            m2 = re.search(r'\$(0x[0-9a-fA-F]+|\d+)', t)
            total += int(m2.group(1), 0)
        else:
            break
        in_pro = True
    return total if in_pro else 0


frames, cur, body = {}, None, []
for line in open(s_path):
    m = re.match(r'^([A-Za-z_][A-Za-z0-9_.$]*):', line)
    if m:
        if cur is not None:
            frames[cur] = prologue_delta(body)
        cur, body = m.group(1), []
        continue
    if cur is not None:
        body.append(line)
if cur is not None:
    frames[cur] = prologue_delta(body)

# Code references with their function-relative offsets. llc resolves every
# reference to a .text symbol inside the same object itself (no relocation), so
# elfpack needs the offsets to hand them to the linker. The two sets are kept
# apart because only a CALL can grow the stack:
#   calls    — CALL opcodes, i.e. safepoints: the runtime reads a stack map
#              index at that PC (direct and indirect, relocated or baked);
#   branches — cross-function direct JMPs (tail jump, goabi thunk `jmp F.impl`)
#              and function-address LEAs (`leaq sym(%rip)`), whether already
#              ELF-relocated or baked: never safepoints, but their reference
#              must follow the target when the Go linker lays out the functions.
# Interior references are dropped because the whole body shifts uniformly;
# references to the function entry are retained so they target its TEXT symbol
# (including any inserted split preamble).
FN_RE = re.compile(r'^([0-9a-f]+) <([^>]+)>:')
INSN_RE = re.compile(r'^\s+([0-9a-f]+):\s+(?:[0-9a-f]{2} )+\s*(\S+)\s*(.*)$')
RELOC_RE = re.compile(r'^\s+([0-9a-f]+):\s+R_X86_64_(\S+)\s+(\S+)')
TGT_RE = re.compile(r'#\s*(0x[0-9a-f]+)\s*<([^>]+)>')
JMP_MNEMONIC = re.compile(r'^j[a-z]+$')

calls, branches = {}, {}
order = []           # function headers in address order (for the end addresses)
cur, cur_addr, insns = None, 0, []
dis = {}             # fn -> [(off, mnemonic, operands, target_addr, target_sym, reloc_sym)]


def flush():
    """Attach the pending instruction list to its function."""
    if cur is not None:
        dis[cur] = insns


for line in open(dis_path):
    m = FN_RE.match(line)
    if m:
        flush()
        cur, insns = m.group(2), []
        cur_addr = int(m.group(1), 16)
        order.append((cur_addr, cur))
        calls.setdefault(cur, [])
        branches.setdefault(cur, [])
        continue
    if cur is None:
        continue
    mr = RELOC_RE.match(line)
    if mr:
        # Relocation line: it belongs to the instruction just printed. Keep the
        # symbol name — it is the accurate target; the objdump comment of a
        # relocated operand only shows the unrelocated address.
        if insns:
            insns[-1][5] = re.sub(r'[+-]0x[0-9a-f]+$', '', mr.group(3))
        continue
    mi = INSN_RE.match(line)
    if not mi:
        continue
    mt = TGT_RE.search(line)
    # objdump instruction addresses are .text-relative, while elfpack expects
    # the CALL/JMP/LEA opcode offset within its function body.
    insns.append([int(mi.group(1), 16) - cur_addr, mi.group(2), mi.group(3),
                  int(mt.group(1), 16) if mt else None,
                  mt.group(2) if mt else None, None])
flush()

# Function end = the next symbol header in address order (alignment padding
# included: a target there is outside the function and must be relocated).
ends = {name: (order[i + 1][0] if i + 1 < len(order) else None)
        for i, (addr, name) in enumerate(order)}
starts = {name: addr for addr, name in order}

for name, ilist in dis.items():
    lo = starts[name]
    hi = ends.get(name)
    for off, mn, operands, tgt, tsym, reloc in ilist:
        if mn.startswith('call'):
            # Every CALL is a safepoint, so its offset always reaches the meta:
            # a direct call (baked or relocated) also needs the linker to own
            # its displacement, an indirect one has no baked reference to fix.
            calls[name].append({"callee": reloc or tsym or "*", "off": off})
            continue
        if reloc:
            # The ELF already carries this reference, so elfpack leaves the
            # relocation to the generic path — but direct JMP/LEA references
            # are still recorded here, so branches stays complete and separate
            # from call safepoints.
            if JMP_MNEMONIC.match(mn) or (
                mn.startswith('lea') and (reloc == '.text' or reloc in starts)
            ):
                branches[name].append({"callee": reloc, "off": off})
            continue
        if tgt is None:
            continue                      # no reference operand
        if tgt > lo and (hi is None or tgt < hi):
            continue                      # interior shifts with body; entry needs its TEXT symbol
        if JMP_MNEMONIC.match(mn) or mn.startswith('lea'):
            branches[name].append({"callee": tsym or "", "off": off})
        elif '(%rip)' in operands:
            raise SystemExit(
                "realbody: %s+0x%x: baked RIP-relative reference to %s is neither "
                "a call, jump nor lea; elfpack cannot re-relocate it" % (name, off, tsym))


# Stack maps: the runtime refuses to copy a frame that has locals but no
# FUNCDATA_LocalsPointerMaps ("missing stackmap"). A frame whose function holds
# no sptr value is pointer-free for the GC, so an empty map is *correct* there;
# functions with sptr keep index -1 and the runtime fails loudly instead of
# silently leaving stale stack pointers behind.
sptr_nodes = set(re.findall(r'^!(\d+) = !\{!"sptr"\}\s*$', ll, re.M))
sptr_ref = re.compile(r'!goc\.color !(?:%s)\b' % '|'.join(sorted(sptr_nodes, key=int))) if sptr_nodes else None
has_sptr = {}
for chunk in ll.split('\ndefine ')[1:]:
    mname = re.match(r'[^@]*@([A-Za-z_][A-Za-z0-9_.$]*)', chunk)
    if not mname or mname.group(1).startswith('llvm.'):
        continue
    has_sptr[mname.group(1)] = bool(sptr_ref and sptr_ref.search(chunk))


# --- sptr frame slots (GOC_SPTR_MAPS) ---------------------------------------
# The coloring marks *values* (!goc.color), so the slots that must be adjusted
# when a stack grows are found by following the stores of sptr values into
# allocas, then looking those allocas up in the post-PEI MIR stack objects.
def _mir_slots(path):
    """{function name: {alloca name: LLVM CFA-relative offset}}"""
    out, cur, lines = {}, None, None
    try:
        txt = open(path).read()
    except OSError:
        return out
    for line in txt.splitlines():
        m = re.match(r'^name:\s+(\S+)\s*$', line)
        if m:
            cur = m.group(1)
            lines = []
            out[cur] = lines
            continue
        if cur is None:
            continue
        if re.match(r'^\s*- \{ id:', line):
            nm = re.search(r'name: ([^,]+)', line)
            off = re.search(r'offset: (-?\d+)', line)
            if nm and off:
                lines.append((nm.group(1), int(off.group(1))))
    return out


def _sptr_slots(ll, sptr_nodes, mir):
    """Per function: positive BP distances of frame words holding sptr values."""
    if not mir:
        return {}
    # GOC_CSR_ADJUST: a live address llc kept in a callee-saved register has
    # no post-PEI slot. The morestack stub rewrites that register if it
    # points into the old stack. An unmarked spill of the same value is
    # still invisible; the growth smoke is the check, not this scanner.
    csr_adjust = os.environ.get("GOC_CSR_ADJUST") == "1"
    dropped = []
    # goc.anchor / goc.spill.root are uninitialized until the store after
    # their def. They are not bits in the function-wide map: a copy before
    # that store sees a small integer and the copier rejects it ("bad pointer
    # in frame"). The per-call Direct locations, read from -stackmap-elf, OR
    # them in only at calls the store dominates.
    def compiler_root(name):
        return (name.startswith("goc.anchor") or name.startswith("goc.arganchor")
                or name.startswith("goc.spill.root"))
    ref = re.compile(r'!goc\.color !(?:%s)\b' % '|'.join(sorted(sptr_nodes, key=int))
                     if sptr_nodes else r'(?!)')
    # The IR pass tags stores through an address of an aggregate alloca *before*
    # it replaces that address with a load from goc.spill.root. The tag carries
    # the underlying alloca name and the exact byte offset within it; the MIR
    # supplies the physical frame offset, so no LLVM type-layout guessing or
    # post-rewrite alias inference is needed here.
    frame_words = {}
    for m in re.finditer(r'^!(\d+) = !\{!"goc\.frame\.words", !"([^"]+)"((?:, i64 \d+)+)\}\s*$', ll, re.M):
        frame_words[int(m.group(1))] = (m.group(2),
                                      [int(n) for n in re.findall(r'i64 (\d+)', m.group(3))])
    slots = {}
    for chunk in ll.split('\ndefine ')[1:]:
        mname = re.match(r'[^@]*@([A-Za-z_][A-Za-z0-9_.$]*)', chunk)
        if not mname or mname.group(1).startswith('llvm.'):
            continue
        name = mname.group(1)
        sptr_vals = set()
        # pointer-typed only: integer-typed sptr values (uintptr_t etc.) must not
        # turn their slot into a "pointer" slot (see GocStackMap.cpp).
        for m in re.finditer(r'^\s*(%\S+) = ([a-z]+)[^\n]*' + ref.pattern + r'', chunk, re.M):
            if m.group(2) in ('alloca', 'load', 'getelementptr', 'bitcast', 'select', 'phi', 'call', 'inttoptr'):
                sptr_vals.add(m.group(1))
        for m in re.finditer(r'^\s*(%\S+) = [a-z]+\s+ptr\b[^\n]*' + ref.pattern + r'', chunk, re.M):
            sptr_vals.add(m.group(1))
        # A store through a loaded pointer (e.g. *pptr in dtoa_malloc)
        # writes the caller's stack, not an alloca in this frame. Only a
        # genuine local alloca can supply a fallback frame slot here;
        # aggregate fields already carry precise goc.frame.words tags.
        held = set(re.findall(r'^\s*(%\S+) = alloca\b', chunk, re.M))
        # Pointer allocas and pointer fields within aggregate allocas. A
        # generated reload address is *not* itself an alloca; its tag resolves
        # the original address instead of accidentally mapping a C register.
        targets = set()
        for tagged in re.finditer(r'!goc\.frame\.words !(\d+)\b', chunk):
            word = frame_words.get(int(tagged.group(1)))
            if word is None:
                raise SystemExit("realbody: FATAL unknown frame-word annotation %s in %s" %
                                 (tagged.group(1), name))
            # Compiler roots stay out of the function-wide map. A spill root
            # can hold &s->token, but the same name also holds small integers
            # in other functions; the copier then rejects 0x1/0x60 ("bad
            # pointer in frame"). Per-call maps are the right place for them.
            # The asm object's .llvm_stackmaps names only 120 of 776 functions;
            # the rest have an empty reloc symbol, so they cannot be applied yet.
            if compiler_root(word[0]):
                continue
            targets.update((word[0], offset) for offset in word[1])
        for m in re.finditer(r'^\s*store (?:volatile )?(?:ptr )?(%\S+), ptr (%\S+),[^\n]*', chunk, re.M):
            slot = m.group(2).lstrip('%')
            if ('!goc.frame.words' not in m.group(0) and m.group(1) in sptr_vals
                    and m.group(2) in held and not compiler_root(slot)):
                targets.add((slot, 0))
        if not targets:
            continue
        obj = {n: o for n, o in mir.get(name, [])}
        # LLVM's post-PEI stack-object offsets use the CFA (entry SP+8),
        # which is RBP+16 after PUSH RBP. E.g. MIR `p` offset -32 becomes
        # -16(%rbp) in the emitted body. Go's varp is RBP, so report the
        # positive RBP distance, not the raw CFA distance.
        offs = set()
        for t, byte_offset in targets:
            if t not in obj:
                # llc dropped the slot. That is safe only when nothing reads it:
                # reanchor moved the live address into an anchor that does have
                # a slot, and the original alloca is stores-only. A load with no
                # slot is a name mismatch or a register promotion we cannot map.
                # A GEP used only to zero the slot is not a read; llc deletes
                # that store with the slot. LLVM names include '.', so %p1 must
                # not match %p1.0.
                end = r'(?![A-Za-z0-9_.$])'
                live = re.search(r'load\s+[^,\n]+,\s*ptr\s+%' + re.escape(t) + end, chunk)
                if not live:
                    for gm in re.finditer(
                            r'^\s*(%\S+) = getelementptr[^\n]*%' + re.escape(t) + end,
                            chunk, re.M):
                        if re.search(r'load\s+[^,\n]+,\s*ptr\s+' + re.escape(gm.group(1)) + end,
                                     chunk):
                            live = True
                            break
                if live:
                    if csr_adjust:
                        dropped.append("%s:%s" % (name, t))
                        continue
                    raise SystemExit("realbody: FATAL sptr alloca %s in %s has no post-PEI slot" % (t, name))
                continue
            off = -obj[t] - 16 - byte_offset
            if off < 8 or off % 8:
                raise SystemExit("realbody: FATAL sptr alloca %s+%d in %s has non-pointer BP offset %d" %
                                 (t, byte_offset, name, off))
            offs.add(off)
        offs = sorted(offs)
        if offs:
            slots[name] = offs
    if dropped:
        print("realbody: csr-adjust dropped %d unmapped sptr allocas (first %s)" % (
            len(dropped), ", ".join(dropped[:8])), file=sys.stderr)
    return slots


sptr_nodes = set(re.findall(r'^!(\d+) = !\{!"sptr"\}\s*$', ll, re.M))
mir_slots = _mir_slots(mir_path) if mir_path and os.path.exists(mir_path) else {}
sptr_slots = _sptr_slots(ll, sptr_nodes, mir_slots) if mir_slots else {}


def sysv_pointer_registers(info):
    """Pointer-typed SysV GPR arguments to adjust when the slow stub grows."""
    regs = ("rdi", "rsi", "rdx", "rcx", "r8", "r9")
    used = sse_used = 0
    ptrs = []
    for ty, attrs in zip(info["params"], info["param_abi_attrs"]):
        if "byval" in attrs:
            continue  # SysV passes the aggregate in the caller's stack area.
        if attrs and any(a != "sret" for a in attrs):
            raise SystemExit("realbody: FATAL unmodelled SysV entry attribute in %s: %s" %
                             (info["name"], attrs))
        if ty == "ptr" or re.fullmatch(r'i(?:1|8|16|32|64)', ty):
            if used < len(regs):
                if ty == "ptr":
                    ptrs.append(regs[used])
                used += 1
        elif ty == "i128":
            if used <= len(regs) - 2:
                used += 2
        elif ty in ("float", "double"):
            if sse_used < 8:
                sse_used += 1
        else:
            aggregate, reason = aggregate_layout(ty, "parameter")
            if not aggregate or reason:
                raise SystemExit("realbody: FATAL unmodelled SysV entry type in %s: %s (%s)" %
                                 (info["name"], ty, reason))
            fields = aggregate["fields"]
            gpr_need = sum(field["bank"] == "int" for field in fields)
            sse_need = len(fields) - gpr_need
            if aggregate["size"] <= 16 and used + gpr_need <= 6 and sse_used + sse_need <= 8:
                for field in fields:
                    if field["bank"] == "int":
                        if field["type"] == "ptr":
                            ptrs.append(regs[used])
                        used += 1
                    else:
                        sse_used += 1
    return ptrs


sysv_ptrs = {info["name"]: sysv_pointer_registers(info)
             for info in parse_functions(ll, "define")}


def entry(mir_name, go_name, frame, abi="", sptr=None, arg_spills=None,
          arg_area=0, stack_ptr_args=None):
    e = {
        "mir_name": mir_name,
        "go_sym": go_name,
        "frame": frame,
        "flags": ("noframe" if no_nosplit else "nosplit"),
        "encoding": "clang-real-isel",
        "calls": [{"callee": c["callee"], "off": c["off"], "stackmap_index": -1}
                  for c in calls.get(mir_name, [])],
        "branches": [{"callee": b["callee"], "off": b["off"]}
                     for b in branches.get(mir_name, [])],
        "has_sptr": has_sptr.get(mir_name, True) if sptr is None else sptr,
        "sptr_slots": sptr_slots.get(mir_name, []),
        "sysv_pointer_regs": sysv_ptrs.get(mir_name, []),
    }
    if abi:
        e["abi"] = abi
    if abi == "ABI0" and mir_name not in sysv_ptrs:
        raise SystemExit("realbody: FATAL missing C signature for %s" % mir_name)
    if arg_spills is not None:
        e["go_arg_spills"] = arg_spills
        e["go_arg_area"] = arg_area
        e["go_stack_ptr_args"] = stack_ptr_args
    return e


def go_args_layout(info):
    """Stack arguments plus ABIInternal register spills in the caller area."""
    off = info["stack_args_size"]  # pointer-aligned stack arguments end here
    spills = []
    stack_ptrs = []
    for param in info["params"]:
        loc = param["location"]
        if loc["kind"] != "register":
            if param["type"] == "ptr":
                stack_ptrs.append(loc["offset"])
            continue
        ty = param["type"]
        size = {"ptr": 8, "float": 4, "double": 8, "i1": 1,
                "i8": 1, "i16": 2, "i32": 4, "i64": 8}[ty]
        off = (off + size - 1) & -size
        spills.append({"reg": loc["register"], "off": 8 + off,
                       "size": size, "ptr": ty == "ptr"})
        off += size
    return spills, stack_ptrs, (off + 7) & -8


mfns = []
# In --goabi mode the C-side symbols are ABI0 (SysV: C calls them directly,
# cross-TU refs are ABI0) and only the Go-facing thunks are ABIInternal.
c_abi = "ABI0" if goabi else ""
# GOC_CRESERVE reserves C stack inside the thunk: the frame is then a constant
# 8+reserve (so elfpack emits the split check and an exact pcsp) and the C call
# tree never triggers morestack — C frames with unions (JSValue) cannot have
# precise pointer maps, so they must not be copied.
thunk_reserve = int(__import__("os").environ.get("GOC_CRESERVE", "0") or "0")
thunk_frame = (8 + thunk_reserve) if thunk_reserve > 0 else 0
for name in goabi:
    spills, stack_ptrs, arg_area = go_args_layout(goabi[name]["abi"]["go"])
    e = entry(name, f"{sym_prefix}.{name}", thunk_frame, "ABIInternal",
              sptr=False, arg_spills=spills, arg_area=arg_area,
              stack_ptr_args=stack_ptrs)
    # The thunk copies SysV stack arguments to the bottom of its reserve
    # (RSP = RBP-reserve) before calling X.impl. If X.impl grows the stack in
    # its prologue, a copied pointer (possibly a Go stack address) must be
    # adjusted through the thunk's own frame map.
    e["sptr_slots"] = sorted(thunk_reserve - p["location"]["offset"]
                             for p in goabi[name]["abi"]["sysv"]["params"]
                             if p["location"]["kind"] == "stack" and p["type"] == "ptr")
    mfns.append(e)
for f in funcs:
    # File-local (static) functions may share a name across TUs, so qualify
    # them with the TU (from source_filename) to keep go_syms unique.
    if f in internal:
        mfns.append(entry(f, f"{sym_prefix}.{tu}.{f}", frames.get(f, 0), c_abi))
    else:
        mfns.append(entry(f, f"{sym_prefix}.{f}", frames.get(f, 0), c_abi))

meta = {
    "producer": "goc-p29-realbody-all",
    "pipeline": "clang.c→real IR→llc ISel→elfpack (not P21 seed templates)",
    "not_source": "p21-color-vertical seed templates",
    "maps_status": "unavailable-real-mf: stackmap_index=-1, no pointer maps (P29 gap)",
    "goabi": ("int/ptr subset thunks" if goabi else "off"),
    "tu": tu,
    "abi": ("ABI0" if abi0 else "ABIInternal"),
    "functions": mfns,
    "mircanon": {"mode": "identity", "transforms": [], "cfg_rewrite": False,
                 "frame_inject": False, "dialect_strip": False},
}
json.dump(meta, open(out_path, "w"), indent=2)
print("realbody --all: %d TEXT entries; frames=%s" %
      (len(mfns), {f["mir_name"]: f["frame"] for f in mfns}))
PY
else
  rg -q "define .* @$FN_NAME" "$LL"
  CALLS_JSON='[]'
  if nm "$ELF" 2>/dev/null | rg -q 'U p28_external_hook' \
    || "$OBJDUMP" -d "$ELF" 2>/dev/null | rg -q 'p28_external_hook'; then
    CALLS_JSON='[{"callee":"p28_external_hook","stackmap_index":0}]'
  fi
  FRAME="$(python3 - "$TMP/body.s" "$FN_NAME" <<'PY'
import re, sys
s_path, fn = sys.argv[1], sys.argv[2]
frames, cur, body = {}, None, []
for line in open(s_path):
    m = re.match(r'^([A-Za-z_][A-Za-z0-9_.$]*):', line)
    if m:
        if cur is not None:
            frames[cur] = body
        cur, body = m.group(1), []
        continue
    if cur is not None:
        body.append(line)
if cur is not None:
    frames[cur] = body
total, in_pro = 0, False
for line in frames.get(fn, []):
    t = line.strip()
    if not t or t.startswith('.') or t.startswith('#'):
        continue
    if re.match(r'^pushq\s+%', t):
        total += 8
    elif re.match(r'^movq\s+%rsp,\s*%rbp', t):
        pass
    elif re.match(r'^subq\s+\$', t) and '%rsp' in t:
        m2 = re.search(r'\$(0x[0-9a-fA-F]+|\d+)', t)
        total += int(m2.group(1), 0)
    else:
        break
    in_pro = True
print(total if in_pro else 24)
PY
)"
  cat > "$TMP/meta.json" << JSON
{
  "producer": "goc-p28-realbody",
  "pipeline": "clang.c→real IR→llc ISel→elfpack (not P21 seed templates)",
  "not_source": "p21-color-vertical seed templates",
  "functions": [
    {
      "mir_name": "$FN_NAME",
      "go_sym": "$GO_SYM",
      "frame": $FRAME,
      "flags": "nosplit",
      "encoding": "clang-real-isel",
      "calls": $CALLS_JSON,
      "branches": []
    }
  ],
  "mircanon": {
    "mode": "identity",
    "transforms": [],
    "cfg_rewrite": false,
    "frame_inject": false,
    "dialect_strip": false
  }
}
JSON
fi

rg -q '"encoding": "clang-real-isel"' "$TMP/meta.json"
rg -q 'not_source' "$TMP/meta.json"
if rg -q 'attrs→seedMIR|seedMIR→Spill' "$TMP/meta.json"; then
  echo "FATAL: meta looks like P21 seedMIR pipeline" >&2
  exit 1
fi

( cd "$P5" && go build -o "$TMP/elfpack" ./goobj/elfpack/ )
"$TMP/elfpack" -elf "$ELF" -meta "$TMP/meta.json" \
  -maps "$TMP/maps" -out-o "$OUT_O" -p "$PKG"

if [[ $MAGIC_OK -eq 1 ]]; then
  python3 - "$OUT_O" <<'PY'
import struct, sys
from pathlib import Path
b = Path(sys.argv[1]).read_bytes()
pat = struct.pack("<I", 0x28C0DE42)
if pat not in b:
    raise SystemExit("FAIL: magic 0x28C0DE42 not in goobj TEXT — body not from source")
print("P28-proof: magic 0x28C0DE42 present in goobj (real ISel body)")
PY
fi

cp "$TMP/meta.json" "${OUT_O%.o}.meta.json"
cp "$LL" "${OUT_O%.o}.ll"
if [[ $ALL -eq 1 ]]; then
  NTEXT="$(go tool nm "$OUT_O" | rg -c ' T ' || true)"
  echo "P29-realbody-all: wrote $OUT_O (encoding=clang-real-isel, goabi=$GOABI, TEXT syms=$NTEXT)"
else
  echo "P28-realbody: wrote $OUT_O (encoding=clang-real-isel)"
fi
