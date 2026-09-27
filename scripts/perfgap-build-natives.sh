#!/usr/bin/env bash
# Build the native reference QuickJS binaries used by docs/perf-gap.md.
#
#   PS=/workspace/perf-study scripts/perfgap-build-natives.sh
#
# Expects $PS/native (quickjs-ng 0.17.0 source tree, the one native ng is built
# from) and $PS/bellard/quickjs-2026-06-04.tar.xz. Existing builds are left alone
# unless FORCE=1. Outputs:
#   ng:      $PS/native/b-gcc-O2/qjs      gcc   -O2 -DNDEBUG          (CMake Release)
#            $PS/native/b-O2/qjs          clang -O2 -DNDEBUG          (the native ng baseline)
#            $PS/native/b-O3/qjs          clang -O3 -DNDEBUG
#            $PS/native/b-gocfull/qjs     clang -O3 -DNDEBUG + goc codegen flags
#   Bellard: $PS/bellard/quickjs-2026-06-04/qjs   gcc   -O2 (upstream default, asserts on)
#            $PS/bellard/gcc-O2-NDEBUG/qjs        gcc   -O2 -DNDEBUG
#            $PS/bellard/clang-O2/qjs             clang -O2 (asserts on)
#            $PS/bellard/clang-O2-NDEBUG/qjs      clang -O2 -DNDEBUG
#            $PS/bellard/clang-O3-NDEBUG/qjs      clang -O3 -DNDEBUG
#            $PS/bellard/clang-O3-gocfull/qjs     clang -O3 -DNDEBUG + goc codegen flags
# Bellard builds use the upstream Makefile; only the CFLAGS_OPT line changes
# (CONFIG_CLANG=y CC=clang-19 for clang). Upstream CFLAGS keep -funsigned-char
# and -fwrapv; ng's CMake keeps -funsigned-char (goc passes neither).
set -euo pipefail
PS="${PS:-/workspace/perf-study}"
CMAKE="${CMAKE:-cmake}"
# goc's codegen constraints (cmd/goc clang flags + backend/realbody llc flags);
# override-stack-alignment=8 is approximated by -mstack-alignment=8.
GF_FULL="-fwrapv -fno-strict-aliasing -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer -fno-optimize-sibling-calls -mno-red-zone -fno-stack-protector -fno-asynchronous-unwind-tables -mstack-alignment=8 -mllvm -no-stack-slot-sharing -mllvm -enable-shrink-wrap=false -mllvm -no-x86-call-frame-opt -mllvm -enable-tail-merge=false"

ng() {  # ng DIR CC FLAGS
  local d="$PS/native/$1"
  [[ -x "$d/qjs" && "${FORCE:-0}" != 1 ]] && { echo "keep $d/qjs"; return; }
  rm -rf "$d"
  "$CMAKE" -S "$PS/native" -B "$d" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="$2" "-DCMAKE_C_FLAGS_RELEASE=$3" > "$d.log" 2>&1
  "$CMAKE" --build "$d" --target qjs_exe -j1 >> "$d.log" 2>&1
  echo "built $d/qjs"
}
bellard() {  # bellard NAME CC MAKEARGS CFLAGS_OPT
  local out="$PS/bellard/$1" tmp
  [[ -x "$out/qjs" && "${FORCE:-0}" != 1 ]] && { echo "keep $out/qjs"; return; }
  tmp="$(mktemp -d)"
  tar -xJf "$PS/bellard/quickjs-2026-06-04.tar.xz" -C "$tmp"
  sed -i "s|^CFLAGS_OPT=\$(CFLAGS) -O2|CFLAGS_OPT=\$(CFLAGS) $4|" "$tmp/quickjs-2026-06-04/Makefile"
  (cd "$tmp/quickjs-2026-06-04" && make -j1 $3 CC="$2" HOST_CC="$2" qjs > build.log 2>&1) || { echo "FAIL $1 (see $tmp)"; return 0; }
  mkdir -p "$out"; cp "$tmp/quickjs-2026-06-04/qjs" "$tmp/quickjs-2026-06-04/build.log" "$out/"
  rm -rf "$tmp"; echo "built $out/qjs"
}
ng b-gcc-O2   /usr/bin/gcc      "-O2 -DNDEBUG"
ng b-O2       /usr/bin/clang-19 "-O2 -DNDEBUG"
ng b-O3       /usr/bin/clang-19 "-O3 -DNDEBUG"
ng b-gocfull  /usr/bin/clang-19 "-O3 -DNDEBUG $GF_FULL"
bellard gcc-O2-NDEBUG     gcc      ""              "-O2 -DNDEBUG"
bellard clang-O2          clang-19 CONFIG_CLANG=y  "-O2"
bellard clang-O2-NDEBUG   clang-19 CONFIG_CLANG=y  "-O2 -DNDEBUG"
bellard clang-O3-NDEBUG   clang-19 CONFIG_CLANG=y  "-O3 -DNDEBUG"
bellard clang-O3-gocfull  clang-19 CONFIG_CLANG=y  "-O3 -DNDEBUG $GF_FULL"

