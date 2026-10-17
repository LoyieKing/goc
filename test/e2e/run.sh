#!/usr/bin/env bash
# End-to-end cases: C through bin/goc into Go objects, linked into Go
# programs and run. Usage: test/e2e/run.sh [case...] (default: all).
#
#   realbody   one C file at -O0; the .gocobj verifies; the magic sentinel survives
#   repro      two -O2 builds give identical .gocobj/.goobj; 10 conversions agree
#   goabi      Go -> C through the ABIInternal thunks (scalars, aggregates, stack args)
#   go_extern  C -> Go stubs, including stack growth
#   stress     C recursion in every stack-check tier while GCs move goroutine stacks
#   retptr     returned stack pointers at -O2 and -O3
#   manyfuncs  600 functions in one object
#   lines      C file/line numbers in Go tracebacks, inlined frames
#   arm64      the linux/arm64 corpus (arm64/run.sh)
#   arm64-stress, arm64-retptr, arm64-stackargs   under qemu-aarch64
#
# arm64 cases need go1.21+; without qemu-aarch64 they are skipped unless
# ARM64_REQUIRE_QEMU=1.
set -euo pipefail
# shellcheck source=../lib.sh
source "$(cd "$(dirname "$0")/.." && pwd)/lib.sh"
E="$ROOT/test/e2e"
B="$ROOT/build/test"
fail() { echo "FAIL $*" >&2; exit 1; }

# The switches `goc build` defaults to, for goc go (which defaults to -O3 cptr).
PLAIN=(--no-default-ptr-color --no-fast-stack-alloca)

# gogo NAME DIR [goc go switches]: build DIR into $B/NAME/NAME, objects in $B/NAME.
gogo() {
  local name="$1" dir="$2"
  shift 2
  mkdir -p "$B/$name"
  "$GOC" go "$dir" -o "$B/$name/$name" --objdir "$B/$name" "$@" >"$B/$name/build.log" 2>&1 \
    || { tail -30 "$B/$name/build.log" >&2; fail "$name: goc go"; }
}

verify() { "$GOC" tool objdump verify "$@"; }

# run2 BIN PATTERN [runner]: run twice (default GOMAXPROCS, then 1); stdout must match.
run2() {
  local bin="$1" pat="$2" r=("${@:3}")
  "${r[@]}" "$bin" | tee "$bin.run.log"
  grep -q "$pat" "$bin.run.log" || fail "$bin: no $pat"
  GOMAXPROCS=1 "${r[@]}" "$bin" | grep -q "$pat" || fail "$bin (GOMAXPROCS=1): no $pat"
}

goversion_ok() {
  local v m
  v="$(go env GOVERSION)"
  m="${v#go1.}"
  m="${m%%[!0-9]*}"
  if [[ -n "$m" ]] && (( m < 21 )); then
    echo "SKIP $1: goc supports arm64 from go1.21 ($v)"
    return 1
  fi
}

QEMU="$(command -v qemu-aarch64 || command -v qemu-aarch64-static || true)"
qemu_ok() {
  goversion_ok "$1" || return 1
  if [[ -z "$QEMU" ]]; then
    [[ -z "${ARM64_REQUIRE_QEMU:-}" ]] || fail "$1: no qemu-aarch64"
    echo "SKIP $1: no qemu-aarch64"
    return 1
  fi
}

case_realbody() {
  local o="$B/realbody"
  mkdir -p "$o"
  "$GOC" build "$E/realbody/realbody.c" -o "$o/realbody.goobj" 2>&1 | tee "$o/build.log"
  verify "$o/realbody.gocobj"
  grep -q 'magic 0x28C0DE42' "$o/build.log" || fail "realbody: magic sentinel missing"
  echo "PASS realbody"
}

case_repro() {
  local o="$B/repro" d i
  rm -rf "$o"
  mkdir -p "$o/a" "$o/b"
  for d in a b; do
    "$GOC" build "$E/repro/repro.c" -o "$o/$d/repro.goobj" -O2 >"$o/$d/build.log" 2>&1 \
      || { cat "$o/$d/build.log" >&2; fail "repro: build"; }
  done
  for f in repro.gocobj repro.goobj; do
    cmp "$o/a/$f" "$o/b/$f" || fail "repro: $f differs between two builds"
  done
  for i in $(seq 10); do
    "$GOC" tool convert -in "$o/a/repro.gocobj" -o "$o/conv$i.goobj" -symabis "$o/conv$i.symabis"
    cmp "$o/conv1.goobj" "$o/conv$i.goobj" && cmp "$o/conv1.symabis" "$o/conv$i.symabis" \
      || fail "repro: conversion $i differs"
  done
  echo "PASS repro"
}

