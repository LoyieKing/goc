#!/usr/bin/env bash
# Build the Go CLI against the goc-built QuickJS core and the tiny C host bridge.
set -euo pipefail
ROOT="${GOC_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
export GOC_ROOT="$ROOT"
# QJS_FLAVOR=ng (default) or bellard, as in scripts/qjs-build.sh. The Bellard
# CLI is build/qjs-bellard/qjscli; the host files map the ng API they use onto
# Bellard's with tests/qjs/_qjs_bellard_compat.h.
QJS_FLAVOR="${QJS_FLAVOR:-ng}"
export QJS_FLAVOR
case "$QJS_FLAVOR" in
  ng)      OUT="${QJS_OUT_DIR:-$ROOT/build/qjs}"; HOST_DEFS=(-DJS_NAN_BOXING=0)
           ENGINE_TUS="quickjs libregexp libunicode dtoa"; EXTRA_OBJS="" ;;
  bellard) OUT="${QJS_OUT_DIR:-$ROOT/build/qjs-bellard}"; HOST_DEFS=(-DGOC_QJS_BELLARD=1)
           ENGINE_TUS="quickjs libregexp libunicode dtoa cutils"
           EXTRA_OBJS=" $OUT/bellard_api.o" ;;
  *) echo "FAIL: QJS_FLAVOR must be ng or bellard" >&2; exit 1 ;;
esac
export GOC_OPT_LEVEL="${GOC_OPT_LEVEL:-3}"
# QJSCLI_TAGS / QJSCLI_OUT build a variant of the same CLI package, e.g.
# scripts/bench-mem.sh uses QJSCLI_TAGS=qjsmem QJSCLI_OUT=build/qjs/qjsmem.
TAGS="${QJSCLI_TAGS:-}"
BIN="${QJSCLI_OUT:-$OUT/qjscli}"

# This checks the engine object and its moving-stack smoke before installing a
# command that will run arbitrary input. The upstream QuickJS tree is untouched.
"$ROOT/scripts/qjs-build.sh"
export GOC_CLANG="${GOC_CLANG:-$ROOT/third_party/llvm-19.1.7-clang-build/bin/clang}"
export GOC_DEFAULT_PTR_COLOR=cptr GOC_NO_NOSPLIT=1 GOC_MORESTACK=1
export GOC_SPTR_MAPS=1 GOC_CRESERVE=8192 GOC_INLINE_DYNALLOC=1

"$ROOT/cmd/goc" build "$ROOT/tests/qjs/_qjs_cli_host.c" \
  -o "$OUT/cli_host.o" --all --goabi "${HOST_DEFS[@]}" -D_GNU_SOURCE -DNDEBUG
"$ROOT/cmd/goc" build "$ROOT/tests/qjs/_qjs_cli_std_os.c" \
  -o "$OUT/cli_std_os.o" --all --goabi "${HOST_DEFS[@]}" -D_GNU_SOURCE -DNDEBUG
"$ROOT/cmd/goc" build "$ROOT/tests/qjs/_qjs_cli_worker.c" \
  -o "$OUT/cli_worker.o" --all --goabi "${HOST_DEFS[@]}" -D_GNU_SOURCE -DNDEBUG

LDX=""  # tests/qjscli/mem_instances.go labels its JSON by flavor
[[ "$QJS_FLAVOR" == bellard ]] && LDX=" -X main.qjsFlavor=bellard"
BINOBJ=""
for src in $ENGINE_TUS; do BINOBJ="$BINOBJ $OUT/$src.o"; done
BINOBJ="${BINOBJ# } $OUT/shim.o $OUT/uptr.o $OUT/cli_host.o $OUT/cli_std_os.o $OUT/cli_worker.o$EXTRA_OBJS"
( cd "$ROOT/tests/qjscli" && \
  CGO_ENABLED=1 GOFLAGS= GOC_BINOBJ="$BINOBJ" \
  go build -a -tags "$TAGS" -ldflags="-extldflags=-lm$LDX" \
    -toolexec "$ROOT/backend/tools/toolexec_pack_goobj.sh" \
    -o "$BIN" . )
echo "CLI ready: $BIN"
"$BIN" -e '
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
