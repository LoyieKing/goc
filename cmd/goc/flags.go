package main

import (
	"fmt"
	"os"
	"strings"

	"github.com/LoyieKing/goc/internal/pipeline"
)

// settings are the compile switches every compiling command takes. Only
// the command line sets them; goc reads no GOC_* environment variable.
type settings struct {
	o    pipeline.Options
	seen map[string]bool
}

func newSettings() *settings { return &settings{seen: map[string]bool{}} }

// consume parses args[i] (and args[i+1] for a separate value) if it is a
// compile switch. It returns how many arguments it used; 0 means args[i]
// is not one.
func (s *settings) consume(args []string, i int) (int, error) {
	a := args[i]
	next := func() (string, error) {
		if i+1 >= len(args) || args[i+1] == "" {
			return "", fmt.Errorf("%s needs a value", a)
		}
		return args[i+1], nil
	}
	// value handles --name VALUE and --name=VALUE.
	value := func(name string) (string, int, bool, error) {
		if a == name {
			v, err := next()
			return v, 2, true, err
		}
		if v, ok := strings.CutPrefix(a, name+"="); ok {
			return v, 1, true, nil
		}
		return "", 0, false, nil
	}
	switch a {
	case "-O0", "-O1", "-O2", "-O3":
		s.o.OptLevel = int(a[2] - '0')
		s.seen["opt"] = true
		return 1, nil
	case "--no-default-ptr-color":
		s.o.DefaultPtrColor = ""
		s.seen["default-ptr-color"] = true
		return 1, nil
	case "--fast-stack-alloca", "--no-fast-stack-alloca":
		s.o.FastStackAlloca = a == "--fast-stack-alloca"
		s.seen["fast-stack-alloca"] = true
		return 1, nil
	case "--color-report", "--no-color-report":
		s.o.ColorReport = a == "--color-report"
		return 1, nil
	case "--abi0":
		s.o.ABI0 = true
		return 1, nil
	case "--keep-tmp":
		d, err := os.MkdirTemp("", "goc-keep")
		if err != nil {
			return 0, err
		}
		s.o.KeepTmp = d
		fmt.Fprintf(os.Stderr, "goc: keeping temps in %s\n", d)
		return 1, nil
	}
	for _, name := range []string{"--default-ptr-color", "--arch", "--keep-tmp", "--opt-extra",
		"--llc-extra", "--pkg", "--ir-renames", "--libcall-impl"} {
		v, n, ok, err := value(name)
		if !ok {
			continue
		}
		if err != nil {
			return 0, err
		}
		switch name {
		case "--default-ptr-color":
			if v == "" {
				return 0, fmt.Errorf("--default-ptr-color needs a value")
			}
			s.o.DefaultPtrColor = v
			s.seen["default-ptr-color"] = true
		case "--arch":
			if v != "amd64" && v != "arm64" {
				return 0, fmt.Errorf("--arch wants amd64 or arm64")
			}
			s.o.Arch = v
		case "--keep-tmp":
			if v == "" {
				return 0, fmt.Errorf("--keep-tmp needs a directory")
			}
			s.o.KeepTmp = v
		case "--opt-extra":
			s.o.OptExtra = strings.Fields(v)
		case "--llc-extra":
			s.o.LLCExtra = strings.Fields(v)
		case "--pkg":
			if v == "" {
				return 0, fmt.Errorf("--pkg needs a package path")
			}
			s.o.Pkg = v
			s.seen["pkg"] = true
		case "--ir-renames":
			s.o.IRRenames = v
		case "--libcall-impl":
			s.o.LibcallImpl = v
		}
		return n, nil
	}
	return 0, nil
}

// productDefaults fills what the user did not pass with the QuickJS-
// validated set `goc go` and `goc check` start from.
func (s *settings) productDefaults() {
	if !s.seen["opt"] {
		s.o.OptLevel = 3
	}
	if !s.seen["default-ptr-color"] {
		s.o.DefaultPtrColor = "cptr"
	}
	if !s.seen["fast-stack-alloca"] {
		s.o.FastStackAlloca = true
	}
}

// String is the resolved switches, as `goc go` prints them.
func (s *settings) String() string {
	o := s.o
	f := []string{fmt.Sprintf("-O%d", o.OptLevel)}
	if o.DefaultPtrColor != "" {
		f = append(f, "--default-ptr-color", o.DefaultPtrColor)
	} else if s.seen["default-ptr-color"] {
		f = append(f, "--no-default-ptr-color")
	}
	if o.FastStackAlloca {
		f = append(f, "--fast-stack-alloca")
	} else {
		f = append(f, "--no-fast-stack-alloca")
	}
	if o.ColorReport {
		f = append(f, "--color-report")
	}
	if o.Arch != "" {
		f = append(f, "--arch", o.Arch)
	}
	if len(o.OptExtra) > 0 {
		f = append(f, "--opt-extra", strings.Join(o.OptExtra, " "))
	}
	if len(o.LLCExtra) > 0 {
		f = append(f, "--llc-extra", strings.Join(o.LLCExtra, " "))
	}
	if o.IRRenames != "" {
		f = append(f, "--ir-renames", o.IRRenames)
	}
	if o.LibcallImpl != "" {
		f = append(f, "--libcall-impl", o.LibcallImpl)
	}
	if o.KeepTmp != "" {
		f = append(f, "--keep-tmp="+o.KeepTmp)
	}
	if o.Pkg != "" {
		f = append(f, "--pkg", o.Pkg)
	}
	return strings.Join(f, " ")
}

const flagsHelp = `Compile switches (goc reads no GOC_* environment variable):
  -O0|-O1|-O2|-O3        LLVM IR optimization level
  --default-ptr-color COLOR
                         Color of an unannotated T* (cptr, sptr, uptr, gptr)
  --no-default-ptr-color Leave that color unset; each pointer is inferred
  --fast-stack-alloca    alloca bumps a 16 MiB pool per object, inline.
                         Much faster. The pool cursor is a plain global,
                         so this is single-goroutine only. Concurrent
                         calls are not thread-safe.
  --no-fast-stack-alloca alloca is one malloc/free per call
  --color-report         Print the pointer-color table; does not change colors
  --no-color-report
  --arch amd64|arm64     Target architecture (default amd64)
  --keep-tmp             Keep the work directory
  --keep-tmp=DIR
  --opt-extra STRING     Extra flags for opt
  --llc-extra STRING     Extra flags for goc-llc
  --pkg PATH             Go symbol prefix (default: main, or the import path)
  --ir-renames SPEC      Space-separated old:new IR renames
  --libcall-impl SPEC    Libcalls whose definition is <name>.impl
  --abi0                 Mark the object ABI0 (C-side platform entries, e.g. a libc shim)

The tools are the ones of this tree: bin/clang, bin/opt, bin/llvm-mc and
passes/ in a release; third_party/llvm-19.1.7-clang-build and build/llvm in
a checkout (goc env prints them).
Clang arguments go after --.
`
