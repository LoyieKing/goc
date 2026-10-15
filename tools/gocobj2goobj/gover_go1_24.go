package main

// Go 1.24: cmd/internal/objabi/reloctype.go, internal/abi/symtab.go.

import (
	"debug/elf"

	"github.com/LoyieKing/goc/goobj/enc/cmd/objlib/objabi"
	"github.com/LoyieKing/goc/goobj/enc/stdinternal/abi"
)

func init() {
	registerGoVersion(&goVersion{
		Name: "go1.24",
		Profiles: map[string]string{
			"amd64": "go1.24-amd64",
			"arm64": "go1.24-arm64",
		},

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
	})
}
