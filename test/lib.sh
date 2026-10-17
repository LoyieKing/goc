# shellcheck shell=bash
# Sourced by the test runners. Sets ROOT and GOC (bin/goc, rebuilt from
# cmd/goc with the host Go), and the tool paths `goc env` reports:
# CLANG OPT LIBDIR PASSES LLC INCLUDE. Nothing is exported: goc puts LIBDIR
# on LD_LIBRARY_PATH for the tools it runs, and an exported LIBDIR would
# make LLVM 19's own opt (lit's) load the clang tree's libLLVM.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GOC="$ROOT/bin/goc"
(cd "$ROOT" && CGO_ENABLED=0 GOFLAGS= GOTOOLCHAIN=local "${GOC_HOST_GO:-go}" build -o bin/goc ./cmd/goc) \
  || { echo "test: building bin/goc failed" >&2; exit 1; }
_goc_env="$("$GOC" env)" || exit 1
_goc_get() { sed -n "s/^$1: //p" <<<"$_goc_env"; }
CLANG="$(_goc_get clang)" OPT="$(_goc_get opt)" LIBDIR="$(_goc_get libdir)"
PASSES="$(_goc_get passes)" LLC="$(_goc_get goc-llc)" INCLUDE="$(_goc_get include)"
unset _goc_env
