#!/usr/bin/env bash
# 1) After asm -gensymabis for our package: append ABIInternal defs for binary-goobj TEXTs.
# 2) After compile -pack for our package: pack binary goobj into the .a.
set -euo pipefail
TOOL="$1"
shift
BASE="$(basename "$TOOL")"

PKG_NEEDLE="p5-machinepass-goobj"

is_our_pkg_args() {
  local prev="" arg
  for arg in "$@"; do
    if [[ "$prev" == "-p" ]] && [[ "$arg" == *"$PKG_NEEDLE"* || "$arg" == "main" ]]; then
      return 0
    fi
    prev="$arg"
  done
  return 1
}

append_goc_symabis() {
  local out="" prev="" arg
  for arg in "$@"; do
    if [[ "$prev" == "-o" ]]; then out="$arg"; fi
    prev="$arg"
  done
  [[ -n "$out" && -f "$out" ]] || return 0
  # Package path from -p
  local pkg="main" prev=""
  for arg in "$@"; do
    if [[ "$prev" == "-p" ]]; then pkg="$arg"; fi
    prev="$arg"
  done
  {
    echo "def ${pkg}.GocCheckedAdd ABIInternal"
    echo "def ${pkg}.GocHoldLive ABIInternal"
    echo "def ${pkg}.GocHoldArg ABIInternal"
    echo "def ${pkg}.StoreGptrWB ABIInternal"
    echo "def ${pkg}.GocHoldTwo ABIInternal"
    echo "def ${pkg}.GocHoldRegOnly ABIInternal"
    echo "def ${pkg}.GocFadd64 ABIInternal"
    echo "def ${pkg}.GocFadd32 ABIInternal"
  } >> "$out"
  # P29: ABIInternal defs for every goobj TEXT from the sidecar meta
  # (multi-function / goabi objects). Extra defs are harmless.
  # GOC_BINOBJ may be a space-separated list of goobj files (multi-TU builds).
  local obj meta
  for obj in ${GOC_BINOBJ:-}; do
    meta="${obj%.o}.meta.json"
    [[ -f "$meta" ]] || continue
    python3 -c '
import json, sys
m = json.load(open(sys.argv[1]))
abi = m.get("abi", "ABIInternal")
for f in m["functions"]:
    print("def %s %s" % (f["go_sym"], abi))
' "$meta" >> "$out"
    echo "toolexec: appended goc ABIInternal symabis from $(basename "$meta")" >&2
  done
  echo "toolexec: appended goc ABIInternal symabis → $out" >&2
}

inject_binobj() {
  local out_a="" prev="" arg
  for arg in "$@"; do
    if [[ "$prev" == "-o" && "$arg" == *.a ]]; then out_a="$arg"; fi
    prev="$arg"
  done
  [[ -n "${GOC_BINOBJ:-}" && -n "$out_a" && -f "$out_a" ]] || return 0
  local pack
  pack="$(dirname "$TOOL")/pack"
  [[ -x "$pack" ]] || pack="$(go env GOTOOLDIR)/pack"
  local f
  for f in $GOC_BINOBJ; do
    [[ -f "$f" ]] || continue
    "$pack" r "$out_a" "$f"
    echo "toolexec: packed $(basename "$f") into $out_a" >&2
  done
}

case "$BASE" in
  asm)
    "$TOOL" "$@"
    if is_our_pkg_args "$@" && [[ " $* " == *" -gensymabis "* ]]; then
      append_goc_symabis "$@"
    fi
    ;;
  compile)
    "$TOOL" "$@"
    if is_our_pkg_args "$@"; then
      inject_binobj "$@"
    fi
    ;;
  *)
    exec "$TOOL" "$@"
    ;;
esac
