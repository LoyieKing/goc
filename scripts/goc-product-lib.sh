# Shared by goc go / goc check / goc toolchain, and by cmd/goc.
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

# Patched Clang that ships with this tree. There is no flag and no
# environment variable to point at another one. A release archive has
# bin/clang. A checkout that built from source has the third_party tree.
goc_resolve_clang() {
  local c
  local candidates=(
    "$ROOT/bin/clang"
    "$ROOT/third_party/llvm-19.1.7-clang-build/bin/clang"
    "$ROOT/third_party/llvm-clang-build/bin/clang"
  )
  for c in "${candidates[@]}"; do
    if [[ -x "$c" ]]; then
      echo "$c"
      return 0
    fi
  done
  goc_die "no patched clang next to goc.
Download a release: https://github.com/LoyieKing/goc/releases
Or build the package: docs/build-from-source.md"
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

# A tool installed next to the patched clang (opt, llc, llvm-mc, ...).
goc_beside() {
  local name="$1" clang bin
  clang="$(goc_resolve_clang)"
  bin="$(dirname "$(readlink -f "$clang")")"
  goc_first_exec "$bin/$name" "$ROOT/bin/$name"
}

# Fill LD_LIBRARY_PATH, OPT, LLC, GOC_STACKMAP.
# Clang itself is found by goc_resolve_clang. Callers do not export a path.
goc_export_tools() {
  local clang libdir toolroot=""
  clang="$(goc_resolve_clang)"
  libdir="$(goc_clang_libdir "$clang")"
  goc_prepend_lib "$libdir"
  # A release keeps passes next to bin/. A checkout uses the repo build outputs.
  toolroot="$(cd "$(dirname "$(readlink -f "$clang")")/.." && pwd)"
  if [[ -x "$toolroot/passes/goc-llc" ]]; then
    export LLC="${LLC:-$toolroot/passes/goc-llc}"
    export GOC_STACKMAP="${GOC_STACKMAP:-$toolroot/passes/libGocPasses.so}"
    export OPT="${OPT:-$toolroot/bin/opt}"
  fi
  if [[ -z "${LLC:-}" && -x "$ROOT/passes/goc-llc" ]]; then
    export LLC="$ROOT/passes/goc-llc"
  fi
  export LLC="${LLC:-$ROOT/build/llvm/goc-llc}"
  if [[ -z "${GOC_STACKMAP:-}" && -f "$ROOT/passes/libGocPasses.so" ]]; then
    export GOC_STACKMAP="$ROOT/passes/libGocPasses.so"
  fi
  export GOC_STACKMAP="${GOC_STACKMAP:-$ROOT/build/llvm/libGocPasses.so}"
  if [[ -z "${OPT:-}" || ! -x "${OPT:-}" ]]; then
    OPT="$(goc_beside opt 2>/dev/null || true)"
    if [[ -z "$OPT" ]]; then
      OPT="$(command -v opt-19 || command -v opt || true)"
    fi
    export OPT
  fi
}

# goc-lower replaces the real-body Python and ripgrep steps. A release
# ships bin/goc-lower. A checkout builds it on first use.
goc_lower_bin() {
  local bin="$ROOT/bin/goc-lower" stale=0 src
  if [[ ! -x "$bin" ]]; then
    stale=1
  else
    for src in "$ROOT/tools/goc-lower/"*.go "$ROOT/goobj/lower/"*.go; do
      if [[ -f "$src" && "$src" -nt "$bin" ]]; then
        stale=1
        break
      fi
    done
  fi
  if [[ $stale -eq 1 ]]; then
    if [[ ! -f "$ROOT/tools/goc-lower/main.go" ]]; then
      goc_die "goc-lower missing ($bin). See docs/build-from-source.md"
    fi
    mkdir -p "$ROOT/bin"
    ( cd "$ROOT" && GOPROXY=off go build -o "$bin" ./tools/goc-lower/ ) \
      || goc_die "could not build goc-lower"
  fi
  printf '%s\n' "$bin"
}

goc_require_tools() {
  local clang
  goc_export_tools
  clang="$(goc_resolve_clang)"
  [[ -x "$clang" ]] || goc_die "clang missing"
  [[ -x "$LLC" ]] || goc_die "goc-llc missing ($LLC). See docs/build-from-source.md."
  [[ -f "$GOC_STACKMAP" ]] || goc_die "libGocPasses.so missing ($GOC_STACKMAP)."
  [[ -n "${OPT:-}" && -x "$OPT" ]] || goc_die "opt missing. See docs/build-from-source.md."
  local missing ldd_args=("$LLC")
  missing="$(ldd "${ldd_args[@]}" 2>/dev/null | awk '/not found/{print $1}' | sort -u)"
  if [[ -n "$missing" ]]; then
    goc_die "LLVM libraries not loaded ($missing). The clang lib dir was prepended to LD_LIBRARY_PATH; libLLVM does not match these passes."
  fi
}