# ---- toggle builds (docs/perf-gap.md, "mechanism experiments")
# Tail duplication of the computed-goto dispatch: LLVM only duplicates a block
# into at most 16 predecessors / successors by default (tail-dup-pred-size,
# tail-dup-succ-size), so JS_CallInternal keeps one shared `jmp *` for ~250
# handlers. gcc duplicates it into every handler.
TD="-mllvm -tail-dup-pred-size=1000 -mllvm -tail-dup-succ-size=1000"
ng b-O3-taildup           /usr/bin/clang-19 "-O3 -DNDEBUG $TD"
ng b-gocfull-taildup      /usr/bin/clang-19 "-O3 -DNDEBUG $GF_FULL $TD"
bellard clang-O3-NDEBUG-taildup  clang-19 CONFIG_CLANG=y "-O3 -DNDEBUG $TD"
bellard clang-O3-gocfull-taildup clang-19 CONFIG_CLANG=y "-O3 -DNDEBUG $GF_FULL $TD"
bellard clang-O2-NDEBUG-taildup  clang-19 CONFIG_CLANG=y "-O2 -DNDEBUG $TD"
# One goc flag at a time on top of Bellard clang -O3 -DNDEBUG (upstream CFLAGS
# already have -fwrapv). -fsigned-char: goc does not pass -funsigned-char.
flag1() { bellard "clang-O3-f-$1" clang-19 CONFIG_CLANG=y "-O3 -DNDEBUG $2"; }
flag1 nsa        "-fno-strict-aliasing"
flag1 fp         "-fno-omit-frame-pointer -mno-omit-leaf-frame-pointer"
flag1 nosib      "-fno-optimize-sibling-calls"
flag1 noredzone  "-mno-red-zone"
flag1 align8     "-mstack-alignment=8"
flag1 noslotshare "-mllvm -no-stack-slot-sharing"
flag1 noshrink   "-mllvm -enable-shrink-wrap=false"
flag1 nocfo      "-mllvm -no-x86-call-frame-opt"
flag1 notailmerge "-mllvm -enable-tail-merge=false"
flag1 signedchar "-fsigned-char"
# goc's pipeline natively (scripts/perfgap-gocpipe-cc.sh): the IR pipeline and
# llc flags of goc without its runtime; -sm adds the stack maps + frame guards.
GP="$(cd "$(dirname "$0")" && pwd)/perfgap-gocpipe-cc.sh"
GOCPIPE_MODE=pipe    bellard gocpipe    "$GP" CONFIG_CLANG=y "-O3 -DNDEBUG"
GOCPIPE_MODE=pipe-sm bellard gocpipe-sm "$GP" CONFIG_CLANG=y "-O3 -DNDEBUG"
# the same with default<O3>'s inliner threshold restored (-inline-threshold=250)
GOCPIPE_MODE=pipe GOCPIPE_OPT_EXTRA=-inline-threshold=250 bellard gocpipe-inl250 "$GP" CONFIG_CLANG=y "-O3 -DNDEBUG"
GOCPIPE_MODE=pipe-sm GOCPIPE_OPT_EXTRA=-inline-threshold=250 bellard gocpipe-sm-inl250 "$GP" CONFIG_CLANG=y "-O3 -DNDEBUG"
