package convert

// How ELF relocations map onto Go relocation types. Types are named by
// their objabi constant; the numbers, which Go renumbers between releases
// (docs/design/go-versions.md §11), come from the release's metadata
// (gometa) when the object is written.

import (
	"debug/elf"
	"fmt"
	"strings"

	"github.com/LoyieKing/goc/internal/gocobj"
	"github.com/LoyieKing/goc/internal/gometa"
)

// sizedReloc is a Go relocation type and the width of the field it patches.
type sizedReloc struct {
	Type string
	Siz  uint8
}

type relocMap struct {
	// amd64 text: a PC-relative field becomes X86Text[type]; one that the
	// .goc call list marks as a call becomes X86Call instead.
	X86Text map[elf.R_X86_64]string
	X86Call string
	X86Data map[elf.R_X86_64]sizedReloc

	// arm64 text.
	A64Call     string                   // BL/B (CALL26, JUMP26)
	A64Page     string                   // a lone ADRP, or a lone ADD low-12
	A64PagePair map[elf.R_AARCH64]string // ADRP + adjacent low-12, by the low-12 type
	A64Lo12     map[elf.R_AARCH64]string // a low-12 load/store without its ADRP
	A64GOTPair  string                   // ADRP GOT page + LD64_GOT_LO12
	A64Data     map[elf.R_AARCH64]sizedReloc
}

var gv = &relocMap{
	X86Text: map[elf.R_X86_64]string{
		elf.R_X86_64_PC32:  "R_PCREL",
		elf.R_X86_64_PLT32: "R_PCREL",
		elf.R_X86_64_32:    "R_ADDR",
		elf.R_X86_64_32S:   "R_ADDR",
	},
	X86Call: "R_CALL",
	X86Data: map[elf.R_X86_64]sizedReloc{
		elf.R_X86_64_64:    {"R_ADDR", 8},
		elf.R_X86_64_32:    {"R_ADDR", 4},
		elf.R_X86_64_32S:   {"R_ADDR", 4},
		elf.R_X86_64_PC32:  {"R_PCREL", 4},
		elf.R_X86_64_PLT32: {"R_PCREL", 4},
	},

	A64Call: "R_CALLARM64",
	A64Page: "R_ARM64_PCREL",
	A64PagePair: map[elf.R_AARCH64]string{
		elf.R_AARCH64_ADD_ABS_LO12_NC:    "R_ADDRARM64",
		elf.R_AARCH64_LDST8_ABS_LO12_NC:  "R_ARM64_PCREL_LDST8",
		elf.R_AARCH64_LDST16_ABS_LO12_NC: "R_ARM64_PCREL_LDST16",
		elf.R_AARCH64_LDST32_ABS_LO12_NC: "R_ARM64_PCREL_LDST32",
		elf.R_AARCH64_LDST64_ABS_LO12_NC: "R_ARM64_PCREL_LDST64",
	},
	A64Lo12: map[elf.R_AARCH64]string{
		elf.R_AARCH64_ADD_ABS_LO12_NC:     "R_ARM64_PCREL",
		elf.R_AARCH64_LDST8_ABS_LO12_NC:   "R_ARM64_LDST8",
		elf.R_AARCH64_LDST16_ABS_LO12_NC:  "R_ARM64_LDST16",
		elf.R_AARCH64_LDST32_ABS_LO12_NC:  "R_ARM64_LDST32",
		elf.R_AARCH64_LDST64_ABS_LO12_NC:  "R_ARM64_LDST64",
		elf.R_AARCH64_LDST128_ABS_LO12_NC: "R_ARM64_LDST128",
	},
	A64GOTPair: "R_ARM64_GOTPCREL",
	A64Data: map[elf.R_AARCH64]sizedReloc{
		elf.R_AARCH64_ABS64: {"R_ADDR", 8},
		elf.R_AARCH64_ABS32: {"R_ADDR", 4},
	},
}

// profileFor returns the goc ABI profile (gocobj/abi.go) that states the
// release's ABI facts for goarch.
func profileFor(m *gometa.Meta, goarch string) (gocobj.Profile, error) {
	a, ok := m.Arch[goarch]
	if !ok {
		return gocobj.Profile{}, fmt.Errorf("%s: no facts for GOARCH=%s", m.Go, goarch)
	}
	if !a.StackCopyAdjustsFP {
		return gocobj.Profile{}, fmt.Errorf("goc does not support GOARCH=%s with %s: its runtime does not adjust "+
			"saved frame pointers when it copies a stack (runtime.adjustframe), and goc's code addresses locals "+
			"from the frame pointer (docs/design/go-versions.md)", goarch, m.Go)
	}
	got := gocobj.Profile{Arch: goarch, PtrSize: a.PtrSize, GReg: a.GReg,
		StackGuard0Off: m.ABI.GOffsets["stackguard0"], StackSmall: m.ABI.StackSmall,
		StackBig: m.ABI.StackBig, Morestack: a.Morestack, FPRegs: a.FloatArgs}
	for _, r := range a.IntArgs {
		if goarch == "amd64" && r[0] != 'R' {
			r = "R" + r // AX -> RAX, as the profiles spell them
		}
		got.IntRegs = append(got.IntRegs, r)
	}
	var best gocobj.Profile
	for _, p := range gocobj.Profiles() {
		// Prefer the goabi* names; go1.24-* are their old aliases.
		if gocobj.SameFacts(p, got) && (best.Name == "" || strings.HasPrefix(p.Name, "goabi") && !strings.HasPrefix(best.Name, "goabi")) {
			best = p
		}
	}
	if best.Name == "" {
		return gocobj.Profile{}, fmt.Errorf("%s %s has ABI facts no goc profile states: %+v\n"+
			"add one to llvm/include/Goc/AbiProfiles.def and internal/gocobj/abi.go (docs/design/go-versions.md)", m.Go, goarch, got)
	}
	return best, nil
}
