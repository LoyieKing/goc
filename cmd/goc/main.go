// Command goc compiles pointer-colored C into Go objects and links them
// into Go programs (docs/quickstart.md).
//
//	goc go [dir] [-o bin] [switches] [-- clang-args]
//	goc build foo.c [-o foo.goobj] [switches] [-- clang-args]
//	goc check [switches]
//	goc cc [clang-args]
//	goc env
//	goc toolexec [-pkg P] OBJ... -- TOOL ARGS   (the go build -toolexec hook)
//	goc tool convert|objdump ...
//	goc version
package main

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"

	"github.com/LoyieKing/goc/internal/convert"
	"github.com/LoyieKing/goc/internal/pipeline"
	"github.com/LoyieKing/goc/internal/toolchain"
)

const usage = `goc: pointer-colored C on Go stacks

Usage:
  goc go [dir] [-o bin] [switches] [-- clang-args]
      Compile every .c of a Go package and build it. Default switches:
      -O3 --default-ptr-color cptr --fast-stack-alloca
  goc build foo.c [-o foo.goobj] [switches] [-- clang-args]
      One C file to a Go object (and foo.gocobj, foo.symabis, foo.ll
      beside it). Default -O0. Unknown -flags go to clang.
  goc check [switches]
      Self-check: build and run examples/hello
  goc cc [clang-args]
      The shipped clang with goc's include directory
  goc env
      The tools this goc runs
  goc toolexec [-pkg P] OBJ... -- TOOL ARGS
      go build -toolexec hook that packs Go objects into package P
  goc tool convert ARGS     .gocobj -> Go object (goc tool convert -h)
  goc tool objdump dump|verify FILE... | diff [-v] A.goobj B.goobj
  goc version

--fast-stack-alloca is much faster and single-threaded only: the alloca
pool cursor is process-global. Morestack, splittable frames and sptr maps
are always on (amd64 and arm64).

Download and use: docs/quickstart.md
Build from source: docs/build-from-source.md
`

func main() {
	if len(os.Args) < 2 {
		fmt.Fprint(os.Stderr, usage)
		os.Exit(2)
	}
	cmd, args := os.Args[1], os.Args[2:]
	var err error
	switch cmd {
	case "go":
		err = cmdGo(args)
	case "build":
		err = cmdBuild(args)
	case "check":
		err = cmdCheck(args)
	case "cc":
		err = cmdCC(args)
	case "env":
		err = cmdEnv(args)
	case "toolexec":
		os.Exit(cmdToolexec(args))
	case "tool":
		os.Exit(cmdTool(args))
	case "version":
		fmt.Println("goc " + version())
	case "help", "-h", "--help":
		fmt.Print(usage)
		fmt.Println()
		fmt.Print(flagsHelp)
	default:
		fmt.Fprintf(os.Stderr, "goc: unknown command %s\n\n%s", cmd, usage)
		os.Exit(2)
	}
	if err != nil {
		fmt.Fprintf(os.Stderr, "goc %s: %v\n", cmd, err)
		os.Exit(1)
	}
}

func version() string {
	root, err := toolchain.Root()
	if err != nil {
		return "unknown"
	}
	b, err := os.ReadFile(filepath.Join(root, "VERSION"))
	if err != nil {
		return "unknown"
	}
	return strings.TrimSpace(string(b))
}

func tools() (*toolchain.Tools, error) {
	root, err := toolchain.Root()
	if err != nil {
		return nil, err
	}
	return toolchain.Require(root)
}

func cmdBuild(args []string) error {
	if len(args) == 0 {
		fmt.Fprintln(os.Stderr, "usage: goc build foo.c [-o foo.goobj] [switches] [-- clang-args]")
		os.Exit(2)
	}
	s := newSettings()
	for i := 0; i < len(args); {
		n, err := s.consume(args, i)
		if err != nil {
			return err
		}
		if n > 0 {
			i += n
			continue
		}
		a := args[i]
		switch {
		case a == "-o":
			if i+1 >= len(args) {
				return fmt.Errorf("-o needs a path")
			}
			s.o.Out = args[i+1]
			i += 2
			continue
		case a == "--":
			s.o.ClangArgs = append(s.o.ClangArgs, args[i+1:]...)
			i = len(args)
			continue
		case a == "-h" || a == "--help":
			fmt.Println("usage: goc build foo.c [-o foo.goobj] [switches] [-- clang-args]")
			fmt.Println()
			fmt.Print(flagsHelp)
			os.Exit(0)
		case strings.HasPrefix(a, "-"):
			s.o.ClangArgs = append(s.o.ClangArgs, a)
		case s.o.Src == "":
			s.o.Src = a
		default:
			s.o.ClangArgs = append(s.o.ClangArgs, a)
		}
		i++
	}
	if s.o.Src == "" {
		fmt.Fprintln(os.Stderr, "goc build: missing input file")
		os.Exit(2)
	}
	t, err := tools()
	if err != nil {
		return err
	}
	return pipeline.Build(t, s.o)
}

func cmdCC(args []string) error {
	root, err := toolchain.Root()
	if err != nil {
		return err
	}
	t, err := toolchain.Locate(root)
	if err != nil {
		return err
	}
	argv := append([]string{t.Clang, "-I", t.Include}, args...)
	return syscall.Exec(t.Clang, argv, t.Env(os.Environ()))
}

func cmdEnv(args []string) error {
	if len(args) != 0 {
		return fmt.Errorf("takes no arguments")
	}
	t, err := tools()
	if err != nil {
		return err
	}
	fmt.Printf("root: %s\nclang: %s\nlibdir: %s\nopt: %s\nllvm-mc: %s\ngoc-llc: %s\npasses: %s\ninclude: %s\n",
		t.Root, t.Clang, t.LibDir, t.Opt, t.MC, t.LLC, t.Passes, t.Include)
	s := newSettings()
	s.productDefaults()
	fmt.Printf("goc go: %s\n", s)
	return nil
}

func cmdTool(args []string) int {
	if len(args) == 0 {
		fmt.Fprintln(os.Stderr, "usage: goc tool convert|objdump ...")
		return 2
	}
	switch args[0] {
	case "convert":
		return convert.Main(args[1:])
	case "objdump":
		return objdump(args[1:])
	}
	fmt.Fprintf(os.Stderr, "goc tool: unknown tool %s (convert, objdump)\n", args[0])
	return 2
}

// run runs a command with the given environment and stdio.
func run(dir string, env []string, name string, args ...string) error {
	cmd := exec.Command(name, args...)
	cmd.Dir = dir
	cmd.Env = env
	cmd.Stdin, cmd.Stdout, cmd.Stderr = os.Stdin, os.Stdout, os.Stderr
	return cmd.Run()
}
