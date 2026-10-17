#!/usr/bin/env bash
# QuickJS on goroutine stacks: compile the engine and the C host with goc,
# link the Go programs, and run the moving-stack smoke.
#
#   ports/quickjs/build.sh [--flavor ng|bellard] [--engine-only]
#                          [--tags TAGS] [-o BIN]
#
#   --flavor ng       third_party/quickjs-ng      -> build/qjs (default)
#   --flavor bellard  third_party/quickjs-bellard -> build/qjs-bellard
#                     (Fabrice Bellard's QuickJS 2026-06-04; the host maps
#                     the ng API onto it with host/bellard_compat.h)
#   --engine-only     stop after the embed smoke (build/<flavor>/qjs_test)
#   --tags, -o        build tags and output of the CLI (default
#                     build/<flavor>/qjscli); bench/mem.sh builds the
#                     qjsmem variant with --tags qjsmem -o build/qjs/qjsmem
#
# The smoke reads QJS_EVAL, QJS_PROMISE and QJS_GROWTH_SWEEP (default 300).
# Prereqs: the QuickJS tree (third_party/README.md), the patched clang next
# to goc, Go 1.24+. The gstack patch in patches/ is applied on first use.
set -euo pipefail
# shellcheck source=../../test/lib.sh
source "$(cd "$(dirname "$0")/../.." && pwd)/test/lib.sh"
P="$ROOT/ports/quickjs"
FLAVOR=ng ENGINE_ONLY=0 TAGS="" BIN=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --flavor) FLAVOR="${2:?--flavor needs ng or bellard}"; shift 2 ;;
    --flavor=*) FLAVOR="${1#*=}"; shift ;;
    --engine-only) ENGINE_ONLY=1; shift ;;
    --tags) TAGS="${2?}"; shift 2 ;;
    -o) BIN="${2:?-o needs a path}"; shift 2 ;;
    -h|--help) sed -n '2,19s/^# \{0,1\}//p' "$0"; exit 0 ;;
    *) echo "build.sh: unknown argument $1 (see --help)" >&2; exit 2 ;;
  esac
done

case "$FLAVOR" in
  ng)
    QJS="$ROOT/third_party/quickjs-ng"
    OUT="$ROOT/build/qjs"
    QJS_TUS=(quickjs libregexp libunicode dtoa)
    FLAVOR_DEFS=(-DJS_NAN_BOXING=0)
    HOST_DEFS=(-DJS_NAN_BOXING=0)
    ;;
  bellard)
    QJS="$ROOT/third_party/quickjs-bellard"
    OUT="$ROOT/build/qjs-bellard"
    # Bellard keeps cutils (dbuf, utf8, rqsort) in its own TU.
    QJS_TUS=(quickjs libregexp libunicode dtoa cutils)
    # No -DJS_NAN_BOXING=0 here: Bellard tests `#ifdef JS_NAN_BOXING`, so
    # defining it to 0 would select NaN boxing. On x86-64 its default is
    # already the 16-byte struct JSValue the Go side expects.
    FLAVOR_DEFS=(-DGOC_QJS_BELLARD=1 "-DCONFIG_VERSION=\"$(cat "$QJS/VERSION" 2>/dev/null)\"")
    HOST_DEFS=(-DGOC_QJS_BELLARD=1)
    ;;
  *) echo "FAIL: --flavor must be ng or bellard" >&2; exit 2 ;;
esac
GSTACK_PATCH="$P/patches/$FLAVOR-gstack.patch"
BIN="${BIN:-$OUT/qjscli}"
mkdir -p "$OUT"

[[ -d "$QJS" ]] || { echo "FAIL: fetch QuickJS ($FLAVOR) to ${QJS#$ROOT/}, see third_party/README.md" >&2; exit 1; }
# `patch -R --dry-run --batch` succeeds on an *unpatched* tree too ("Unreversed
# patch detected! Ignoring -R." then a forward dry run). -f makes -R literal:
# it succeeds only if this exact patch is already in. An older revision of
# the patch makes both checks fail.
if patch -R --dry-run -f --silent -d "$QJS" -p1 -i "$GSTACK_PATCH" >/dev/null 2>&1; then
  : # already present in this ignored checkout
