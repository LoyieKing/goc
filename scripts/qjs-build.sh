#!/usr/bin/env bash
# quickjs-ng via goc: color, compile to goobj, link, run the goroutine-stack smoke.
#
# Prereqs: third_party/quickjs-ng, patched clang (GOC_CLANG),
#          llc-19 / llvm-objdump-19 / ld.lld-19, Go 1.24+.
set -euo pipefail
ROOT="${GOC_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
export GOC_ROOT="$ROOT"
# QJS_FLAVOR selects the engine tree: ng (default, third_party/quickjs-ng ->
# build/qjs) or bellard (Fabrice Bellard's QuickJS 2026-06-04,
# third_party/quickjs-bellard -> build/qjs-bellard). Both use the same shim,
# uptr runtime, flags and goc pipeline; see docs/benchmark.md for the port.
QJS_FLAVOR="${QJS_FLAVOR:-ng}"
case "$QJS_FLAVOR" in
  ng)
    QJS="$ROOT/third_party/quickjs-ng"
    OUT="${QJS_OUT_DIR:-$ROOT/build/qjs}"
    GSTACK_PATCH="$ROOT/scripts/qjs-gstack.patch"
    QJS_TUS=(quickjs libregexp libunicode dtoa)
    FLAVOR_DEFS=(-DJS_NAN_BOXING=0)
    ;;
  bellard)
    QJS="$ROOT/third_party/quickjs-bellard"
    OUT="${QJS_OUT_DIR:-$ROOT/build/qjs-bellard}"
    GSTACK_PATCH="$ROOT/scripts/qjs-gstack-bellard.patch"
    # Bellard keeps cutils (dbuf, utf8, rqsort) in its own TU.
    QJS_TUS=(quickjs libregexp libunicode dtoa cutils)
    # No -DJS_NAN_BOXING=0 here: Bellard tests `#ifdef JS_NAN_BOXING`, so
    # defining it to 0 would select NaN boxing. On x86-64 its default is
    # already the 16-byte struct JSValue the Go side expects.
    FLAVOR_DEFS=(-DGOC_QJS_BELLARD=1 "-DCONFIG_VERSION=\"$(cat "$QJS/VERSION" 2>/dev/null)\"")
    ;;
  *) echo "FAIL: QJS_FLAVOR must be ng or bellard" >&2; exit 1 ;;
esac
export QJS_FLAVOR
mkdir -p "$OUT"

CLANG="${GOC_CLANG:-}"
if [[ -z "$CLANG" ]]; then
  for c in "$ROOT/third_party/llvm-19.1.7-clang-build/bin/clang" \
           "$ROOT/third_party/llvm-clang-build/bin/clang"; do
    [[ -x "$c" ]] && CLANG="$c" && break
  done
fi
if [[ -z "$CLANG" || ! -x "$CLANG" ]]; then
  echo "FAIL: set GOC_CLANG to the patched clang-19" >&2
  exit 1
fi
export GOC_CLANG="$CLANG"
export GOC_OPT_LEVEL="${GOC_OPT_LEVEL:-3}"
[[ -d "$QJS" ]] || { echo "FAIL: fetch QuickJS ($QJS_FLAVOR) to ${QJS#$ROOT/}, see third_party/README.md" >&2; exit 1; }
# `patch -R --dry-run --batch` succeeds on an *unpatched* tree too ("Unreversed
# patch detected! Ignoring -R." then a forward dry run), so the old check never
# applied the patch. -f makes -R literal: it succeeds only if this exact patch
# is already in. An older revision of the patch makes both checks fail.
if patch -R --dry-run -f --silent -d "$QJS" -p1 -i "$GSTACK_PATCH" >/dev/null 2>&1; then
  : # already present in this ignored checkout
elif patch --dry-run -N --batch --silent -d "$QJS" -p1 -i "$GSTACK_PATCH" >/dev/null 2>&1; then
  patch -N --batch --silent -d "$QJS" -p1 -i "$GSTACK_PATCH"
else
  echo "FAIL: QuickJS source differs from scripts/qjs-gstack.patch (an older" \
       "revision applied? restore the pristine files and rerun)" >&2
  exit 1
fi

# -DNDEBUG: same as the native reference (CMake Release: clang-19 -O2 -DNDEBUG).
QJS_DEFS=("${FLAVOR_DEFS[@]}" -D_GNU_SOURCE -DGOC_QJS_GSTACK=1 -DNDEBUG)
UPTR_DEFS=(-DGOC_UPTR_FREESTANDING -DGOC_UPTR_HAVE_TLS -DGOC_DYNALLOC_POOL)
export GOC_DEFAULT_PTR_COLOR=cptr   # bulk coloring: uncolored ptrs are cptr
export GOC_NO_NOSPLIT=1             # let the meta say "splittable", not "nosplit"
export GOC_MORESTACK="${GOC_MORESTACK:-1}"  # real split check + morestack stub in goc TEXT
export GOC_SPTR_MAPS="${GOC_SPTR_MAPS:-1}"  # real locals maps at C call sites
export GOC_INLINE_DYNALLOC=1        # bump the alloca pool in-line; no per-call memset
export GOC_CRESERVE="${GOC_CRESERVE:-8192}"  # fixed Go->C thunk frame, not a C-stack workaround
# Bellard has no JS_SetPromiseHook: its smoke skips the promise-hook probe.
[[ "$QJS_FLAVOR" == bellard ]] && QJS_PROMISE="${QJS_PROMISE:-0}"
export QJS_EVAL="${QJS_EVAL:-1}" QJS_PROMISE="${QJS_PROMISE:-1}"

