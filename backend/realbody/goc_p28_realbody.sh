#!/usr/bin/env bash
# P28/P29: real Clang IR body → llc ISel → elfpack goobj (NO P21 seedMIR).
# Usage: goc_p28_realbody.sh <input.ll|.c> <out.goobj.o> [fn_name] [go_sym] [--all] [--goabi]
#   --all:   emit TEXT for every defined function in the TU (multi-function, P29);
#            frames derived from each llc prologue, per-function CALL lists.
#            Real-MF pointer maps are not available yet → stackmap_index=-1.
#   --goabi: Go ABIInternal entry thunks for the three call paths that exist
#            today (Go→C thunk, C→C on the platform ABI, C→Go .goabi).
#            GOC_ARCH=amd64 (default): SysV rdi/rsi/...  arm64: AAPCS64 x0–x7.
#            Each eligible F is renamed F.impl and gains a thunk F.
set -euo pipefail
SELF="$(cd "$(dirname "$0")" && pwd)"
ROOT="${GOC_ROOT:-$(cd "$SELF/../.." && pwd)}"
export GOC_ROOT="$ROOT"
P5="$ROOT/backend"
# shellcheck source=scripts/goc-product-lib.sh
source "$ROOT/scripts/goc-product-lib.sh"

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
# --all's fourth argument is the full Go package prefix. An import path
# may contain dots (example.com/foo). A single-function go_sym is main.Name,
# and the package is the part before the first dot.
if [[ $ALL -eq 1 ]]; then
  PKG="$GO_SYM"
else
  PKG="${GO_SYM%%.*}"
fi

CLANG="$(goc_resolve_clang)"
goc_prepend_lib "$(goc_clang_libdir "$CLANG")"
# Default stays linux/amd64. arm64 is a separate ISel, not a -march switch.
# gep remat is unsafe on arm64 until GocFrameAddrFix is ported, and the x86
# leaq asm does not assemble, so arm64 forces the stock-llc path and refuses
# stack maps. g is x28, reserved by llc; there is no TLS load.
GOC_ARCH="${GOC_ARCH:-amd64}"
case "$GOC_ARCH" in
  amd64|arm64) ;;
  *) echo "realbody: FATAL GOC_ARCH=$GOC_ARCH (want amd64 or arm64)" >&2; exit 1 ;;
esac
export GOC_ARCH
if [[ "$GOC_ARCH" == arm64 ]]; then
  export GOC_FRAMEADDR_MODE=asm
  if [[ "${GOC_SPTR_MAPS:-0}" == 1 || "${GOC_CSR_ADJUST:-0}" == 1 ]]; then
    echo "realbody: FATAL GOC_SPTR_MAPS and GOC_CSR_ADJUST are x86-only; arm64 has no frame-address repair" >&2
    exit 1
  fi
  case "${LLC:-}" in
    *goc-llc*|"")
      LLC="$(goc_beside llc 2>/dev/null || true)"
      if [[ -z "$LLC" ]]; then
        LLC="$(command -v llc-19 || command -v llc || true)"
      fi
      [[ -n "$LLC" && -x "$LLC" ]] || {
        echo "realbody: FATAL no llc with an AArch64 target beside the patched clang" >&2
        exit 1
      }
      ;;
  esac
  echo "realbody: GOC_ARCH=arm64 llc=$LLC (x28 reserved; frame-address repair not ported)" >&2
fi
# GOC_FRAMEADDR_MODE=gep (default): goc-reanchor rematerializes frame
# addresses as plain GEPs, which is only safe with goc-llc's post-RA
# GocFrameAddrFix pass (backend/pass/goc_llc.cpp). asm: the old opaque leaq
# per use; stock llc is enough.
if [[ -x "$ROOT/passes/goc-llc" ]]; then
  GOC_LLC_BIN="$ROOT/passes/goc-llc"
else
  GOC_LLC_BIN="$ROOT/backend/build/pass-out/goc-llc"
fi
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
  if [[ -z "${LLC:-}" ]]; then
    LLC="$(goc_beside llc 2>/dev/null || true)"
    [[ -n "$LLC" ]] || LLC="$(command -v llc-19 || command -v llc || true)"
  fi
  [[ -n "${LLC:-}" ]] || { echo "realbody: FATAL llc missing" >&2; exit 1; }
