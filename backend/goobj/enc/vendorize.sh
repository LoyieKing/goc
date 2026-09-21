#!/usr/bin/env bash
# Vendor Go toolchain encode packages into goobj/enc for overlay-free builds.
# No git clone. Copies from local GOROOT and rewrites imports.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ENC="$ROOT/goobj/enc"
GOROOT="$(go env GOROOT)"
MOD="goc.local/p5-machinepass-goobj/goobj/enc"

rm -rf "$ENC/cmd" "$ENC/stdinternal"
mkdir -p "$ENC"

copy_pkg() {
  local src="$1" dst="$2"
  mkdir -p "$dst"
  # Copy non-test .go files only; skip arch subdirs we don't need unless whole pkg
  find "$src" -maxdepth 1 -type f -name '*.go' ! -name '*_test.go' -exec cp {} "$dst/" \;
}

# cmd/internal packages
for pkg in bio dwarf goobj hash obj objabi src sys; do
  copy_pkg "$GOROOT/src/cmd/internal/$pkg" "$ENC/cmd/internal/$pkg"
done
copy_pkg "$GOROOT/src/cmd/internal/obj/x86" "$ENC/cmd/internal/obj/x86"
# Drop other obj arch dirs — not imported by amd64 path when we only import x86

# std internal packages that cmd/internal/obj needs
for pkg in abi buildcfg goarch coverage/rtcov runtime/sys; do
  mkdir -p "$ENC/stdinternal/$(dirname $pkg)"
  copy_pkg "$GOROOT/src/internal/$pkg" "$ENC/stdinternal/$pkg"
done

# Also need goos? buildcfg imports it
if [[ -d "$GOROOT/src/internal/goos" ]]; then
  copy_pkg "$GOROOT/src/internal/goos" "$ENC/stdinternal/goos"
fi
# goexperiment
if [[ -d "$GOROOT/src/internal/goexperiment" ]]; then
  copy_pkg "$GOROOT/src/internal/goexperiment" "$ENC/stdinternal/goexperiment"
fi

# Rewrite imports in all vendored .go files
while IFS= read -r -d '' f; do
  # cmd/internal/X → goc.local/.../cmd/internal/X
  sed -i -E "s|\"cmd/internal/([^\"]+)\"|\"${MOD}/cmd/internal/\1\"|g" "$f"
  # internal/X → goc.local/.../stdinternal/X  (only our vendored set)
  sed -i -E "s|\"internal/(abi|buildcfg|goarch|goos|goexperiment|coverage/rtcov|runtime/sys)\"|\"${MOD}/stdinternal/\1\"|g" "$f"
done < <(find "$ENC" -name '*.go' -print0)

# go.mod for enc (same module as parent — use replace from root)
# Ensure root go.mod exists
if [[ ! -f "$ROOT/go.mod" ]]; then
  (cd "$ROOT" && go mod init goc.local/p5-machinepass-goobj)
fi

echo "Vendored into $ENC"
# Quick compile check of obj package
(cd "$ROOT" && go build ./goobj/enc/cmd/internal/obj/...) || {
  echo "NOTE: first build may need more stdinternal packages; fixing..."
  exit 1
}
