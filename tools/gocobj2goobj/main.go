// gocobj2goobj converts a .gocobj into a Go object file (internal/convert).
//
//	gocobj2goobj -in foo.gocobj -o foo.goobj [-p main] [-symabis foo.symabis]
package main

import (
	"os"

	"github.com/LoyieKing/goc/internal/convert"
)

func main() { os.Exit(convert.Main(os.Args[1:])) }
