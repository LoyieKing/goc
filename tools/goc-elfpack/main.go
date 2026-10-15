// goc-elfpack packs an llc-produced ELF object into a Go object file.
// The logic is in goobj/elfpack; gocobj2goobj is its successor.
package main

import (
	"os"

	"github.com/LoyieKing/goc/goobj/elfpack"
)

func main() { elfpack.Main(os.Args[1:]) }