fi
# gep remat is a plain GEP. Stock llc CSEs it into a callee-saved register
# and keeps that register across the call; tail duplication makes that
# live range common. Only goc-llc's post-RA GocFrameAddrFix re-derives it.
# A pre-set LLC=llc-19 used to skip the missing-binary check above.
if [[ "${GOC_FRAMEADDR_MODE:-gep}" != "asm" ]]; then
  if ! grep -a -q 'goc-frameaddr-fix' "$LLC" 2>/dev/null; then
    echo "realbody: FATAL $LLC has no GocFrameAddrFix; frame-address GEPs would stay stale across calls. Build backend/build/pass-out/goc-llc, or set GOC_FRAMEADDR_MODE=asm." >&2
    exit 1
  fi
fi
if [[ -z "${LLVM_MC:-}" || ! -x "${LLVM_MC:-}" ]]; then
  LLVM_MC="$(goc_beside llvm-mc 2>/dev/null || true)"
  if [[ -z "$LLVM_MC" ]]; then
    LLVM_MC="$(command -v llvm-mc-19 || command -v llvm-mc || true)"
  fi
fi
[[ -n "${LLVM_MC:-}" && -x "$LLVM_MC" ]] || {
  echo "realbody: FATAL llvm-mc missing. See docs/build-from-source.md" >&2
  exit 1
}
MC="$LLVM_MC"
if [[ -z "${OBJDUMP:-}" || ! -x "${OBJDUMP:-}" ]]; then
  OBJDUMP="$(goc_beside llvm-objdump 2>/dev/null || true)"
  if [[ -z "$OBJDUMP" ]]; then
    OBJDUMP="$(command -v llvm-objdump-19 || command -v llvm-objdump || true)"
  fi
fi
[[ -n "${OBJDUMP:-}" && -x "$OBJDUMP" ]] || {
  echo "realbody: FATAL llvm-objdump missing. See docs/build-from-source.md" >&2
  exit 1
}
if [[ -z "${OPT:-}" || ! -x "${OPT:-}" ]]; then
  OPT="$(goc_beside opt 2>/dev/null || true)"
  if [[ -z "$OPT" ]]; then
    OPT="$(command -v opt-19 || command -v opt || true)"
  fi