case_goabi() {
  gogo goabi "$E/goabi" -O0 "${PLAIN[@]}"
  go tool nm "$B/goabi/goabi.goobj" >"$B/goabi/nm.txt"
  for s in goabi_add2 goabi_add2.impl goabi_addl; do
    grep -q " T main\.$s\$" "$B/goabi/nm.txt" || fail "goabi: no main.$s"
  done
  "$B/goabi/goabi" | tee "$B/goabi/run.log"
  grep -q '^PASS goabi' "$B/goabi/run.log" || fail "goabi"
}

case_go_extern() {
  gogo go_extern "$E/go_extern" -O0 "${PLAIN[@]}"
  grep -q 'go_extern: 3 stubs' "$B/go_extern/build.log" || fail "go_extern: not 3 stubs"
  go tool nm "$B/go_extern/go_extern.goobj" | grep -q ' T main\.go_extern_probe$' || fail "go_extern: no probe"
  "$B/go_extern/go_extern" | tee "$B/go_extern/run.log"
  grep -q '^PASS go_extern' "$B/go_extern/run.log" || fail "go_extern"
}

case_stress() {
  gogo stress "$E/stress" -O2 "${PLAIN[@]}"
  verify "$B/stress/stress.gocobj"
  run2 "$B/stress/stress" '^PASS stress'
  echo "PASS stress"
}

case_retptr() {
  local O
  for O in 2 3; do
    gogo retptr "$E/retptr" -O$O --default-ptr-color cptr --no-fast-stack-alloca
    run2 "$B/retptr/retptr" '^PASS retptr'
  done
  echo "PASS retptr (amd64)"
}

case_manyfuncs() {
  local o="$B/manyfuncs" n=600 i
  rm -rf "$o"
  mkdir -p "$o/src"
  {
    echo '#include "goc.h"'
    for i in $(seq 0 $((n - 1))); do
      echo "__attribute__((noinline)) long mf_$i(long x) { return x + $i; }"
    done
    echo 'long manyfuncs_sum(long x) {'
    echo '  long s = 0;'
    for i in $(seq 0 $((n - 1))); do echo "  s += mf_$i(x);"; done
    echo '  return s;'
    echo '}'
  } >"$o/src/manyfuncs.c"
  cat >"$o/src/main.go" <<'GO'
package main

import (
	"fmt"
	"os"
)

func manyfuncs_sum(x int64) int64

func main() {
	if got, want := manyfuncs_sum(1), int64(600+599*600/2); got != want {
		fmt.Printf("FAIL manyfuncs: got %d want %d\n", got, want)
		os.Exit(1)
	}
	fmt.Println("PASS manyfuncs")
}
GO
  printf 'module goc.local/manyfuncs\n\ngo 1.18\n' >"$o/src/go.mod"
  gogo manyfuncs "$o/src" -O1 "${PLAIN[@]}"
  "$o/manyfuncs"
}

case_lines() {
  gogo lines "$E/lines" -O2 "${PLAIN[@]}"
  verify "$B/lines/lines.gocobj"
  "$B/lines/lines" | tee "$B/lines/run.log"
  grep -q '^PASS lines' "$B/lines/run.log" || fail "lines"
}

case_arm64() {
  goversion_ok arm64 || return 0
  "$E/arm64/run.sh"
}

case_arm64-stress() {
  qemu_ok "arm64 stress" || return 0
  gogo stress-arm64 "$E/stress" --arch arm64 -O2 "${PLAIN[@]}"
  verify "$B/stress-arm64/stress.gocobj"
  run2 "$B/stress-arm64/stress-arm64" '^PASS stress' timeout 600 "$QEMU"
  echo "PASS arm64 stress"
}

case_arm64-stackargs() {
  qemu_ok "arm64 stackargs" || return 0
  gogo stackargs-arm64 "$E/arm64/stackargs" --arch arm64 -O2 "${PLAIN[@]}"
  local got
  got="$(timeout 60 "$QEMU" "$B/stackargs-arm64/stackargs-arm64" 2>&1)"
  [[ "$got" == "$(printf '801\n536')" ]] || fail "arm64 stackargs: $got"
  echo "PASS arm64 stack arguments"
}

case_arm64-retptr() {
  qemu_ok "arm64 retptr" || return 0
  local O
  for O in 2 3; do
    gogo retptr-arm64 "$E/retptr" --arch arm64 -O$O --default-ptr-color cptr --no-fast-stack-alloca
    verify "$B/retptr-arm64/retptr.gocobj"
    run2 "$B/retptr-arm64/retptr-arm64" '^PASS retptr' timeout 600 "$QEMU"
  done
  echo "PASS retptr (arm64/qemu)"
}

ALL=(realbody repro goabi go_extern stress retptr manyfuncs lines arm64 arm64-stress arm64-stackargs arm64-retptr)
cases=("$@")
[[ ${#cases[@]} -gt 0 ]] || cases=("${ALL[@]}")
for c in "${cases[@]}"; do
  declare -F "case_$c" >/dev/null || fail "unknown case $c (cases: ${ALL[*]})"
  echo "=== e2e $c ==="
  "case_$c"
done
echo "=== e2e: ${#cases[@]} cases passed ==="
