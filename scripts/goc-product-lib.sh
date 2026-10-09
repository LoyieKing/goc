# Shared by goc go / goc check / goc toolchain.
# Source this file. Caller sets ROOT and exports GOC_ROOT.
# Product defaults and flag parsing live in goc-flags.sh.

goc_die() { echo "goc: $*" >&2; exit 1; }

goc_prepend_lib() {
  local libdir="$1"
  [[ -n "$libdir" && -d "$libdir" ]] || return 0
  case ":${LD_LIBRARY_PATH:-}:" in
    *":$libdir:"*) ;;
    *) export LD_LIBRARY_PATH="$libdir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ;;
  esac
}

# Print the patched clang. Do not fall back to system clang-19: it has no
# goc Sema. Order: GOC_CLANG, GOC_TOOLCHAIN, the in-tree clang build, then
# an unpacked toolchain next to the repo or under ~/.goc.
goc_resolve_clang() {
  local c
  if [[ -n "${GOC_CLANG:-}" ]]; then
    [[ -x "$GOC_CLANG" ]] || goc_die "GOC_CLANG is not executable: $GOC_CLANG"
    echo "$GOC_CLANG"
    return 0
  fi
  local candidates=()
  [[ -n "${GOC_TOOLCHAIN:-}" ]] && candidates+=("$GOC_TOOLCHAIN/bin/clang")
  candidates+=(
    "$ROOT/third_party/llvm-19.1.7-clang-build/bin/clang"
    "$ROOT/third_party/llvm-clang-build/bin/clang"
    "$ROOT/third_party/goc-toolchain/bin/clang"
    "$HOME/.goc/toolchain/bin/clang"
  )
  for c in "${candidates[@]}"; do
    if [[ -x "$c" ]]; then
      echo "$c"
      return 0
    fi
  done
  goc_die "no patched clang. Build third_party/llvm-19.1.7-clang-build (docs/guide.md) or unpack a toolchain (docs/toolchain.md)."
}

goc_clang_libdir() {
  local clang="$1" bin
  bin="$(readlink -f "$clang")"
  cd "$(dirname "$bin")/../lib" && pwd
}

goc_first_exec() {
  local c
  for c in "$@"; do
    if [[ -n "$c" && -x "$c" ]]; then
      echo "$c"
      return 0
    fi
  done
  return 1
}

# Fill GOC_CLANG, LD_LIBRARY_PATH, OPT, LLC, GOC_COLOR_ESCAPE, GOC_STACKMAP.
goc_export_tools() {
  local clang libdir toolroot=""
  clang="$(goc_resolve_clang)"
  export GOC_CLANG="$clang"
  libdir="$(goc_clang_libdir "$clang")"
  goc_prepend_lib "$libdir"
  # A packed toolchain keeps passes next to bin/. An in-tree clang uses the
  # repo build outputs.
  toolroot="$(cd "$(dirname "$(readlink -f "$clang")")/.." && pwd)"
  if [[ -x "$toolroot/passes/goc-llc" ]]; then
    export LLC="${LLC:-$toolroot/passes/goc-llc}"
    export GOC_COLOR_ESCAPE="${GOC_COLOR_ESCAPE:-$toolroot/passes/goc-color-escape}"
    export GOC_STACKMAP="${GOC_STACKMAP:-$toolroot/passes/GocStackMap.so}"
    export OPT="${OPT:-$toolroot/bin/opt}"
  fi
  export LLC="${LLC:-$ROOT/backend/build/pass-out/goc-llc}"
  export GOC_STACKMAP="${GOC_STACKMAP:-$ROOT/backend/build/pass-out/GocStackMap.so}"
  # Leave GOC_COLOR_ESCAPE unset when the repo pass is not built yet.
  # `goc build` compiles that pass on first use. A packed pass is exported above.
  if [[ -z "${GOC_COLOR_ESCAPE:-}" && -x "$ROOT/frontend/color-escape/build/goc-color-escape" ]]; then
    export GOC_COLOR_ESCAPE="$ROOT/frontend/color-escape/build/goc-color-escape"
  fi
  if [[ -z "${OPT:-}" || ! -x "${OPT:-}" ]]; then
    OPT="$(command -v opt-19 || true)"
    export OPT
  fi
}

goc_require_tools() {
  goc_export_tools
  [[ -x "$GOC_CLANG" ]] || goc_die "clang missing"
  [[ -x "$LLC" ]] || goc_die "goc-llc missing ($LLC). Build the backend passes or unpack a toolchain."
  [[ -f "$GOC_STACKMAP" ]] || goc_die "GocStackMap.so missing ($GOC_STACKMAP)."
  [[ -n "${OPT:-}" && -x "$OPT" ]] || goc_die "opt missing. Put opt-19 on PATH or unpack a toolchain with bin/opt."
  if [[ -n "${GOC_COLOR_ESCAPE:-}" && ! -x "$GOC_COLOR_ESCAPE" ]]; then
    goc_die "goc-color-escape missing ($GOC_COLOR_ESCAPE)."
  fi
  local missing ldd_args=("$LLC")
  [[ -n "${GOC_COLOR_ESCAPE:-}" && -x "$GOC_COLOR_ESCAPE" ]] && ldd_args+=("$GOC_COLOR_ESCAPE")
  missing="$(ldd "${ldd_args[@]}" 2>/dev/null | awk '/not found/{print $1}' | sort -u)"
  if [[ -n "$missing" ]]; then
    goc_die "LLVM libraries not loaded ($missing). The clang lib dir was prepended to LD_LIBRARY_PATH; the toolchain libLLVM does not match these passes."
  fi
}

