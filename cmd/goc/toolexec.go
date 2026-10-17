package main

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"

	"github.com/LoyieKing/goc/internal/pipeline"
)

// cmdToolexec is the go build -toolexec hook:
//
//	go build -a -toolexec "goc toolexec [-pkg P] a.goobj b.goobj --" ...
//
// goc already wrote the Go objects; this makes Go's asm and compile steps
// take them for package P (default main):
//
//  1. after asm -gensymabis: append the def lines of their TEXT symbols
//     (foo.goobj -> foo.symabis), since asm only scanned .s files;
//  2. after compile: pack each object into the package archive. The Go
//     linker loads an archive member only when its name ends in .o or
//     .syso, so foo.goobj is packed as foo.o.
//
// Any other tool, and any other package, runs unchanged.
func cmdToolexec(args []string) int {
	pkg := "main"
	var objs []string
	i := 0
	for ; i < len(args) && args[i] != "--"; i++ {
		if args[i] == "-pkg" && i+1 < len(args) {
			pkg = args[i+1]
			i++
			continue
		}
		objs = append(objs, args[i])
	}
	if i+1 >= len(args) {
		fmt.Fprintln(os.Stderr, "usage: goc toolexec [-pkg P] OBJ... -- TOOL ARGS")
		return 2
	}
	tool, targs := args[i+1], args[i+2:]
	base := strings.TrimSuffix(filepath.Base(tool), ".exe")
	if (base != "asm" && base != "compile") || argAfter(targs, "-p") != pkg {
		if err := syscall.Exec(tool, append([]string{tool}, targs...), os.Environ()); err != nil {
			fmt.Fprintf(os.Stderr, "goc toolexec: %v\n", err)
		}
		return 1
	}
	cmd := exec.Command(tool, targs...)
	cmd.Stdin, cmd.Stdout, cmd.Stderr = os.Stdin, os.Stdout, os.Stderr
	if err := cmd.Run(); err != nil {
		if ee, ok := err.(*exec.ExitError); ok {
			return ee.ExitCode()
		}
		fmt.Fprintf(os.Stderr, "goc toolexec: %v\n", err)
		return 1
	}
	var err error
	switch {
	case base == "asm" && hasArg(targs, "-gensymabis"):
		err = appendSymabis(argAfter(targs, "-o"), objs)
	case base == "compile":
		err = packObjects(tool, argAfter(targs, "-o"), objs)
	}
	if err != nil {
		fmt.Fprintf(os.Stderr, "goc toolexec: %v\n", err)
		return 1
	}
	return 0
}

func argAfter(args []string, flag string) string {
	for i := 0; i+1 < len(args); i++ {
		if args[i] == flag {
			return args[i+1]
		}
	}
	return ""
}

func hasArg(args []string, a string) bool {
	for _, x := range args {
		if x == a {
			return true
		}
	}
	return false
}

func appendSymabis(out string, objs []string) error {
	if out == "" {
		return nil
	}
	if _, err := os.Stat(out); err != nil {
		return nil
	}
	f, err := os.OpenFile(out, os.O_APPEND|os.O_WRONLY, 0)
	if err != nil {
		return err
	}
	defer f.Close()
	for _, obj := range objs {
		sa := pipeline.Side(obj) + ".symabis"
		b, err := os.ReadFile(sa)
		if err != nil {
			continue
		}
		if _, err := f.Write(b); err != nil {
			return err
		}
		fmt.Fprintf(os.Stderr, "toolexec: appended goc symabis from %s\n", filepath.Base(sa))
	}
	return nil
}

func packObjects(tool, archive string, objs []string) error {
	if len(objs) == 0 || !strings.HasSuffix(archive, ".a") {
		return nil
	}
	if _, err := os.Stat(archive); err != nil {
		return nil
	}
	// go1.25+ distributions no longer ship pack prebuilt; `go tool pack`
	// builds it on first use.
	pack := []string{filepath.Join(filepath.Dir(tool), "pack")}
	env := os.Environ()
	if st, err := os.Stat(pack[0]); err != nil || st.Mode()&0o111 == 0 {
		pack = []string{"go", "tool", "pack"}
		env = append(env, "GOFLAGS=", "GOTOOLCHAIN=local")
	}
	stage, err := os.MkdirTemp("", "goc-pack")
	if err != nil {
		return err
	}
	defer os.RemoveAll(stage)
	for _, obj := range objs {
		b, err := os.ReadFile(obj)
		if err != nil {
			continue
		}
		name := filepath.Base(obj)
		if strings.HasSuffix(name, ".goobj") {
			name = strings.TrimSuffix(name, ".goobj") + ".o"
		}
		member := filepath.Join(stage, name)
		if err := os.WriteFile(member, b, 0o644); err != nil {
			return err
		}
		cmd := exec.Command(pack[0], append(pack[1:], "r", archive, member)...)
		cmd.Env = env
		cmd.Stdout, cmd.Stderr = os.Stderr, os.Stderr
		if err := cmd.Run(); err != nil {
			return fmt.Errorf("pack %s: %v", archive, err)
		}
		fmt.Fprintf(os.Stderr, "toolexec: packed %s into %s\n", filepath.Base(obj), archive)
	}
	return nil
}