fi
[[ -n "${OPT:-}" && -x "$OPT" ]] || {
  echo "realbody: FATAL opt missing. See docs/build-from-source.md" >&2
  exit 1
}
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
if [[ "$GOC_ARCH" == arm64 && "$IN" == *.c ]]; then
  # runtime/uptr is the one TU that has an aarch64 path. Every other file
  # that still contains the amd64 host sequence is refused rather than
  # compiled into a binary that reads FS or traps in syscall.
  case "$IN" in
    *runtime/uptr/*) ;;
    *)
      if grep -E -q 'movq[[:space:]]+%%fs:-8|"syscall"' "$IN"; then
        echo "realbody: FATAL $IN is a linux/amd64 host (movq %%fs:-8 or syscall) and is not ported to arm64" >&2
        exit 1
      fi
      ;;
  esac
fi
if [[ "$IN" == *.ll ]]; then
  cp "$IN" "$LL"
else
  NATIVE_DEFS=(-DGOC_USE_INTREE_ATTRS)
  if ! "$CLANG" --version 2>/dev/null | grep -q 'clang version 19'; then
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
  TARGET_ARGS=()
  if [[ "$GOC_ARCH" == arm64 ]]; then
    TARGET_ARGS=(-target aarch64-unknown-linux-gnu -ffixed-x28)
  fi
  "$CLANG" "${NATIVE_DEFS[@]}" "${TARGET_ARGS[@]}" -mno-red-zone -fno-stack-protector \
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
# amd64 Go calls are only 8-byte aligned, so the x86 backend must be told.
# arm64 Go requires 16-byte SP; do not inject the x86 override. The
# no-realign-stack attribute still applies: a dynamic AND of SP would make
# pcsp unusable on either architecture.
LOWER="$(goc_lower_bin)"
"$LOWER" fix-ir "$LL"

MAGIC_OK=0
if grep -qE '28C0DE42|0x28c0de42|683728450' "$LL"; then
  MAGIC_OK=1
fi

mkdir -p "$TMP/maps"
cp "$SELF/args_map.bin" "$TMP/maps/args_map.bin"
cp "$SELF/locals_map.bin" "$TMP/maps/locals_map.bin"

# Optional IR-level symbol redirects for compiler-emitted libc calls that -D
# macros cannot reach (e.g. @memcpy from struct copies):
#   GOC_IR_RENAMES="memcpy:goc_memcpy memset:goc_memset"
if [[ -n "${GOC_IR_RENAMES:-}" ]]; then
  "$LOWER" rename-ir "$LL" "$GOC_IR_RENAMES"
fi

GOABI_JSON="$TMP/goabi.json"
: > "$GOABI_JSON"
if [[ "$IN" != *.ll && "$OPT_LEVEL" != 0 ]]; then
  # Same contract as cmd/goc: inline first, record maps after. Strip the two
  # passes that rewrite internal SysV calls before those maps are taken.
  if [[ -n "${GOC_STACKMAP:-}" && -f "$GOC_STACKMAP" ]]; then
    SMPASS="$GOC_STACKMAP"
  elif [[ -f "$ROOT/passes/GocStackMap.so" ]]; then
    SMPASS="$ROOT/passes/GocStackMap.so"
  else
    SMPASS="$ROOT/backend/build/pass-out/GocStackMap.so"
  fi
  [[ -f "$SMPASS" ]] || { echo "realbody: FATAL missing $SMPASS" >&2; exit 1; }
  "$OPT" -load-pass-plugin="$SMPASS" -passes=goc-inline-gate -S "$LL" -o "$TMP/body.gate.ll"
  cp "$TMP/body.gate.ll" "$LL"
  PIPELINE="$("$OPT" "-passes=default<O$OPT_LEVEL>" -print-pipeline-passes -disable-output /dev/null)"
  PIPELINE="${PIPELINE//,argpromotion/}"
  PIPELINE="${PIPELINE//,globalopt/}"
  # GOC_OPT_EXTRA: extra opt flags for experiments (default empty), e.g.
  # "-inline-threshold=250": the textual pipeline above does not carry
  # default<O3>'s inliner threshold (docs/perf-gap.md).
  read -r -a _goc_opt_extra <<<"${GOC_OPT_EXTRA:-}"
  "$OPT" "-passes=$PIPELINE" "${_goc_opt_extra[@]}" -S "$LL" -o "$TMP/body.opt.ll"
  cp "$TMP/body.opt.ll" "$LL"
  echo "realbody: O$OPT_LEVEL inlined before stack maps" >&2
fi
if [[ $GOABI -eq 1 ]]; then
  "$LOWER" goabi "$LL" "$TMP/body.impl.ll" "$TMP/thunks.s" "$GOABI_JSON"
  cp "$TMP/body.impl.ll" "$LL"
fi

# The MIR, object and assembly must come from the *same* IR. A stackmap pass
# after object emission changes neither its .llvm_stackmaps section nor the
# register allocation; silently continuing without real locations is unsafe.
if [[ $ALL -eq 1 && "${GOC_SPTR_MAPS:-0}" == "1" &&
      "${GOC_STACKMAP_PREPARED:-0}" != "1" ]]; then
  if [[ -n "${GOC_STACKMAP:-}" && -f "$GOC_STACKMAP" ]]; then
    SMPASS="$GOC_STACKMAP"
  elif [[ -f "$ROOT/passes/GocStackMap.so" ]]; then
    SMPASS="$ROOT/passes/GocStackMap.so"
  else
    SMPASS="$ROOT/backend/build/pass-out/GocStackMap.so"
  fi
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
# Per-call Direct locations are the roots. Keep RBP valid at every call.
# Spill slots share (no -no-stack-slot-sharing): root allocas are not spill
# slots, and goc-reanchor keeps their lifetime markers off so a scalar cannot
# take a stackmap slot. Call-frame opt turns a reserved outgoing area into
# PUSH/POP around a call, so SP is not the constant pcsp claims and the
# unwinder reads g as a return PC.
# Tail merging would hoist a CALL shared by two blocks into a common tail and
# leave each stackmap record before a JMP; elfpack attaches a record to the
# next CALL in layout order, so the merged CALL would get another path's roots.
# Tail duplication is on: QuickJS dispatch blocks are shared tails, and gcc
# duplicates them. Stock llc accepts the thresholds on both arches.
if [[ "$GOC_ARCH" == arm64 ]]; then
  # No -march=x86-64, no -no-x86-call-frame-opt, no -reserve-goc-r14.
  # +reserve-x28 keeps g live across the body. AArch64 has no red zone.
  LLC_ARGS=("-O$LLC_OPT_LEVEL" -relocation-model=pic
            -mtriple=aarch64-unknown-linux-gnu -mattr=+reserve-x28
            -frame-pointer=all -enable-shrink-wrap=false -disable-tail-calls
            -enable-tail-merge=false
            -tail-dup-pred-size=1000 -tail-dup-succ-size=1000)
else
  LLC_ARGS=("-O$LLC_OPT_LEVEL" -relocation-model=pic -march=x86-64
            -frame-pointer=all -enable-shrink-wrap=false -disable-tail-calls
            -no-x86-call-frame-opt -enable-tail-merge=false
            -tail-dup-pred-size=1000 -tail-dup-succ-size=1000)
fi
# GOC_LLC_EXTRA: extra llc flags for experiments (default empty).
if [[ -n "${GOC_LLC_EXTRA:-}" ]]; then
  read -r -a _goc_llc_extra <<<"$GOC_LLC_EXTRA"
  LLC_ARGS+=("${_goc_llc_extra[@]}")
fi
if [[ "$GOC_ARCH" == arm64 ]]; then
  if [[ "${GOC_FIXED_G:-0}" == "1" ]]; then
    echo "realbody: note GOC_FIXED_G is implicit on arm64 (x28 reserved); not passing -reserve-goc-r14" >&2
  fi
elif [[ "${GOC_FIXED_G:-0}" == "1" ]]; then
  LLC_ARGS+=(-reserve-goc-r14)
  echo "realbody: GOC_FIXED_G=1 llc=$LLC" >&2
fi
"$LLC" "${LLC_ARGS[@]}" -filetype=asm \
  -o "$TMP/body.s" "$LL"

# ALIGNFIX: without realignment an amd64 frame is only 8-byte aligned, so
# aligned SSE memory moves would #GP. arm64 has no SSE and keeps 16-byte SP,
# so this rewrite does not apply.
if [[ "$GOC_ARCH" != arm64 ]]; then
  "$LOWER" align-asm "$TMP/body.s"
fi
MC_TRIPLE=x86_64-unknown-linux-gnu
if [[ "$GOC_ARCH" == arm64 ]]; then
  MC_TRIPLE=aarch64-unknown-linux-gnu
fi
# Go thunks are appended to the same assembly and assembled once. That puts
# both .text contributions in one relocatable object, so the driver does not
# call ld or ld.lld.
ASM_IN="$TMP/body.s"
if [[ $GOABI -eq 1 ]]; then
  cat "$TMP/body.s" "$TMP/thunks.s" > "$TMP/body.thunks.s"
  ASM_IN="$TMP/body.thunks.s"
fi
"$MC" -filetype=obj -triple="$MC_TRIPLE" \
  -o "$TMP/body.llc.o" "$ASM_IN"
ELF="$TMP/body.llc.o"

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
  "$LOWER" meta "$LL" "$TMP/body.s" "$TMP/body.dis" "$TMP/meta.json" "$GO_SYM" "$GOABI_JSON" "$ABI0" "$TMP/body.mir"
else
  "$LOWER" has-define "$LL" "$FN_NAME"
  CALLS_JSON='[]'
  if nm "$ELF" 2>/dev/null | grep -q 'U p28_external_hook' \
    || "$OBJDUMP" -d "$ELF" 2>/dev/null | grep -q 'p28_external_hook'; then
    CALLS_JSON='[{"callee":"p28_external_hook","stackmap_index":0}]'
  fi
  FRAME="$("$LOWER" frame "$TMP/body.s" "$FN_NAME")"
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

"$LOWER" check-meta "$TMP/meta.json"

# A release ships bin/elfpack. A checkout builds it from backend/goobj.
if [[ -x "$ROOT/bin/elfpack" ]]; then
  ELFPACK="$ROOT/bin/elfpack"
else
  ( cd "$P5" && go build -o "$TMP/elfpack" ./goobj/elfpack/ )
  ELFPACK="$TMP/elfpack"
fi
# HeaderString follows GOARCH, not the host. An explicit amd64 must override
# a user's GOARCH too. The elfpack binary itself stays a host (amd64) tool.
GOARCH="$GOC_ARCH" "$ELFPACK" -elf "$ELF" -meta "$TMP/meta.json" \
  -maps "$TMP/maps" -out-o "$OUT_O" -p "$PKG"

if [[ $MAGIC_OK -eq 1 ]]; then
  "$LOWER" magic-obj "$OUT_O"
fi

cp "$TMP/meta.json" "${OUT_O%.o}.meta.json"
cp "$LL" "${OUT_O%.o}.ll"
if [[ $ALL -eq 1 ]]; then
  NTEXT="$(go tool nm "$OUT_O" | grep -c ' T ' || true)"
  echo "P29-realbody-all: wrote $OUT_O (encoding=clang-real-isel, goabi=$GOABI, TEXT syms=$NTEXT)"
else
  echo "P28-realbody: wrote $OUT_O (encoding=clang-real-isel)"
fi