elif patch --dry-run -N --batch --silent -d "$QJS" -p1 -i "$GSTACK_PATCH" >/dev/null 2>&1; then
  patch -N --batch --silent -d "$QJS" -p1 -i "$GSTACK_PATCH"
else
  echo "FAIL: QuickJS source differs from ${GSTACK_PATCH#$ROOT/} (an older" \
       "revision applied? restore the pristine files and rerun)" >&2
  exit 1
fi

# -O3, bulk coloring (uncolored pointers are cptr), the alloca pool bumped
# in line (no per-call memset): what goc go does by default.
# -DNDEBUG: same as the native reference (CMake Release: clang-19 -O2 -DNDEBUG).
GOCB=("$GOC" build -O3 --default-ptr-color cptr --fast-stack-alloca)
QJS_DEFS=("${FLAVOR_DEFS[@]}" -D_GNU_SOURCE -DGOC_QJS_GSTACK=1 -DNDEBUG)
# Bellard has no JS_SetPromiseHook: its smoke skips the promise-hook probe.
[[ "$FLAVOR" == bellard ]] && QJS_PROMISE="${QJS_PROMISE:-0}"
export QJS_EVAL="${QJS_EVAL:-1}" QJS_PROMISE="${QJS_PROMISE:-1}"
nm_text() { go tool nm "$1" | grep -c ' T '; }

echo "=== [1/5] freestanding libc shim -> goobj ==="
# Same mode as the QJS TUs, so cross-TU C references agree on names
# (main.goc_abort.impl etc.). The ABIInternal thunks are dead weight here.
"${GOCB[@]}" "$P/host/libc_shim.c" -o "$OUT/shim.goobj" -- "${QJS_DEFS[@]}"
echo "  shim: $(nm_text "$OUT/shim.goobj") TEXT"

# Redirect QJS's libc references to the shim's goc_-prefixed symbols: -D for
# source-level names, --ir-renames for compiler-emitted libcalls.
SHIM_NAMES=$(go tool nm "$OUT/shim.goobj" | awk '$2=="T"{print $3}' \
  | sed 's/^.*\.//; s/\.impl$//' | grep '^goc_' | sed 's/^goc_//' | sort -u \
  | grep -v -x -e memcpy -e memmove -e memset | tr '\n' ' ')
