# shellcheck shell=bash
# goc_converter ROOT: print the path of a gocobj2goobj for the Go in PATH.
#
# gocobj2goobj must be built against the object writer of the exact Go
# release that links its output (relocation numbers move between releases
# and the linker compares the object header byte for byte;
# docs/multi-go-version.md). tools/goc-goenc copies that writer out of the
# active GOROOT; the result is cached per Go release and converter source.
#
# ROOT holds go.mod, gocobj/, tools/gocobj2goobj and tools/goc-goenc (a
# checkout, or a release's src/). A release's prebuilt bin/gocobj2goobj is
# used when it was built for the same Go.
goc_converter() {
  local root="$1" ver goroot sum key cache dir
  ver="$(GOTOOLCHAIN=local go env GOVERSION)" || return 1
  goroot="$(GOTOOLCHAIN=local go env GOROOT)" || return 1
  local pre="${GOC_PREBUILT_CONVERTER:-}"
  if [[ -n "$pre" && -x "$pre" && "$("$pre" -version 2>/dev/null)" == "$ver" ]]; then
    echo "$pre"
    return 0
  fi
  sum="$( (cd "$root" && cat go.mod gocobj/*.go tools/gocobj2goobj/*.go tools/goc-goenc/*.go \
    && echo "$goroot") | sha256sum | cut -c1-12)"
  key="$ver-$sum"
  cache="${GOC_CACHE:-$root/build/goenc}"
  dir="$cache/$key"
  if [[ -x "$dir/gocobj2goobj" ]]; then
    echo "$dir/gocobj2goobj"
    return 0
  fi
  mkdir -p "$cache"
  (
    # Parallel compiles of one package share the cache entry.
    exec 9>"$dir.lock"
    flock 9
    [[ -x "$dir/gocobj2goobj" ]] && exit 0
    local tmp="$dir.tmp.$$"
    rm -rf "$tmp" && mkdir -p "$tmp"
    cd "$root" || exit 1
    GOTOOLCHAIN=local GOFLAGS= go run ./tools/goc-goenc -goroot "$goroot" -out "$tmp/enc" >&2 || exit 1
    local lang
    lang="$(sed -n 's/^go //p' "$tmp/enc/go.mod")"
    sed -e "s|=> ./goobj/enc|=> $tmp/enc|" -e "s|^go .*|go $lang|" go.mod >"$tmp/go.mod"
    : >"$tmp/go.sum"
    GOTOOLCHAIN=local GOFLAGS= CGO_ENABLED=0 GOOS= GOARCH= \
      go build -modfile="$tmp/go.mod" -o "$tmp/gocobj2goobj" ./tools/gocobj2goobj >&2 || exit 1
    # The modfile names the writer by absolute path; fix it up after the move.
    rm -rf "$dir" && mv "$tmp" "$dir"
    sed -i "s|=> $tmp/enc|=> $dir/enc|" "$dir/go.mod"
  ) || { echo "goc: building gocobj2goobj for $ver failed" >&2; return 1; }
  echo "$dir/gocobj2goobj"
}
