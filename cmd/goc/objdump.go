package main

import (
	"flag"
	"fmt"
	"os"

	"github.com/LoyieKing/goc/internal/gocobj"
	"github.com/LoyieKing/goc/internal/goobj"
)

func objdumpUsage() int {
	fmt.Fprintln(os.Stderr, "usage: goc tool objdump dump|verify FILE...\n       goc tool objdump diff [-v] A.goobj B.goobj")
	return 2
}

// objdump prints and checks .gocobj files (docs/design/gocobj-format.md)
// and compares Go object files. verify exits 1 if any file breaks an
// invariant; diff exits 0 for no difference, 1 for a difference, 2 for an
// error.
func objdump(args []string) int {
	if len(args) < 1 {
		return objdumpUsage()
	}
	switch args[0] {
	case "diff":
		return objdiff(args[1:])
	case "dump", "verify":
	default:
		return objdumpUsage()
	}
	if len(args) < 2 {
		return objdumpUsage()
	}
	rc := 0
	for _, path := range args[1:] {
		f, err := gocobj.Open(path)
		if err != nil {
			fmt.Fprintf(os.Stderr, "%s: %v\n", path, err)
			rc = 1
			continue
		}
		if args[0] == "dump" {
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
	return rc
}

func objdiff(args []string) int {
	fs := flag.NewFlagSet("diff", flag.ExitOnError)
	verbose := fs.Bool("v", false, "print every difference, not just the first 20")
	fs.Parse(args)
	if fs.NArg() != 2 {
		return objdumpUsage()
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
		fmt.Printf("goc tool objdump diff: %d differences\n", len(diffs))
		return 1
	}
	fmt.Printf("goc tool objdump diff: no semantic difference (%d symbols)\n", n)
	return 0
}
