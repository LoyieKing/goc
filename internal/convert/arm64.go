package convert

// linux/arm64: the ELF relocations llc and llvm-mc leave in an AArch64
// .gocobj, mapped to Go's arm64 relocation types. Nothing here decodes or
// emits instructions: the linker ORs its values into immediates, so the
// immediate fields a relocation targets are cleared, as the Go assembler
// leaves them.

import (
	"debug/elf"
	"encoding/binary"
	"fmt"
	"sort"

	"github.com/LoyieKing/goc/internal/goobj"
)

func insnAt(b []byte, off int) uint32       { return binary.LittleEndian.Uint32(b[off:]) }
func putInsnAt(b []byte, off int, w uint32) { binary.LittleEndian.PutUint32(b[off:], w) }

// arm64PagePair maps the low-12 relocation that follows an ADRP to the Go
// relocation covering both instructions.
func arm64PagePair(second elf.R_AARCH64) (string, bool) {
	t, ok := gv.A64PagePair[second]
	return t, ok
}

// arm64ClearPagePair zeros the ADRP immediate (bits 30-29, 23-5) and the
// next instruction's imm12 (bits 21-10; an ADD also loses LSL #12).
func arm64ClearPagePair(code []byte, off int) {
	putInsnAt(code, off, insnAt(code, off)&^0x60FFFFE0)
	w1 := insnAt(code, off+4) &^ 0x003FFC00
	if w1>>24 == 0x91 {
		w1 &^= 1 << 22
	}
	putInsnAt(code, off+4, w1)
}

// arm64TextRelocs translates the relocations inside one function. rs are
// the section's relocations; x is the function.
func (c *converter) arm64TextRelocs(x *fn, rs []rela) error {
	s := x.sym
	var mine []rela
	for _, r := range rs {
		if r.Off >= x.off && r.Off < x.off+x.tab.TextSize {
			mine = append(mine, r)
		}
	}
	sort.SliceStable(mine, func(i, j int) bool { return mine[i].Off < mine[j].Off })
	used := make([]bool, len(mine))
	for i, r := range mine {
		if used[i] {
			continue
		}
		off := int(r.Off - x.off)
		if off%4 != 0 || off+4 > len(s.P) {
			return fmt.Errorf("%s+%#x: relocation is not on a 4-byte instruction", s.Name, off)
		}
		target, add, ename := c.resolveTarget(r)
		if target == nil {
			return fmt.Errorf("reloc at %s+%#x: unresolved symbol %q", s.Name, off, ename)
		}
		next := -1
		if i+1 < len(mine) && mine[i+1].Off == r.Off+4 {
			next = i + 1
		}
		pair := func(want func(elf.R_AARCH64) (string, bool)) (string, error) {
			if next < 0 {
				return "", fmt.Errorf("%s+%#x: ADRP without a low-12 relocation on the next instruction", s.Name, off)
			}
			n := mine[next]
			typ, ok := want(elf.R_AARCH64(n.Type))
			if !ok {
				return "", fmt.Errorf("%s+%#x: ADRP followed by relocation type %d", s.Name, off, n.Type)
			}
			if n.Sym != r.Sym || n.Add != r.Add {
				return "", fmt.Errorf("%s+%#x: ADRP pair targets differ", s.Name, off)
			}
			used[next] = true
			arm64ClearPagePair(s.P, off)
			return typ, nil
		}
		switch elf.R_AARCH64(r.Type) {
		case elf.R_AARCH64_CALL26, elf.R_AARCH64_JUMP26:
			if add != 0 {
				return fmt.Errorf("call at %s+%#x has addend %d", s.Name, off, add)
			}
			putInsnAt(s.P, off, insnAt(s.P, off)&^0x03ffffff)
			s.AddRel(goobj.Reloc{Off: int32(off), Siz: 4, Type: gv.A64Call, Sym: target})
		case elf.R_AARCH64_ADR_PREL_PG_HI21:
			if next >= 0 {
				if _, ok := arm64PagePair(elf.R_AARCH64(mine[next].Type)); !ok {
					next = -1 // e.g. LDST128: Go has no pair type for it
				}
			}
			if next < 0 || mine[next].Sym != r.Sym || mine[next].Add != r.Add {
				// llc scheduled the low half away from the ADRP (or one ADRP
				// feeds several loads). Go's linker patches each half on its
				// own: R_ARM64_PCREL on an ADRP, R_ARM64_LDSTn / R_ARM64_PCREL
				// on the low-12 instruction (see archreloc).
				putInsnAt(s.P, off, insnAt(s.P, off)&^0x60FFFFE0)
				s.AddRel(goobj.Reloc{Off: int32(off), Siz: 4, Type: gv.A64Page, Add: add, Sym: target})
				continue
			}
			typ, err := pair(arm64PagePair)
			if err != nil {
				return err
			}
			s.AddRel(goobj.Reloc{Off: int32(off), Siz: 8, Type: typ, Add: add, Sym: target})
		case elf.R_AARCH64_ADR_GOT_PAGE:
			_, err := pair(func(t elf.R_AARCH64) (string, bool) {
				return gv.A64GOTPair, t == elf.R_AARCH64_LD64_GOT_LO12_NC
			})
			if err != nil {
				return err
			}
			s.AddRel(goobj.Reloc{Off: int32(off), Siz: 8, Type: gv.A64GOTPair, Add: add, Sym: target})
		case elf.R_AARCH64_ADD_ABS_LO12_NC, elf.R_AARCH64_LDST8_ABS_LO12_NC,
			elf.R_AARCH64_LDST16_ABS_LO12_NC, elf.R_AARCH64_LDST32_ABS_LO12_NC,
			elf.R_AARCH64_LDST64_ABS_LO12_NC, elf.R_AARCH64_LDST128_ABS_LO12_NC:
			// A low-12 half whose ADRP is not right before it.
			w := insnAt(s.P, off) &^ 0x003FFC00
			typ := gv.A64Lo12[elf.R_AARCH64(r.Type)]
			if elf.R_AARCH64(r.Type) == elf.R_AARCH64_ADD_ABS_LO12_NC {
				w &^= 1 << 22
			}
			putInsnAt(s.P, off, w)
			s.AddRel(goobj.Reloc{Off: int32(off), Siz: 4, Type: typ, Add: add, Sym: target})
		case elf.R_AARCH64_ABS64, elf.R_AARCH64_ABS32:
			d := gv.A64Data[elf.R_AARCH64(r.Type)]
			s.AddRel(goobj.Reloc{Off: int32(off), Siz: d.Siz, Type: d.Type, Add: add, Sym: target})
		default:
			return fmt.Errorf("unsupported arm64 text reloc type %d in %s at %#x", r.Type, s.Name, off)
		}
	}
	return nil
}

// arm64DataReloc maps a relocation in a data section.
func arm64DataReloc(t uint32) (string, uint8, bool) {
	r, ok := gv.A64Data[elf.R_AARCH64(t)]
	return r.Type, r.Siz, ok
}
