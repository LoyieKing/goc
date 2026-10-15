// goc-objdump prints and checks .gocobj files (docs/gocobj-format.md).
//
//	goc-objdump dump FILE...
//	goc-objdump verify FILE...
//
// verify exits 1 if any file breaks an invariant.
package main

import (
	"fmt"
	"os"

	"github.com/LoyieKing/goc/gocobj"
)

func main() {
	if len(os.Args) < 3 || (os.Args[1] != "dump" && os.Args[1] != "verify") {
		fmt.Fprintln(os.Stderr, "usage: goc-objdump dump|verify FILE...")
		os.Exit(2)
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
