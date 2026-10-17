// goc-objdump prints and checks .gocobj files (docs/gocobj-format.md) and
// compares Go object files.
//
//	goc-objdump dump FILE...
//	goc-objdump verify FILE...
//	goc-objdump diff [-v] A.goobj B.goobj
//
// verify exits 1 if any file breaks an invariant. diff compares two Go
// objects semantically (symbols, relocations, decoded pc-value tables,
// funcdata, FuncInfo) and exits 0 for no difference, 1 for a difference,
// 2 for an error.
package main

import (
	"flag"
	"fmt"
	"os"

	"github.com/LoyieKing/goc/internal/gocobj"
	"github.com/LoyieKing/goc/internal/goobj"
)

func usage() {
	fmt.Fprintln(os.Stderr, "usage: goc-objdump dump|verify FILE...\n       goc-objdump diff [-v] A.goobj B.goobj")
	os.Exit(2)
}

func main() {
	if len(os.Args) < 2 {
		usage()
	}
	switch os.Args[1] {
	case "diff":
		os.Exit(diff(os.Args[2:]))
	case "dump", "verify":
	default:
		usage()
	}
	if len(os.Args) < 3 {
		usage()
	}
	rc := 0
	for _, path := range os.Args[2:] {
		f, err := gocobj.Open(path)
		if err != nil {
			fmt.Fprintf(os.Stderr, "%s: %v\n", path, err)
			rc = 1
			continue
		}
		if os.Args[1] == "dump" {
			gocobj.Dump(os.Stdout, f)
			continue
		}
		errs := gocobj.Verify(f)
		for _, e := range errs {
			fmt.Fprintf(os.Stderr, "%s: %v\n", path, e)
		}
		if len(errs) > 0 {
			rc = 1
		} else {
			fmt.Printf("%s: ok\n", path)
		}
	}
	os.Exit(rc)
}

func diff(args []string) int {
	fs := flag.NewFlagSet("diff", flag.ExitOnError)
	verbose := fs.Bool("v", false, "print every difference, not just the first 20")
	fs.Parse(args)
	if fs.NArg() != 2 {
		usage()
	}
	n, diffs, err := goobj.Diff(fs.Arg(0), fs.Arg(1))
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 2
	}
	const limit = 20
	for i, d := range diffs {
		if i == limit && !*verbose {
			fmt.Printf("... %d more\n", len(diffs)-limit)
			break
		}
		fmt.Println(d)
	}
	if len(diffs) > 0 {
		fmt.Printf("goc-objdump diff: %d differences\n", len(diffs))
		return 1
	}
	fmt.Printf("goc-objdump diff: no semantic difference (%d symbols)\n", n)
	return 0
}
