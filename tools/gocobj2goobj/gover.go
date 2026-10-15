package main

// The Go-version dependent part of the conversion. Everything a new Go
// release may renumber or rename lives in one gover_go1_XX.go file that
// registers a goVersion; the rest of the converter only looks things up
// here. Upgrading Go: re-vendor goobj/enc (goobj/enc/vendorize.sh), copy the
// newest gover file to the new version, check each entry against that Go's
// cmd/internal/objabi, internal/abi and runtime, and add the matching ABI
// profile (llvm/include/Goc/AbiProfiles.def, gocobj/abi.go).

import (
	"debug/elf"
	"fmt"
	"runtime"
	"sort"
	"strings"

	"github.com/LoyieKing/goc/goobj/enc/cmd/objlib/objabi"
)

// sizedReloc is a Go relocation type and the width of the field it patches.
type sizedReloc struct {
	Type objabi.RelocType
	Siz  uint8
}

type goVersion struct {
	Name string // "go1.24": matches go1.24, go1.24.N, go1.24rcN

	// Profiles is the gocobj ABI profile the object must carry, per GOARCH.
	Profiles map[string]string

	// amd64 text: a PC-relative field becomes X86Text[type]; one that the
	// .goc call list marks as a call becomes X86Call instead.
	X86Text map[elf.R_X86_64]objabi.RelocType
	X86Call objabi.RelocType
	X86Data map[elf.R_X86_64]sizedReloc

	// arm64 text.
	A64Call     objabi.RelocType                   // BL/B (CALL26, JUMP26)
	A64Page     objabi.RelocType                   // a lone ADRP, or a lone ADD low-12
	A64PagePair map[elf.R_AARCH64]objabi.RelocType // ADRP + adjacent low-12, by the low-12 type
	A64Lo12     map[elf.R_AARCH64]objabi.RelocType // a low-12 load/store without its ADRP
	A64GOTPair  objabi.RelocType                   // ADRP GOT page + LD64_GOT_LO12
	A64Data     map[elf.R_AARCH64]sizedReloc       // ABS64/ABS32, text and data

	// The funcdata/pcdata indices the stack maps go under.
	FuncdataArgs, FuncdataLocals, PcdataStackMapIndex int64
}

var goVersions []*goVersion

func registerGoVersion(v *goVersion) { goVersions = append(goVersions, v) }

func (v *goVersion) matches(goversion string) bool {
	rest, ok := strings.CutPrefix(goversion, v.Name)
	return ok && (rest == "" || rest[0] == '.' || strings.HasPrefix(rest, "rc") || strings.HasPrefix(rest, "beta") || rest[0] == ' ' || rest[0] == '-')
}

// lookupGoVersion picks the mapping for goversion; empty means the Go this
// converter was built with, which is the toolchain the driver links with.
func lookupGoVersion(goversion string) (*goVersion, error) {
	if goversion == "" {
		goversion = runtime.Version()
	}
	for _, v := range goVersions {
		if v.matches(goversion) {
			return v, nil
		}
	}
	var names []string
	for _, v := range goVersions {
		names = append(names, v.Name)
	}
	sort.Strings(names)
	return nil, fmt.Errorf("no Go object mapping for %s (have %s); add tools/gocobj2goobj/gover_%s.go",
		goversion, strings.Join(names, ", "), strings.ReplaceAll(majorMinor(goversion), ".", "_"))
}

func majorMinor(goversion string) string {
	parts := strings.SplitN(goversion, ".", 3)
	if len(parts) >= 2 {
		n := 0
		for n < len(parts[1]) && parts[1][n] >= '0' && parts[1][n] <= '9' {
			n++
		}
		return parts[0] + "." + parts[1][:n]
	}
	return goversion
}

// gv is the mapping in use, set by main.
var gv *goVersion
