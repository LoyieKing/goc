package main

// How ELF relocations map onto Go relocation types, and the funcdata/pcdata
// indices the stack maps go under. Every entry names its objabi/abi
// constant; the numbers come from the Go object writer this converter was
// built against, which tools/goc-goenc copies from the GOROOT that links
// the result (Go renumbers R_* between releases: docs/go-version-diffs.md).
// Nothing here depends on the Go version.

import (
	"debug/elf"
	"fmt"
	"runtime"

	"github.com/LoyieKing/goc/gocobj"
	"github.com/LoyieKing/goc/goobj/enc/cmd/objlib/objabi"
	"github.com/LoyieKing/goc/goobj/enc/stdinternal/abi"
	"github.com/LoyieKing/goc/goobj/enc/stdinternal/buildcfg"
)

// sizedReloc is a Go relocation type and the width of the field it patches.
type sizedReloc struct {
	Type objabi.RelocType
	Siz  uint8
}

type relocMap struct {
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

// gv is the mapping in use.
var gv = &relocMap{
	X86Text: map[elf.R_X86_64]objabi.RelocType{
		elf.R_X86_64_PC32:  objabi.R_PCREL,
		elf.R_X86_64_PLT32: objabi.R_PCREL,
		elf.R_X86_64_32:    objabi.R_ADDR,
		elf.R_X86_64_32S:   objabi.R_ADDR,
	},
	X86Call: objabi.R_CALL,
	X86Data: map[elf.R_X86_64]sizedReloc{
		elf.R_X86_64_64:    {objabi.R_ADDR, 8},
		elf.R_X86_64_32:    {objabi.R_ADDR, 4},
		elf.R_X86_64_32S:   {objabi.R_ADDR, 4},
		elf.R_X86_64_PC32:  {objabi.R_PCREL, 4},
		elf.R_X86_64_PLT32: {objabi.R_PCREL, 4},
	},

	A64Call: objabi.R_CALLARM64,
	A64Page: objabi.R_ARM64_PCREL,
	A64PagePair: map[elf.R_AARCH64]objabi.RelocType{
		elf.R_AARCH64_ADD_ABS_LO12_NC:    objabi.R_ADDRARM64,
		elf.R_AARCH64_LDST8_ABS_LO12_NC:  objabi.R_ARM64_PCREL_LDST8,
		elf.R_AARCH64_LDST16_ABS_LO12_NC: objabi.R_ARM64_PCREL_LDST16,
		elf.R_AARCH64_LDST32_ABS_LO12_NC: objabi.R_ARM64_PCREL_LDST32,
		elf.R_AARCH64_LDST64_ABS_LO12_NC: objabi.R_ARM64_PCREL_LDST64,
	},
	A64Lo12: map[elf.R_AARCH64]objabi.RelocType{
		elf.R_AARCH64_ADD_ABS_LO12_NC:     objabi.R_ARM64_PCREL,
		elf.R_AARCH64_LDST8_ABS_LO12_NC:   objabi.R_ARM64_LDST8,
		elf.R_AARCH64_LDST16_ABS_LO12_NC:  objabi.R_ARM64_LDST16,
		elf.R_AARCH64_LDST32_ABS_LO12_NC:  objabi.R_ARM64_LDST32,
		elf.R_AARCH64_LDST64_ABS_LO12_NC:  objabi.R_ARM64_LDST64,
		elf.R_AARCH64_LDST128_ABS_LO12_NC: objabi.R_ARM64_LDST128,
	},
	A64GOTPair: objabi.R_ARM64_GOTPCREL,
	A64Data: map[elf.R_AARCH64]sizedReloc{
		elf.R_AARCH64_ABS64: {objabi.R_ADDR, 8},
		elf.R_AARCH64_ABS32: {objabi.R_ADDR, 4},
	},

	FuncdataArgs:        abi.FUNCDATA_ArgsPointerMaps,
	FuncdataLocals:      abi.FUNCDATA_LocalsPointerMaps,
	PcdataStackMapIndex: abi.PCDATA_StackMapIndex,
}

// goVersion is the Go release the writer was copied from. The linker
// compares the object header, which carries it, byte for byte.
func goVersion() string { return buildcfg.Version }

// profileFor is the ABI profile tools/goc-goenc matched this Go release to.
func profileFor(goarch string) (gocobj.Profile, error) {
	name, ok := objabi.GocProfiles[goarch]
	if !ok {
		return gocobj.Profile{}, fmt.Errorf("no goc ABI profile for GOARCH=%s with %s", goarch, goVersion())
	}
	p, ok := gocobj.LookupProfile(name)
	if !ok {
		return gocobj.Profile{}, fmt.Errorf("goc ABI profile %q (from goc-goenc) is not in gocobj/abi.go", name)
	}
	return p, nil
}

// builtWith reports a converter built by one Go and run for another: its
// objects would carry the wrong header.
func builtWith() string { return runtime.Version() }