echo "=== [1/4] freestanding libc shim + uptr runtime → goobj ==="
# Same mode as the QJS TUs, so cross-TU C references agree on names
# (main.goc_abort.impl etc.). The ABIInternal thunks are dead weight here.
env -u GOC_PKG "$ROOT/cmd/goc" build "$ROOT/tests/qjs/_qjs_libc_shim.c" \
  -o "$OUT/shim.o" --all --goabi "${QJS_DEFS[@]}"
echo "  shim: $(go tool nm "$OUT/shim.o" | grep -c ' T ') TEXT"
env -u GOC_PKG -u GOC_DEFAULT_PTR_COLOR -u GOC_IR_RENAMES \
  "$ROOT/cmd/goc" build "$ROOT/runtime/uptr/goc_uptr_runtime.c" \
  -o "$OUT/uptr.o" --all --goabi -I "$ROOT/runtime" "${UPTR_DEFS[@]}"
echo "  uptr runtime: $(go tool nm "$OUT/uptr.o" | grep -c ' T ') TEXT"

# Redirect QJS's libc references to the shim's goc_-prefixed symbols: -D for
# source-level names, GOC_IR_RENAMES for compiler-emitted libcalls.
SHIM_NAMES=$(go tool nm "$OUT/shim.o" | awk '$2=="T"{print $3}' \
  | sed 's/^.*\.//; s/\.impl$//' | grep '^goc_' | sed 's/^goc_//' | sort -u \
  | grep -v -x -e memcpy -e memmove -e memset | tr '\n' ' ')
# LLVM lowers some intrinsics (e.g. floor/trunc) to bare libcalls *after* the
# Go ABI IR rename. Bind only names the shim actually defined as ABI0 .impl
# bodies; varargs and other skipped functions retain their plain names.
GOC_LIBCALL_IMPL=$(go tool nm "$OUT/shim.o" | awk '
  $2 == "T" && $3 ~ /^main\.[A-Za-z_][A-Za-z0-9_]*\.impl$/ {
    sub(/^main\./, "", $3); sub(/\.impl$/, "", $3); print $3
  }' | sort -u | tr '\n' ' ')
export GOC_LIBCALL_IMPL
SHIM_DEFS=""
IR_RENAMES=""
for n in $SHIM_NAMES; do
  SHIM_DEFS="$SHIM_DEFS -D$n=goc_$n"
  IR_RENAMES="$IR_RENAMES $n:goc_$n"
done
export GOC_IR_RENAMES="$IR_RENAMES"
echo "  shim redirects: $(echo $SHIM_NAMES | wc -w) symbols"
env -u GOC_PKG "$ROOT/cmd/goc" build "$ROOT/tests/qjs/_qjs_promise_probe.c" \
  -o "$OUT/promise_probe.o" --all --goabi "${QJS_DEFS[@]}"
EXTRA_OBJS=""
if [[ "$QJS_FLAVOR" == bellard ]]; then
  # Exported JS_FreeValue & co. (static inline in Bellard's quickjs.h).
  env -u GOC_PKG "$ROOT/cmd/goc" build "$ROOT/tests/qjs/_qjs_bellard_api.c" \
    -o "$OUT/bellard_api.o" --all --goabi "${QJS_DEFS[@]}" $SHIM_DEFS
  EXTRA_OBJS=" $OUT/bellard_api.o"
fi

echo "=== [2/4] QuickJS ($QJS_FLAVOR) TUs → goobj (colored, Go-ABI thunks) ==="
for src in "${QJS_TUS[@]}"; do
  TU_DEFS=()
  # Bellard: the public JS_FreeRuntime lives in _qjs_bellard_api.c (runs the
  # emulated ng runtime finalizers first); quickjs.c's own one is renamed.
  [[ "$QJS_FLAVOR" == bellard && "$src" == quickjs ]] && \
    TU_DEFS=(-DJS_FreeRuntime=goc_bellard_JS_FreeRuntime)
  env -u GOC_PKG "$ROOT/cmd/goc" build "$QJS/$src.c" -o "$OUT/$src.o" \
    --all --goabi "${QJS_DEFS[@]}" "${TU_DEFS[@]}" $SHIM_DEFS
  echo "  $src: $(go tool nm "$OUT/$src.o" | grep -c ' T ') TEXT"
done

echo "=== [3/4] link Go caller (toolexec packs goobjs; -lm via extld) ==="
BINOBJ=""
for src in "${QJS_TUS[@]}"; do BINOBJ="$BINOBJ $OUT/$src.o"; done
BINOBJ="${BINOBJ# } $OUT/shim.o $OUT/uptr.o $OUT/promise_probe.o$EXTRA_OBJS"
( cd "$ROOT/tests/qjs" && \
  CGO_ENABLED=1 GOFLAGS= GOC_BINOBJ="$BINOBJ" \
  go build -a -ldflags="-extldflags=-lm" \
    -toolexec "$ROOT/backend/tools/toolexec_pack_goobj.sh" \
    -o "$OUT/qjs_test" . )

echo "=== [4/4] run on the goroutine stack ==="
set +e
timeout 20 "$OUT/qjs_test"
rc=$?
if [[ $rc -eq 0 ]]; then
  # Golden stack-growth test: fresh goroutines with varied stack pre-fill so
  # morestack copies land at many different points inside parser/interpreter
  # C frames (frame addresses live across calls, callee-saved regs, spills).
  echo "--- growth sweep (QJS_GROWTH_SWEEP=${QJS_GROWTH_SWEEP:-300}) ---"
  QJS_GROWTH_SWEEP="${QJS_GROWTH_SWEEP:-300}" timeout 120 "$OUT/qjs_test"
  rc=$?
fi
set -e
if [[ $rc -eq 0 ]]; then
  echo "PASS qjs-build"
else
  echo "qjs-build: FAIL" >&2
fi
exit "$rc"
