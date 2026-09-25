#!/usr/bin/env bash
# Build the Go CLI against the goc-built QuickJS core and the tiny C host bridge.
set -euo pipefail
ROOT="${GOC_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
export GOC_ROOT="$ROOT"
OUT="$ROOT/build/qjs"
export GOC_OPT_LEVEL="${GOC_OPT_LEVEL:-3}"

# This checks the engine object and its moving-stack smoke before installing a
# command that will run arbitrary input. The upstream QuickJS tree is untouched.
"$ROOT/scripts/qjs-build.sh"
export GOC_CLANG="${GOC_CLANG:-$ROOT/third_party/llvm-19.1.7-clang-build/bin/clang}"
export GOC_DEFAULT_PTR_COLOR=cptr GOC_NO_NOSPLIT=1 GOC_MORESTACK=1
export GOC_SPTR_MAPS=1 GOC_CRESERVE=8192 GOC_INLINE_DYNALLOC=1

"$ROOT/cmd/goc" build "$ROOT/tests/qjs/_qjs_cli_host.c" \
  -o "$OUT/cli_host.o" --all --goabi -DJS_NAN_BOXING=0 -D_GNU_SOURCE
"$ROOT/cmd/goc" build "$ROOT/tests/qjs/_qjs_cli_std_os.c" \
  -o "$OUT/cli_std_os.o" --all --goabi -DJS_NAN_BOXING=0 -D_GNU_SOURCE
"$ROOT/cmd/goc" build "$ROOT/tests/qjs/_qjs_cli_worker.c" \
  -o "$OUT/cli_worker.o" --all --goabi -DJS_NAN_BOXING=0 -D_GNU_SOURCE

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
' >/dev/null
echo "PASS qjs-cli-math-bridge"
