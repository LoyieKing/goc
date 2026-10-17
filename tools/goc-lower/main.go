// Command goc-lower is the real-body helper shipped next to gocobj2goobj.
// The driver calls it instead of python3 and ripgrep.
package main

import (
	"fmt"
	"io"
	"os"

	"github.com/LoyieKing/goc/goobj/lower"
)

func main() {
	if len(os.Args) < 2 {
		usage()
		os.Exit(2)
	}
	arch := os.Getenv("GOC_ARCH")
	if arch == "" {
		arch = "amd64"
	}
	var err error
	switch os.Args[1] {
	case "goabi":
		if len(os.Args) != 6 && len(os.Args) != 7 {
			usage()
			os.Exit(2)
		}
		tables := ""
		if len(os.Args) == 7 {
			tables = os.Args[6]
		}
		err = lower.WriteGoABI(os.Args[2], os.Args[3], os.Args[4], os.Args[5], tables, arch)
	case "fix-ir":
		err = lower.FixIR(need(2), arch)
	case "rename-ir":
		if len(os.Args) != 4 {
			usage()
			os.Exit(2)
		}
		err = lower.RenameIR(os.Args[2], os.Args[3])
	case "align-asm":
		err = lower.AlignASM(need(2))
	case "fnname":
		name, e := lower.FirstFnName(need(2))
		err = e
		if err == nil && name != "" {
			fmt.Println(name)
		}
	case "magic-obj":
		err = lower.ObjectHasMagic(need(2))
	default:
		usage()
		os.Exit(2)
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

func need(i int) string {
	if len(os.Args) <= i {
		usage()
		os.Exit(2)
	}
	return os.Args[i]
}

func usage() {
	io.WriteString(os.Stderr, `goc-lower: real-body helper
  goabi <in.ll> <out.ll> <thunks.s> <goabi.json> [thunks.goc.s]
  fix-ir <file.ll>
  rename-ir <file.ll> <old:new pairs>
  align-asm <file.s>
  fnname <ll>
  magic-obj <goobj>
`)
}
