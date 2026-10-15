#!/usr/bin/env bash
# -toolexec hook. goc already wrote the .goobj. This script makes Go's asm
# and compile stages recognize it:
#   1) After asm -gensymabis: append defs for the .goobj TEXTs. asm only
#      scanned .s files.
#   2) After compile: pack that .goobj into the package archive.
# The Go linker loads an archive member only when its name ends in .o or
# .syso (cmd/link, names shorter than 16 bytes). The packed member is
# therefore named .o. The file on disk stays .goobj.
set -euo pipefail
TOOL="$1"
shift
BASE="$(basename "$TOOL")"

is_our_pkg_args() {
  local prev="" arg
  local want="${GOC_PACK_PKG:-}"
  for arg in "$@"; do
    if [[ "$prev" == "-p" ]]; then
      # goc go sets GOC_PACK_PKG to the compiler -p value (main, or the
      # import path). Unset matches package main only. Other packages are not
      # packed, so a dependency is left alone.
      if [[ -n "$want" ]]; then
        [[ "$arg" == "$want" ]] && return 0
      elif [[ "$arg" == "main" ]]; then
        return 0
      fi
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
  # Defs for every TEXT in the sidecar meta. Extra defs are harmless.
  # GOC_BINOBJ may be a space-separated list (multi-TU builds).
  # foo.goobj and the legacy foo.o both sit beside foo.meta.json.
  local obj meta side
  for obj in ${GOC_BINOBJ:-}; do
    side="$obj"
    case "$side" in
      *.goobj) side="${side%.goobj}" ;;
      *.o) side="${side%.o}" ;;
    esac
    meta="${side}.meta.json"
    [[ -f "$meta" ]] || continue
    lower="${GOC_LOWER:-${GOC_ROOT:-}/bin/goc-lower}"
    if [[ ! -x "$lower" ]]; then
      echo "toolexec: goc-lower missing ($lower)" >&2
      exit 1
    fi
    "$lower" symabis "$meta" >> "$out"
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
  # Member name must end in .o. See the header comment.
  local stage f base
  stage="$(mktemp -d)"
  trap 'rm -rf "$stage"' RETURN
  for f in $GOC_BINOBJ; do
    [[ -f "$f" ]] || continue
    base="$(basename "$f")"
    case "$base" in
      *.goobj) base="${base%.goobj}.o" ;;
    esac
    cp "$f" "$stage/$base"
    "$pack" r "$out_a" "$stage/$base"
    echo "toolexec: packed $(basename "$f") into $out_a" >&2
  done
  rm -rf "$stage"
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
