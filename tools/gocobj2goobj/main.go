// gocobj2goobj converts a .gocobj (docs/gocobj-format.md) into a Go object
// file.
//
//	gocobj2goobj -in foo.gocobj -meta foo.meta.json -maps DIR -o foo.goobj [-p main]
//
// Plan row 5, transitional: the file is checked with gocobj.Verify and its
// ABI profile must match this converter's Go version; the conversion itself
// still runs the elfpack code (goobj/elfpack), with meta.json and the maps
// directory for what the .goc.* tables do not carry yet. Later rows move
// that information into the tables and replace elfpack piece by piece.
package main

import (
	"flag"
	"fmt"
	"os"

	"github.com/LoyieKing/goc/gocobj"
	"github.com/LoyieKing/goc/goobj/elfpack"
)

// profileFor is the ABI profile each target architecture of this converter
// expects. It changes when the vendored goobj writer moves to a new Go.
var profileFor = map[string]string{
	"amd64": "go1.24-amd64",
	"arm64": "go1.24-arm64",
}

func main() {
	in := flag.String("in", "", "input .gocobj")
	meta := flag.String("meta", "", "meta.json sidecar")
	maps := flag.String("maps", "", "maps directory")
	out := flag.String("o", "", "output Go object file")
	pkg := flag.String("p", "main", "package path")
	flag.Parse()
	if *in == "" || *out == "" || *meta == "" {
		fmt.Fprintln(os.Stderr, "usage: gocobj2goobj -in foo.gocobj -meta foo.meta.json [-maps DIR] -o foo.goobj [-p pkg]")
		os.Exit(2)
	}
	f, err := gocobj.Open(*in)
	if err != nil {
		fmt.Fprintf(os.Stderr, "gocobj2goobj: %v\n", err)
		os.Exit(1)
	}
	if errs := gocobj.Verify(f); len(errs) > 0 {
		for _, e := range errs {
			fmt.Fprintf(os.Stderr, "gocobj2goobj: %s: %v\n", *in, e)
		}
		os.Exit(1)
	}
	goarch := os.Getenv("GOARCH")
	if goarch == "" {
		goarch = "amd64"
	}
	if want := profileFor[goarch]; f.Header.Profile != want {
		fmt.Fprintf(os.Stderr, "gocobj2goobj: %s has ABI profile %q, this converter writes %q for GOARCH=%s\n",
			*in, f.Header.Profile, want, goarch)
		os.Exit(1)
	}
	elfpack.Main([]string{"-elf", *in, "-meta", *meta, "-maps", *maps, "-out-o", *out, "-p", *pkg})
}
