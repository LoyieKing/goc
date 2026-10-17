// Package convert turns a .gocobj (docs/design/gocobj-format.md) into a Go object
// file.
//
//	goc tool convert -in foo.gocobj -o foo.goobj [-p main] [-symabis foo.symabis]
//
// Main is that command line (goc tool convert).
//
// The conversion is table driven: the machine code, its ELF relocations and
// the .goc.* tables (frame sizes, pc-value tables, GC maps, ABI, nosplit) go
// into the object as they are. The converter decodes no instruction and
// emits none; the backend already put the stack check and the morestack
// block into the code. What it does decide is the Go side of each name
// (package qualification, ABI0 vs ABIInternal, the .impl/.goabi/@plt
// conventions) and how ELF relocations map onto Go relocation types.
//
// The object is written by internal/goobj, driven by the target Go release's
// facts (gometa): the release is `go env GOVERSION` of the go command in
// PATH unless -goversion or -go-meta says otherwise
// (docs/design/go-versions.md).
package convert

import (
	"flag"
	"fmt"
	"os"

	"github.com/LoyieKing/goc/internal/gocobj"
	"github.com/LoyieKing/goc/internal/gometa"
)

// Main runs the converter command line on args (without the program
// name) and returns the exit status.
func Main(args []string) (rc int) {
	defer func() {
		if r := recover(); r != nil {
			if e, ok := r.(exitCode); ok {
				rc = int(e)
				return
			}
			panic(r)
		}
	}()
	flag := flag.NewFlagSet("goc tool convert", flag.ExitOnError)
	in := flag.String("in", "", "input .gocobj")
	out := flag.String("o", "", "output Go object file")
	pkg := flag.String("p", "main", "package path")
	printProfile := flag.Bool("print-profile", false, "print the goc ABI profile for -goarch (or $GOARCH) and exit")
	printVersion := flag.Bool("version", false, "print the Go release objects are written for and exit")
	goarchFlag := flag.String("goarch", "", "target GOARCH (default $GOARCH, else amd64)")
	goversion := flag.String("goversion", "", "target Go version (default: go env GOVERSION)")
	metaFile := flag.String("go-meta", "", "Go release facts (JSON from cmd/goc-gometa) instead of the embedded ones")
	header := flag.String("header", "", "object header line (default: the one `go tool asm` writes)")
	symabis := flag.String("symabis", "", "also write the asm symabis (def lines) of the functions here")
	libcalls := flag.String("libcall-impl", "", "space-separated libcalls whose definition is <name>.impl (default: the core integer and string ones)")
	flag.Parse(args)
	goarch := *goarchFlag
	if goarch == "" {
		goarch = os.Getenv("GOARCH")
	}
	if goarch == "" {
		goarch = "amd64"
	}
	meta, ver, err := resolve(*metaFile, *goversion)
	if err != nil {
		fatalf("%v", err)
	}
	if *printVersion {
		fmt.Println(ver)
		return 0
	}
	want, err := profileFor(meta, goarch)
	if err != nil {
		fatalf("%v", err)
	}
	if *printProfile {
		fmt.Println(want.Name)
		return 0
	}
	if *in == "" || *out == "" {
		fmt.Fprintln(os.Stderr, "usage: goc tool convert -in foo.gocobj -o foo.goobj [-p pkg] [-symabis file]")
		return 2
	}
	f, err := gocobj.Open(*in)
	if err != nil {
		fatalf("%v", err)
	}
	if errs := gocobj.Verify(f); len(errs) > 0 {
		for _, e := range errs {
			fmt.Fprintf(os.Stderr, "goc tool convert: %s: %v\n", *in, e)
		}
		return 1
	}
	// The object's profile must state the facts of this Go's ABI; the name
	// may differ (go1.24-amd64 is goabi1-amd64's old name).
	if have, ok := gocobj.LookupProfile(f.Header.Profile); !ok || !gocobj.SameFacts(have, want) {
		fatalf("%s has ABI profile %q; %s on GOARCH=%s needs %q (or one with the same facts)",
			*in, f.Header.Profile, ver, goarch, want.Name)
	}
	if f.Header.Flags&gocobj.HeaderStackChecksInCode == 0 {
		fatalf("%s: the backend did not emit the stack checks (header flag stack_checks_in_code is 0)", *in)
	}
	hdr := *header
	if hdr == "" {
		if hdr, err = objectHeader(goarch, ver); err != nil {
			fatalf("%v", err)
		}
	} else if hdr[len(hdr)-1] != '\n' {
		hdr += "\n"
	}
	c, err := newConverter(*in, f, *pkg, goarch, *libcalls)
	if err != nil {
		fatalf("%v", err)
	}
	if err := c.write(*out, meta, hdr); err != nil {
		fatalf("%v", err)
	}
	if *symabis != "" {
		if err := os.WriteFile(*symabis, []byte(c.symabis()), 0o644); err != nil {
			fatalf("%v", err)
		}
	}
	return 0
}

// resolve returns the facts of the target Go release and its version:
// metaFile when given, else the embedded facts of ver (default: go env
// GOVERSION of the go command in PATH).
func resolve(metaFile, ver string) (*gometa.Meta, string, error) {
	var meta *gometa.Meta
	var err error
	if metaFile != "" {
		meta, err = gometa.Load(metaFile)
		if err == nil && ver == "" {
			ver = meta.SourceVersion
			if v, e := goEnv("GOVERSION"); e == nil && sameMinor(v, meta.Go) {
				ver = v
			}
		}
	} else {
		if ver == "" {
			ver, err = goEnv("GOVERSION")
		}
		if err == nil {
			meta, err = gometa.ForVersion(ver)
		}
	}
	return meta, ver, err
}

// Profile names the goc ABI profile of the go command in PATH for goarch
// (what -print-profile prints).
func Profile(goarch string) (string, error) {
	meta, _, err := resolve("", "")
	if err != nil {
		return "", err
	}
	p, err := profileFor(meta, goarch)
	return p.Name, err
}

func sameMinor(v, minor string) bool {
	m, ok := gometa.Minor(v)
	return ok && m == minor
}

type exitCode int

func fatalf(format string, args ...any) {
	fmt.Fprintf(os.Stderr, "goc tool convert: "+format+"\n", args...)
	panic(exitCode(1))
}
