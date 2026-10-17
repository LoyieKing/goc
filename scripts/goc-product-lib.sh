# Shared by cmd/goc, goc go / goc check / goc toolchain, realbody and the tests.
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

# The tools of this tree. A release has bin/{clang,opt,llvm-mc} and
# passes/{goc-llc,libGocPasses.so}; a checkout has the patched clang build
# in third_party/llvm-19.1.7-clang-build and the passes in build/llvm.
# There is no flag and no environment variable to point at other tools.
goc_resolve_clang() {
  local c
  for c in "$ROOT/bin/clang" "$ROOT/third_party/llvm-19.1.7-clang-build/bin/clang"; do
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

# Set GOC_CLANG GOC_OPT GOC_MC GOC_LLC GOC_PASSES and put the clang tree's
# libLLVM on LD_LIBRARY_PATH (goc-llc and the passes load it).
goc_tools() {
  local bin
  GOC_CLANG="$(goc_resolve_clang)"
  bin="$(dirname "$(readlink -f "$GOC_CLANG")")"
  GOC_OPT="$bin/opt"
  GOC_MC="$bin/llvm-mc"
  if [[ -x "$ROOT/passes/goc-llc" ]]; then
    GOC_LLC="$ROOT/passes/goc-llc"
    GOC_PASSES="$ROOT/passes/libGocPasses.so"
  else
    GOC_LLC="$ROOT/build/llvm/goc-llc"
    GOC_PASSES="$ROOT/build/llvm/libGocPasses.so"
  fi
  goc_prepend_lib "$(goc_clang_libdir "$GOC_CLANG")"
}

# goc-lower replaces the real-body Python and ripgrep steps. A release
# ships bin/goc-lower. A checkout builds it on first use.
goc_lower_bin() {
  local bin="$ROOT/bin/goc-lower" stale=0 src
  if [[ ! -x "$bin" ]]; then
    stale=1
  else
    for src in "$ROOT/tools/goc-lower/"*.go "$ROOT/internal/lower/"*.go; do
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
    ( cd "$ROOT" && GOPROXY=off GOROOT= GOTOOLDIR= "${GOC_HOST_GO:-go}" build -o "$bin" ./tools/goc-lower/ ) \
      || goc_die "could not build goc-lower"
  fi
  printf '%s\n' "$bin"
}

goc_require_tools() {
  goc_tools
  [[ -x "$GOC_OPT" ]] || goc_die "opt missing ($GOC_OPT). See docs/build-from-source.md."
  [[ -x "$GOC_MC" ]] || goc_die "llvm-mc missing ($GOC_MC). See docs/build-from-source.md."
  [[ -x "$GOC_LLC" ]] || goc_die "goc-llc missing ($GOC_LLC). Run scripts/build-passes.sh."
  [[ -f "$GOC_PASSES" ]] || goc_die "libGocPasses.so missing ($GOC_PASSES). Run scripts/build-passes.sh."
  local missing
  missing="$(ldd "$GOC_LLC" 2>/dev/null | awk '/not found/{print $1}' | sort -u)"
  if [[ -n "$missing" ]]; then
    goc_die "LLVM libraries not loaded ($missing). The clang lib dir was prepended to LD_LIBRARY_PATH; libLLVM does not match these passes."
  fi
}
