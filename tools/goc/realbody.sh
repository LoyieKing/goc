#!/usr/bin/env bash
# Real Clang IR body → goc-llc → .gocobj → gocobj2goobj → goobj.
# Every defined function is emitted. Each eligible function is renamed F.impl
# and gains a Go ABIInternal entry F (Go→C thunk, C→C on the platform ABI,
# C→Go .goabi). amd64 uses SysV; arm64 uses AAPCS64.
# Usage: realbody.sh <input.ll|.c> <out.goobj> [pkg_prefix] [--abi0]
#   pkg_prefix defaults to main. An import path may contain dots.
#   --abi0: mark the object ABI0 (C-side platform entries, e.g. a libc shim).
set -euo pipefail
SELF="$(cd "$(dirname "$0")" && pwd)"
ROOT="${GOC_ROOT:-$(cd "$SELF/../.." && pwd)}"
export GOC_ROOT="$ROOT"
GOMOD_ROOT="$ROOT"
# shellcheck source=scripts/goc-product-lib.sh
source "$ROOT/scripts/goc-product-lib.sh"

ABI0=0
POS=()
for a in "$@"; do
  case "$a" in
    --all|--goabi|--all=*|--goabi=*)
      echo "realbody: every defined function is emitted, with a Go entry. There is no --all or --goabi." >&2
      exit 2
      ;;
    --abi0) ABI0=1 ;;
    --*)
      echo "realbody: unknown flag $a" >&2
      exit 2
      ;;
    *) POS+=("$a") ;;
  esac
done
set -- ${POS[@]+"${POS[@]}"}

IN="${1:?input .ll or .c}"
OUT_O="${2:?output .goobj}"
if [[ $# -gt 3 ]]; then
  echo "realbody: usage: realbody.sh <input.ll|.c> <out.goobj> [pkg_prefix] [--abi0]" >&2
  exit 2
fi
PKG="${3:-main}"

CLANG="$(goc_resolve_clang)"
goc_prepend_lib "$(goc_clang_libdir "$CLANG")"
# Default stays linux/amd64. arm64 is a separate ISel, not a -march switch.
# Both go through goc-llc: it emits the stack check (AArch64 hook) and the
# .goc.* tables. g is x28, reserved by llc; there is no TLS load. Frame-
# address repair (GocFrameAddrFix) and stack-map roots are x86-only, as on
# main, so arm64 functions carry no locals pointer maps.
GOC_ARCH="${GOC_ARCH:-amd64}"
case "$GOC_ARCH" in
  amd64|arm64) ;;
  *) echo "realbody: FATAL GOC_ARCH=$GOC_ARCH (want amd64 or arm64)" >&2; exit 1 ;;
esac
export GOC_ARCH
if [[ "$GOC_ARCH" == arm64 && "${GOC_CSR_ADJUST:-0}" == 1 ]]; then
  echo "realbody: FATAL GOC_CSR_ADJUST is x86-only; arm64 has no frame-address repair" >&2
  exit 1
fi
# goc-reanchor rematerializes frame addresses as plain GEPs, which is only
# safe with goc-llc's post-RA GocFrameAddrFix pass (llvm/lib/CodeGen).
# Stock llc CSEs the GEP into a callee-saved register and keeps it across the
# call; only GocFrameAddrFix re-derives it, so a stock LLC is refused.
if [[ -n "${GOC_FRAMEADDR_MODE:-}" && "$GOC_FRAMEADDR_MODE" != gep ]]; then
  echo "realbody: FATAL GOC_FRAMEADDR_MODE=$GOC_FRAMEADDR_MODE was removed; frame addresses are always GEPs repaired by goc-llc" >&2
  exit 1
fi
if [[ -x "$ROOT/passes/goc-llc" ]]; then
  GOC_LLC_BIN="$ROOT/passes/goc-llc"
else
  GOC_LLC_BIN="$ROOT/build/llvm/goc-llc"
fi
if [[ -z "${LLC:-}" && ! -x "$GOC_LLC_BIN" ]]; then
  "$ROOT/scripts/build-passes.sh" >&2 || true
fi
if [[ -z "${LLC:-}" && ! -x "$GOC_LLC_BIN" ]]; then
  echo "realbody: FATAL missing $GOC_LLC_BIN (frame-address repair); run scripts/build-passes.sh" >&2
  exit 1
fi
LLC="${LLC:-$GOC_LLC_BIN}"
if ! grep -a -q 'goc-frameaddr-fix' "$LLC" 2>/dev/null; then
  echo "realbody: FATAL $LLC has no GocFrameAddrFix; frame-address GEPs would stay stale across calls. Build build/llvm/goc-llc." >&2
  exit 1
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

# GOC_KEEP_TMP: a directory to work in and keep; 1 (as cmd/goc takes it)
# keeps a fresh temp directory instead of creating ./1.
if [[ -n "${GOC_KEEP_TMP:-}" && "$GOC_KEEP_TMP" != 1 ]]; then
  TMP="$GOC_KEEP_TMP"
else
  TMP="$(mktemp -d)"
  [[ -n "${GOC_KEEP_TMP:-}" ]] && echo "realbody: kept $TMP" >&2
