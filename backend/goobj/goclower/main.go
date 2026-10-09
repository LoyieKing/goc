// Command goc-lower is the real-body helper shipped next to elfpack.
// The driver calls it instead of python3 and ripgrep.
package main

import (
	"fmt"
	"io"
	"os"

	"goc.local/p5-machinepass-goobj/goobj/lower"
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
		if len(os.Args) != 6 {
			usage()
			os.Exit(2)
		}
		err = lower.WriteGoABI(os.Args[2], os.Args[3], os.Args[4], os.Args[5], arch)
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
	case "meta":
		if len(os.Args) != 10 {
			usage()
			os.Exit(2)
		}
		err = lower.WriteMeta(os.Args[2], os.Args[3], os.Args[4], os.Args[5], os.Args[6], os.Args[7], os.Args[8], os.Args[9], arch, os.Getenv("GOC_NO_NOSPLIT") == "1")
	case "frame":
		if len(os.Args) != 4 {
			usage()
			os.Exit(2)
		}
		fmt.Println(lower.FrameOf(mustRead(os.Args[2]), os.Args[3], arch))
	case "fnname":
		name, e := lower.FirstFnName(need(2))
		err = e
		if err == nil && name != "" {
			fmt.Println(name)
		}
	case "symabis":
		err = lower.SymABIs(need(2))
	case "magic-obj":
		err = lower.ObjectHasMagic(need(2))
	case "check-meta":
		err = lower.CheckMeta(need(2))
	case "has-define":
		if len(os.Args) != 4 {
			usage()
			os.Exit(2)
		}
		ok, e := lower.HasDefine(os.Args[2], os.Args[3])
		if e != nil {
			err = e
			break
		}
		if !ok {
			os.Exit(1)
		}
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

func mustRead(path string) string {
	b, err := os.ReadFile(path)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	return string(b)
}

func usage() {
	io.WriteString(os.Stderr, `goc-lower: real-body helper
  goabi <in.ll> <out.ll> <thunks.s> <goabi.json>
  fix-ir <file.ll>
  rename-ir <file.ll> <old:new pairs>
  align-asm <file.s>
  meta <ll> <asm> <dis> <out.json> <sym-prefix> <goabi.json> <abi0 0|1> <mir>
  frame <asm> <function>
  fnname <ll>
  symabis <meta.json>
  magic-obj <goobj>
  check-meta <meta.json>
  has-define <ll> <function>
`)
}
