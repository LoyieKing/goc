// gocobj2goobj converts a .gocobj (docs/gocobj-format.md) into a Go object
// file.
//
//	gocobj2goobj -in foo.gocobj -o foo.goobj [-p main] [-symabis foo.symabis]
//
// The conversion is table driven: the machine code, its ELF relocations and
// the .goc.* tables (frame sizes, pc-value tables, GC maps, ABI, nosplit) go
// into the object as they are. The converter decodes no instruction and
// emits none; the backend already put the stack check and the morestack
// block into the code. What it does decide is the Go side of each name
// (package qualification, ABI0 vs ABIInternal, the .impl/.goabi/@plt
// conventions) and how ELF relocations map onto Go relocation types.
package main

import (
	"flag"
	"fmt"
	"os"

	"github.com/LoyieKing/goc/gocobj"
)

func main() {
	in := flag.String("in", "", "input .gocobj")
	out := flag.String("o", "", "output Go object file")
	pkg := flag.String("p", "main", "package path")
	goversion := flag.String("goversion", os.Getenv("GOC_GOVERSION"), "Go release the object is for (default: the Go this converter was built with)")
	symabis := flag.String("symabis", "", "also write the asm symabis (def lines) of the functions here")
	flag.Parse()
	if *in == "" || *out == "" {
		fmt.Fprintln(os.Stderr, "usage: gocobj2goobj -in foo.gocobj -o foo.goobj [-p pkg] [-symabis file]")
		os.Exit(2)
	}
	v, err := lookupGoVersion(*goversion)
	if err != nil {
		fatalf("%v", err)
	}
	gv = v
	f, err := gocobj.Open(*in)
	if err != nil {
		fatalf("%v", err)
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
	if want := gv.Profiles[goarch]; f.Header.Profile != want {
		fatalf("%s has ABI profile %q, this converter writes %q for GOARCH=%s", *in, f.Header.Profile, want, goarch)
	}
	if f.Header.Flags&gocobj.HeaderStackChecksInCode == 0 {
		fatalf("%s: the backend did not emit the stack checks (header flag stack_checks_in_code is 0)", *in)
	}
	c, err := newConverter(*in, f, *pkg, goarch)
	if err != nil {
		fatalf("%v", err)
	}
	if err := c.write(*out); err != nil {
		fatalf("%v", err)
	}
	if *symabis != "" {
		if err := os.WriteFile(*symabis, []byte(c.symabis()), 0o644); err != nil {
			fatalf("%v", err)
		}
	}
}

func fatalf(format string, args ...any) {
	fmt.Fprintf(os.Stderr, "gocobj2goobj: "+format+"\n", args...)
	os.Exit(1)
}
