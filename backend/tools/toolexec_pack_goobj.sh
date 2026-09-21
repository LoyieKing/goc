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
  echo "toolexec: appended goc ABIInternal symabis → $out" >&2
}

inject_binobj() {
  local out_a="" prev="" arg
  for arg in "$@"; do
    if [[ "$prev" == "-o" && "$arg" == *.a ]]; then out_a="$arg"; fi
    prev="$arg"
  done
  [[ -n "${GOC_BINOBJ:-}" && -f "${GOC_BINOBJ}" && -n "$out_a" && -f "$out_a" ]] || return 0
  local pack
  pack="$(dirname "$TOOL")/pack"
  [[ -x "$pack" ]] || pack="$(go env GOTOOLDIR)/pack"
  "$pack" r "$out_a" "$GOC_BINOBJ"
  echo "toolexec: packed binary goobj into $out_a" >&2
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
