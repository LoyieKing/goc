#!/usr/bin/env bash
# Build the Go CLI against the goc-built QuickJS core and the tiny C host bridge.
set -euo pipefail
ROOT="${GOC_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
export GOC_ROOT="$ROOT"
OUT="$ROOT/build/qjs"
# O2 = O3 within noise for QuickJS (callgrind instr -0.0003%, V8/fixed within
# run-to-run spread; perf-opt item 4), builds faster and matches native -O2.
export GOC_OPT_LEVEL="${GOC_OPT_LEVEL:-2}"

# This checks the engine object and its moving-stack smoke before installing a
# command that will run arbitrary input. The upstream QuickJS tree is untouched.
"$ROOT/scripts/qjs-build.sh"
export GOC_CLANG="${GOC_CLANG:-$ROOT/third_party/llvm-19.1.7-clang-build/bin/clang}"
export GOC_DEFAULT_PTR_COLOR=cptr GOC_NO_NOSPLIT=1 GOC_MORESTACK=1
export GOC_SPTR_MAPS=1 GOC_CRESERVE=8192 GOC_INLINE_DYNALLOC=1

"$ROOT/cmd/goc" build "$ROOT/tests/qjs/_qjs_cli_host.c" \
  -o "$OUT/cli_host.o" --all --goabi -DJS_NAN_BOXING=0 -D_GNU_SOURCE -DNDEBUG
"$ROOT/cmd/goc" build "$ROOT/tests/qjs/_qjs_cli_std_os.c" \
  -o "$OUT/cli_std_os.o" --all --goabi -DJS_NAN_BOXING=0 -D_GNU_SOURCE -DNDEBUG
"$ROOT/cmd/goc" build "$ROOT/tests/qjs/_qjs_cli_worker.c" \
  -o "$OUT/cli_worker.o" --all --goabi -DJS_NAN_BOXING=0 -D_GNU_SOURCE -DNDEBUG

BINOBJ="$OUT/quickjs.o $OUT/libregexp.o $OUT/libunicode.o $OUT/dtoa.o $OUT/shim.o $OUT/uptr.o $OUT/cli_host.o $OUT/cli_std_os.o $OUT/cli_worker.o"
( cd "$ROOT/tests/qjscli" && \
  CGO_ENABLED=1 GOFLAGS= GOC_BINOBJ="$BINOBJ" \
  go build -a -ldflags="-extldflags=-lm" \
    -toolexec "$ROOT/backend/tools/toolexec_pack_goobj.sh" \
    -o "$OUT/qjscli" . )
echo "CLI ready: $OUT/qjscli"
"$OUT/qjscli" -e '
  const close = (a, b) => Math.abs(a - b) < 1e-12;
  if (!close(Math.log(Math.E), 1) ||
      !close(Math.atan2(1, 1), Math.PI / 4) ||
      !close(Math.sin(Math.PI / 2), 1) ||
      !Number.isNaN(Math.log(-1)))
    throw new Error("QuickJS math bridge returned an invalid result");
  // glibc sin, not Go math: the printed value must match native libm.
  if (Math.sin(1).toString() !== "0.8414709848078965")
    throw new Error("Math.sin does not match libm");
  const epoch = new Date(0);
  if (Date.parse(epoch.toString()) !== 0 || Date.parse(epoch.toISOString()) !== 0)
    throw new Error("localtime bridge corrupted Date");
  if (epoch.toGMTString() !== "Thu, 01 Jan 1970 00:00:00 GMT")
    throw new Error("Date.toGMTString mismatch");
' >/dev/null
echo "PASS qjs-cli-math-bridge"
