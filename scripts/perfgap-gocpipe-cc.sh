#!/usr/bin/env bash
# Compiler wrapper for docs/perf-gap.md: a *native* build through goc's
# optimization pipeline and codegen flags, without the goc runtime (no uptr
# coloring, no morestack prologue, no Go ABI thunks, no libc shim).
#
#   GOCPIPE_MODE=pipe     clang -O3 frontend (goc's flags, -disable-llvm-passes)
#                         -> opt default<O3> minus argpromotion,globalopt
#                         -> llc with goc's flags (as backend/realbody)
#   GOCPIPE_MODE=pipe-sm  as pipe, plus goc-stackmap + goc-reanchor and goc-llc
#                         (GocFrameAddrFix): the stack-map and frame-address
#                         guard codegen, on a stack that never moves.
# Use as CC for Bellard's Makefile; non-compile invocations go to clang.
set -euo pipefail
ROOT="${GOC_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
CLANG="${GOCPIPE_CLANG:-clang-19}"
OPT="${OPT:-opt-19}"
MODE="${GOCPIPE_MODE:-pipe}"
compile=0; out=""; src=""; args=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    -c) compile=1 ;;
    -o) out="$2"; shift ;;
    -O*) ;;                                   # goc always uses O3
    -g|-g[0-9]) ;;                            # goc builds carry no debug info
    *.c) src="$1" ;;
    *) args+=("$1") ;;
  esac
  shift
done
if [[ $compile -eq 0 || -z "$src" ]]; then
  exec "$CLANG" "${args[@]}" ${src:+"$src"} ${out:+-o "$out"}
fi
t="$(mktemp -d)"; trap 'rm -rf "$t"' EXIT
"$CLANG" "${args[@]}" -O3 -fwrapv -fno-strict-aliasing -fno-omit-frame-pointer \
  -mno-omit-leaf-frame-pointer -fno-optimize-sibling-calls -mno-red-zone \
  -fno-stack-protector -fno-asynchronous-unwind-tables -Xclang -disable-llvm-passes \
  -emit-llvm -c -o "$t/a.bc" "$src"
# goc also sets override-stack-alignment=8 + no-realign-stack (the Go ABI only
# keeps 8-byte alignment). Not here: this binary calls glibc directly, and
# glibc's SSE spills (movaps) need the SysV 16-byte alignment at every call.
# Intermediates stay bitcode: with a textual .ll between opt and codegen this
# Bellard build crashed deterministically on the V8 file (the .ll printed from
# a working .bc also crashed; see docs/perf-gap.md, caveats).
PIPELINE="$("$OPT" "-passes=default<O3>" -print-pipeline-passes -disable-output /dev/null)"
PIPELINE="${PIPELINE//,argpromotion/}"
PIPELINE="${PIPELINE//,globalopt/}"
# GOCPIPE_PIPELINE=default: plain -passes=default<O3> (bisecting aid)
[[ "${GOCPIPE_PIPELINE:-}" == default ]] && PIPELINE="default<O3>"
# GOCPIPE_OPT_EXTRA: e.g. -inline-threshold=250 (the textual pipeline drops
# default<O3>'s inliner threshold; see docs/perf-gap.md)
# shellcheck disable=SC2086
"$OPT" "-passes=$PIPELINE" ${GOCPIPE_OPT_EXTRA:-} "$t/a.bc" -o "$t/b.bc"
LLC=llc-19
if [[ "$MODE" == pipe-sm ]]; then
  SM="$ROOT/backend/build/pass-out/GocStackMap.so"
  "$OPT" -load-pass-plugin="$SM" -passes=goc-stackmap "$t/b.bc" -o "$t/c.bc"
  "$OPT" -load-pass-plugin="$SM" -passes=goc-reanchor "$t/c.bc" -o "$t/b.bc"
  LLC="$ROOT/backend/build/pass-out/goc-llc"
fi
"$LLC" -O3 -relocation-model=pic -march=x86-64 -frame-pointer=all -enable-shrink-wrap=false \
  -disable-tail-calls -no-stack-slot-sharing -no-x86-call-frame-opt -enable-tail-merge=false \
  ${GOCPIPE_LLC_EXTRA:-} -filetype=asm -o "$t/a.s" "$t/b.bc"
# ALIGNFIX as in backend/realbody (a no-op for correctness at 16-byte alignment,
# kept so the instruction selection matches goc's).
sed -i -E 's/\bmovaps\b/movups/g; s/\bmovapd\b/movupd/g; s/\bmovdqa\b/movdqu/g' "$t/a.s"
"$CLANG" -c -o "$out" "$t/a.s"