fi
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
    TARGET_ARGS=(-target aarch64-unknown-linux-gnu -ffixed-x28 -fno-pic)
    # -fno-pic: declarations are dso_local, so llc reaches them with ADRP
    # pairs, not GOT loads. Debian's cross headers when present.
    [[ -d /usr/aarch64-linux-gnu/include ]] && TARGET_ARGS+=(-isystem /usr/aarch64-linux-gnu/include)
  fi
  # -gline-tables-only: the FILE/LINE/INL_INDEX rows and the inline tree.
  "$CLANG" "${NATIVE_DEFS[@]}" "${TARGET_ARGS[@]}" -mno-red-zone -fno-stack-protector \
    -fno-asynchronous-unwind-tables -gline-tables-only -I "$INC" \
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
  elif [[ -f "$ROOT/passes/libGocPasses.so" ]]; then
    SMPASS="$ROOT/passes/libGocPasses.so"
  else
    SMPASS="$ROOT/build/llvm/libGocPasses.so"
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
  "$OPT" -load-pass-plugin="$SMPASS" "-passes=$PIPELINE,function(goc-name-blocks)" "${_goc_opt_extra[@]}" -S "$LL" -o "$TMP/body.opt.ll"
  cp "$TMP/body.opt.ll" "$LL"
  echo "realbody: O$OPT_LEVEL inlined before stack maps" >&2
fi
# goc-llc writes the .goc.* tables of the C functions (body.goc.s) and
# goc-lower goabi those of the Go ABI thunks (thunks.goc.s); both are
# appended to the assembly (docs/gocobj-format.md).
GOC_TABLES="$TMP/body.goc.s"
"$LOWER" goabi "$LL" "$TMP/body.impl.ll" "$TMP/thunks.s" "$GOABI_JSON" "$TMP/thunks.goc.s"
cp "$TMP/body.impl.ll" "$LL"
# Go symbol ABI of the C functions: ABI0 when Go ABI thunks wrap them (the
# thunk owns the ABIInternal name) or for a C-side object (--abi0).
SYM_ABI=1
if [[ "$ABI0" == 1 ]] || grep -q '"name"' "$GOABI_JSON"; then
  SYM_ABI=0
fi

# Record the stack-map roots on the IR goc-llc compiles: the tables take
# their locations from the STACKMAP instructions of this very compile.
if [[ "${GOC_STACKMAP_PREPARED:-0}" != "1" ]]; then
  if [[ -n "${GOC_STACKMAP:-}" && -f "$GOC_STACKMAP" ]]; then
    SMPASS="$GOC_STACKMAP"
  elif [[ -f "$ROOT/passes/libGocPasses.so" ]]; then
    SMPASS="$ROOT/passes/libGocPasses.so"
  else
    SMPASS="$ROOT/build/llvm/libGocPasses.so"
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
# leave each stackmap record before a JMP; the tables attach a record to the
# next CALL in layout order, so the merged CALL would get another path's roots.
# Tail duplication is on: QuickJS dispatch blocks are shared tails, and gcc
# duplicates them.
if [[ "$GOC_ARCH" == arm64 ]]; then
  # No -march=x86-64, no -no-x86-call-frame-opt, no -reserve-goc-r14.
  # +reserve-x28 keeps g live across the body. AArch64 has no red zone.
  # Jump tables stay 4-byte: goc-llc inserts code after the compression
  # pass sized the entries (LR copy, frame-address repair, stack check).
  # Static model: extern data is reached with ADRP+ADD/LDR (PC-relative,
  # also fine in a PIE), not through a GOT pair that llc may split.
  LLC_ARGS=("-O$LLC_OPT_LEVEL" -relocation-model=static
            -mtriple=aarch64-unknown-linux-gnu -mattr=+reserve-x28
            -aarch64-enable-compress-jump-tables=false
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
# goc-llc emits the Go stack check and morestack block of each function and
# writes the .goc.* tables (docs/gocobj-format.md) as assembler text. They
# are assembled into the same ELF object, which makes it a .gocobj. One
# section per function: every reference between functions then carries an
# ELF relocation, so the converter never patches code.
LLC_ARGS+=(-function-sections -goc-sym-abi="$SYM_ABI")
if [[ "$GOC_ARCH" == arm64 ]]; then
  LLC_ARGS+=(-goc-profile=go1.24-arm64)
fi
if [[ "${GOC_FIXED_G:-0}" == "1" ]]; then
  LLC_ARGS+=(-goc-fixed-g)
fi
"$LLC" "${LLC_ARGS[@]}" -goc-tables "$GOC_TABLES" -filetype=asm \
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
cat "$TMP/body.s" "$TMP/thunks.s" "$GOC_TABLES" "$TMP/thunks.goc.s" > "$TMP/body.thunks.s"
"$MC" -filetype=obj -triple="$MC_TRIPLE" \
  -o "$TMP/body.llc.o" "$TMP/body.thunks.s"
ELF="$TMP/body.llc.o"

# A release ships bin/gocobj2goobj. A checkout builds it.
PACK_BIN="$ROOT/bin/gocobj2goobj"
if [[ ! -x "$PACK_BIN" ]]; then
  PACK_BIN="$TMP/pack"
  ( cd "$GOMOD_ROOT" && go build -o "$PACK_BIN" ./tools/gocobj2goobj/ )
fi
side="$OUT_O"
case "$side" in
  *.goobj) side="${side%.goobj}" ;;
  *.o) side="${side%.o}" ;;
esac
# HeaderString follows GOARCH, not the host. An explicit amd64 must override
# a user's GOARCH too. The converter itself stays a host tool.
GOARCH="$GOC_ARCH" "$PACK_BIN" -in "$ELF" -o "$OUT_O" -p "$PKG" -symabis "${side}.symabis"

if [[ $MAGIC_OK -eq 1 ]]; then
  "$LOWER" magic-obj "$OUT_O"
fi

# foo.goobj sits beside foo.gocobj, foo.symabis and foo.ll.
cp "$ELF" "${side}.gocobj"
cp "$LL" "${side}.ll"
NTEXT="$(go tool nm "$OUT_O" | grep -c ' T ' || true)"
echo "realbody: wrote $OUT_O (encoding=clang-real-isel, TEXT syms=$NTEXT)"
