package main

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"

	"github.com/LoyieKing/goc/internal/pipeline"
	"github.com/LoyieKing/goc/internal/toolchain"
)

const goUsage = `usage: goc go [dir] [-o binary] [switches] [-- clang-args...]

dir is a Go package (default: .). Every *.c in that directory is compiled
and linked into the package.

  linux, CGO_ENABLED=0, internal linking. The C may not call libc.
  --arch amd64 (default) or arm64 (GOARCH=arm64; run it under qemu or on
  an arm64 host). Writes goc_marker_<arch>.s if the package has no
  assembly file, so Go accepts bodyless declarations. You still declare
  each C function in Go.
  Defaults: -O3 --default-ptr-color cptr --fast-stack-alloca

Package main produces a binary (default: <dir>/<dirname>). Any other package
is type-checked and packed into the build cache; -o is refused.
--objdir DIR keeps the objects (foo.goobj, foo.gocobj, foo.symabis, foo.ll
for each foo.c) in DIR instead of build/goc-go/ under the goc tree.
`

func cmdGo(args []string) error {
	s := newSettings()
	dir, out, objdir := ".", "", ""
	var extra []string
	for i := 0; i < len(args); {
		n, err := s.consume(args, i)
		if err != nil {
			return err
		}
		if n > 0 {
			i += n
			continue
		}
		switch a := args[i]; {
		case a == "-h" || a == "--help":
			fmt.Print(goUsage)
			fmt.Println()
			fmt.Print(flagsHelp)
			os.Exit(0)
		case a == "-o":
			if i+1 >= len(args) {
				return fmt.Errorf("-o needs a path")
			}
			out = args[i+1]
			i += 2
			continue
		case a == "--objdir" || strings.HasPrefix(a, "--objdir="):
			if v, ok := strings.CutPrefix(a, "--objdir="); ok {
				objdir = v
				i++
				continue
			}
			if i+1 >= len(args) {
				return fmt.Errorf("--objdir needs a directory")
			}
			objdir = args[i+1]
			i += 2
			continue
		case a == "--":
			extra = args[i+1:]
			i = len(args)
			continue
		case strings.HasPrefix(a, "-"):
			return fmt.Errorf("unknown flag: %s (clang arguments go after --)", a)
		default:
			dir = a
		}
		i++
	}
	s.productDefaults()
	s.o.ClangArgs = extra
	return goBuild(s, dir, out, objdir)
}

func goBuild(s *settings, dir, out, objdir string) error {
	st, err := os.Stat(dir)
	if err != nil || !st.IsDir() {
		return fmt.Errorf("not a directory: %s", dir)
	}
	if dir, err = filepath.Abs(dir); err != nil {
		return err
	}
	arch := s.o.Arch
	if arch == "" {
		arch = "amd64"
	}
	goEnv := append(os.Environ(), "CGO_ENABLED=0", "GOFLAGS=", "GOTOOLCHAIN=local",
		"GOPROXY=off", "GOSUMDB=off", "GOOS=linux", "GOARCH="+arch)
	t, err := tools()
	if err != nil {
		return err
	}
	list := func(f string) (string, error) {
		cmd := exec.Command("go", "list", "-f", f, ".")
		cmd.Dir, cmd.Env, cmd.Stderr = dir, goEnv, os.Stderr
		b, err := cmd.Output()
		return strings.TrimSpace(string(b)), err
	}
	name, err := list("{{.Name}}")
	if err != nil {
		return fmt.Errorf("go list: %v", err)
	}
	importPath, err := list("{{.ImportPath}}")
	if err != nil {
		return fmt.Errorf("go list: %v", err)
	}
	if !s.seen["pkg"] {
		s.o.Pkg = importPath
		if name == "main" {
			s.o.Pkg = "main"
		}
	}
	fmt.Fprintf(os.Stderr, "goc go: %s\n", s)

	srcs, _ := filepath.Glob(filepath.Join(dir, "*.c"))
	if len(srcs) == 0 {
		return fmt.Errorf("no .c files in %s", dir)
	}
	sort.Strings(srcs)
	// Go accepts a bodyless declaration only in a package with assembly
	// for the target arch.
	nasm, err := list("{{len .SFiles}}")
	if err != nil {
		return fmt.Errorf("go list: %v", err)
	}
	if nasm == "0" {
		marker := filepath.Join(dir, "goc_marker_"+arch+".s")
		if err := os.WriteFile(marker, []byte(markerAsm), 0o644); err != nil {
			return err
		}
		fmt.Fprintf(os.Stderr, "goc go: wrote %s\n", marker)
	}
	cache := objdir
	if cache == "" {
		sum := sha256.Sum256([]byte(dir))
		cache = filepath.Join(t.Root, "build", "goc-go", hex.EncodeToString(sum[:])[:16])
	}
	if err := os.MkdirAll(cache, 0o755); err != nil {
		return err
	}
	var objs []string
	for _, src := range srcs {
		obj := filepath.Join(cache, strings.TrimSuffix(filepath.Base(src), ".c")+".goobj")
		fmt.Fprintf(os.Stderr, "goc go: compile %s\n", src)
		o := s.o
		o.Src, o.Out = src, obj
		if err := pipeline.Build(t, o); err != nil {
			return err
		}
		objs = append(objs, obj)
	}
	if name == "main" {
		if out == "" {
			out = filepath.Join(dir, filepath.Base(dir))
		} else if out, err = filepath.Abs(out); err != nil {
			return err
		}
		if err := os.MkdirAll(filepath.Dir(out), 0o755); err != nil {
			return err
		}
	} else if out != "" {
		return fmt.Errorf("-o is for package main; %s is package %s", importPath, name)
	}
	hook, err := ToolexecFlag(s.o.Pkg, objs)
	if err != nil {
		return err
	}
	bargs := []string{"build", "-a", "-toolexec", hook}
	if out != "" {
		bargs = append(bargs, "-o", out)
	}
	if err := run(dir, goEnv, "go", append(bargs, ".")...); err != nil {
		return fmt.Errorf("go build: %v", err)
	}
	if name == "main" {
		fmt.Printf("goc go: %s\n", out)
	} else {
		fmt.Printf("goc go: packed %s\n", importPath)
	}
	return nil
}