# LLVM lowers some intrinsics (e.g. floor/trunc) to bare libcalls *after* the
# Go ABI IR rename. Bind only names the shim actually defined as ABI0 .impl
# bodies; varargs and other skipped functions retain their plain names.
LIBCALL_IMPL=$(go tool nm "$OUT/shim.goobj" | awk '
  $2 == "T" && $3 ~ /^main\.[A-Za-z_][A-Za-z0-9_]*\.impl$/ {
    sub(/^main\./, "", $3); sub(/\.impl$/, "", $3); print $3
  }' | sort -u | tr '\n' ' ')
SHIM_DEFS=""
IR_RENAMES=""
for n in $SHIM_NAMES; do
  SHIM_DEFS="$SHIM_DEFS -D$n=goc_$n"
  IR_RENAMES="$IR_RENAMES $n:goc_$n"
done
echo "  shim redirects: $(echo $SHIM_NAMES | wc -w) symbols"
SHIMB=("${GOCB[@]}" --ir-renames "$IR_RENAMES" --libcall-impl "$LIBCALL_IMPL")
"${SHIMB[@]}" "$P/host/promise_probe.c" -o "$OUT/promise_probe.goobj" -- "${QJS_DEFS[@]}"
EXTRA_OBJS=""
if [[ "$FLAVOR" == bellard ]]; then
  # Exported JS_FreeValue & co. (static inline in Bellard's quickjs.h).
  # shellcheck disable=SC2086
  "${SHIMB[@]}" "$P/host/bellard_api.c" -o "$OUT/bellard_api.goobj" -- "${QJS_DEFS[@]}" $SHIM_DEFS
  EXTRA_OBJS=" $OUT/bellard_api.goobj"
fi

echo "=== [2/5] QuickJS ($FLAVOR) TUs -> goobj (colored, Go-ABI thunks) ==="
for src in "${QJS_TUS[@]}"; do
  TU_DEFS=()
  # Bellard: the public JS_FreeRuntime lives in bellard_api.c (runs the
  # emulated ng runtime finalizers first); quickjs.c's own one is renamed.
  [[ "$FLAVOR" == bellard && "$src" == quickjs ]] && \
    TU_DEFS=(-DJS_FreeRuntime=goc_bellard_JS_FreeRuntime)
  # shellcheck disable=SC2086
  "${SHIMB[@]}" "$QJS/$src.c" -o "$OUT/$src.goobj" -- "${QJS_DEFS[@]}" "${TU_DEFS[@]}" $SHIM_DEFS
  echo "  $src: $(nm_text "$OUT/$src.goobj") TEXT"
done
ENGINE=""
for src in "${QJS_TUS[@]}"; do ENGINE="$ENGINE $OUT/$src.goobj"; done
ENGINE="${ENGINE# } $OUT/shim.goobj"

echo "=== [3/5] link the embed smoke (toolexec packs goobjs; -lm via extld) ==="
# glibc trampolines for the shim's transcendentals. The .S stays outside
# the Go package: cgo would compile a .S next to the .go files, and a
# .syso of the same code would be a second definition. *.syso is gitignored;
# both Go packages pick the file up from their directory.
gcc -c -o "$P/embed/goc_libm_glibc.syso" "$P/host/asm/goc_libm_glibc.S"
cp -f "$P/embed/goc_libm_glibc.syso" "$P/cli/goc_libm_glibc.syso"
( cd "$P/embed" && CGO_ENABLED=1 GOFLAGS= \
  go build -a -ldflags="-extldflags=-lm" \
    -toolexec "$GOC toolexec $ENGINE $OUT/promise_probe.goobj$EXTRA_OBJS --" \
    -o "$OUT/qjs_test" . )

echo "=== [4/5] run on the goroutine stack ==="
timeout 20 "$OUT/qjs_test"
# Golden stack-growth test: fresh goroutines with varied stack pre-fill so
# morestack copies land at many different points inside parser/interpreter
# C frames (frame addresses live across calls, callee-saved regs, spills).
echo "--- growth sweep (QJS_GROWTH_SWEEP=${QJS_GROWTH_SWEEP:-300}) ---"
QJS_GROWTH_SWEEP="${QJS_GROWTH_SWEEP:-300}" timeout 120 "$OUT/qjs_test"
echo "PASS qjs embed ($FLAVOR)"
[[ "$ENGINE_ONLY" == 1 ]] && exit 0

echo "=== [5/5] the qjscli command ==="
for h in cli_host cli_std_os cli_worker; do
  "${GOCB[@]}" "$P/host/$h.c" -o "$OUT/$h.goobj" -- "${HOST_DEFS[@]}" -D_GNU_SOURCE -DNDEBUG
done
LDX=""  # cli/mem_instances.go labels its JSON by flavor
[[ "$FLAVOR" == bellard ]] && LDX=" -X main.qjsFlavor=bellard"
( cd "$P/cli" && CGO_ENABLED=1 GOFLAGS= \
  go build -a -tags "$TAGS" -ldflags="-extldflags=-lm$LDX" \
    -toolexec "$GOC toolexec $ENGINE $OUT/cli_host.goobj $OUT/cli_std_os.goobj $OUT/cli_worker.goobj$EXTRA_OBJS --" \
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
