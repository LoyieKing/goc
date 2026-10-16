# shellcheck shell=bash
# goc_converter ROOT: print the path of gocobj2goobj.
#
# One converter serves every supported Go release: it writes the object
# itself from the release's facts (gometa/data/*.json, made by
# tools/golangc-metadata-resolver; docs/multi-go-version.md) and picks them
# by `go env GOVERSION`. A release ships it prebuilt as bin/gocobj2goobj; a
# checkout builds it once per converter source into build/conv.
goc_converter() {
  local root="$1" sum dir
  if [[ ! -f "$root/go.mod" ]]; then
    [[ -x "$root/bin/gocobj2goobj" ]] || { echo "goc: no $root/bin/gocobj2goobj" >&2; return 1; }
    echo "$root/bin/gocobj2goobj"
    return 0
  fi
  sum="$( (cd "$root" && cat go.mod gocobj/*.go gometa/*.go gometa/data/*.json goobjw/*.go \
    tools/gocobj2goobj/*.go) | sha256sum | cut -c1-12)"
  dir="${GOC_CACHE:-$root/build/conv}/$sum"
  if [[ -x "$dir/gocobj2goobj" ]]; then
    echo "$dir/gocobj2goobj"
    return 0
  fi
  mkdir -p "$dir"
  (
    # Parallel compiles share the cache entry.
    exec 9>"$dir.lock"
    flock 9
    [[ -x "$dir/gocobj2goobj" ]] && exit 0
    cd "$root" || exit 1
    GOFLAGS= GOROOT= GOTOOLDIR= CGO_ENABLED=0 GOOS= GOARCH= "${GOC_HOST_GO:-go}" build -o "$dir/gocobj2goobj.tmp.$$" ./tools/gocobj2goobj >&2 \
      && mv "$dir/gocobj2goobj.tmp.$$" "$dir/gocobj2goobj"
  ) || { echo "goc: building gocobj2goobj failed" >&2; return 1; }
  echo "$dir/gocobj2goobj"
}