// ToolexecFlag is the go build -toolexec value that packs objs into pkg.
func ToolexecFlag(pkg string, objs []string) (string, error) {
	exe, err := os.Executable()
	if err != nil {
		return "", err
	}
	words := append([]string{exe, "toolexec", "-pkg", pkg}, objs...)
	for _, w := range words {
		if strings.ContainsAny(w, " \t\n'\"") {
			return "", fmt.Errorf("path %q has a space or quote; go build -toolexec cannot pass it", w)
		}
	}
	return strings.Join(append(words, "--"), " "), nil
}

const markerAsm = `// Code generated by goc go. Do not edit.
// Present so Go accepts bodyless declarations of the C functions.
// The function bodies are the packed goc goobj, not this TEXT.
#include "textflag.h"

TEXT ·gocMarker(SB), NOSPLIT|NOFRAME, $0-0
	RET
`

func cmdCheck(args []string) error {
	s := newSettings()
	for i := 0; i < len(args); {
		n, err := s.consume(args, i)
		if err != nil {
			return err
		}
		if n == 0 {
			if args[i] == "-h" || args[i] == "--help" {
				fmt.Println("usage: goc check [switches]\nBuilds a copy of examples/hello and requires stdout \"hello 42\".")
				fmt.Println()
				fmt.Print(flagsHelp)
				os.Exit(0)
			}
			return fmt.Errorf("takes switches only (got %s)", args[i])
		}
		i += n
	}
	root, err := toolchain.Root()
	if err != nil {
		return err
	}
	tmp, err := os.MkdirTemp("", "goc-check")
	if err != nil {
		return err
	}
	defer os.RemoveAll(tmp)
	for _, f := range []string{"go.mod", "main.go", "add.c"} {
		b, err := os.ReadFile(filepath.Join(root, "examples", "hello", f))
		if err != nil {
			return err
		}
		if err := os.WriteFile(filepath.Join(tmp, f), b, 0o644); err != nil {
			return err
		}
	}
	s.productDefaults()
	bin := filepath.Join(tmp, "hello")
	if err := goBuild(s, tmp, bin, ""); err != nil {
		return err
	}
	got, err := exec.Command(bin).Output()
	if err != nil {
		return fmt.Errorf("examples/hello: %v", err)
	}
	if strings.TrimRight(string(got), "\n") != "hello 42" {
		return fmt.Errorf("examples/hello printed %q, want \"hello 42\"", got)
	}
	fmt.Println("PASS goc check")
	return nil
}
