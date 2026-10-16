package goobjw

import "fmt"

// dwarf builds a TEXT symbol's DWARF info and line-table fragments, as
// cmd/internal/obj's populateDWARF does at Flushplist time: the function
// has no code yet, so its size is 0 and the line program has no rows.
func (l *Link) dwarf(s *Sym) {
	fn := s.Func
	m := l.Meta
	name := s.Name
	if m.Writer.TrimInlineHash {
		name = TrimInlineHash(name)
	}
	size := s.Size

	info := &Sym{Kind: "SDWARFFCN"}
	uleb(info, uint64(m.DWARF.AbbrevFunction))
	info.P = append(info.P, name...) // DW_FORM_string
	info.P = append(info.P, 0)
	info.Size = int64(len(info.P))
	if m.DWARF.FuncDIE == "v2" && l.Dwarf5 {
		// DW_FORM_addrx (an index the linker assigns) and DW_FORM_udata.
		typ, sz := l.addrxFlavor(l.DwTextCount * 2)
		off := info.Size
		info.prepwrite(off, sz)
		info.AddRel(Reloc{Off: int32(off), Siz: uint8(sz), Type: typ, Sym: s})
		uleb(info, uint64(size))
	} else {
		// Two DW_FORM_addr: low and high pc.
		for _, add := range []int64{0, size} {
			off := info.Size
			info.prepwrite(off, l.Arch.PtrSize)
			info.AddRel(Reloc{Off: int32(off), Siz: uint8(l.Arch.PtrSize), Type: "R_ADDR", Add: add, Sym: s})
		}
	}
	info.P = append(info.P, 1, 0x9c) // DW_FORM_block1: DW_OP_call_frame_cfa
	info.P = append(info.P, 1, 0, 0, 0)
	info.Size = int64(len(info.P))   // DW_FORM_data4 decl_file: file 0, 1-based
	uleb(info, uint64(fn.StartLine)) // DW_FORM_udata decl_line
	external := byte(1)
	if s.Static {
		external = 0
	}
	info.P = append(info.P, external, 0) // DW_FORM_flag; end of children
	info.Size = int64(len(info.P))
	fn.dwarfInfo = info

	// The line program: DW_LNE_set_address, no rows (every instruction is
	// at pc 0, line 1, file 1, which is the state machine's start),
	// DW_LNS_advance_pc by the size, DW_LNE_end_sequence.
	lines := &Sym{Kind: "SDWARFLINES"}
	lines.P = append(lines.P, 0)
	uleb(lines, uint64(1+l.Arch.PtrSize))
	lines.P = append(lines.P, 2) // DW_LNE_set_address
	lines.Size = int64(len(lines.P))
	off := lines.Size
	lines.prepwrite(off, l.Arch.PtrSize)
	lines.AddRel(Reloc{Off: int32(off), Siz: uint8(l.Arch.PtrSize), Type: "R_ADDR", Sym: s})
	lines.P = append(lines.P, 2) // DW_LNS_advance_pc
	lines.Size = int64(len(lines.P))
	uleb(lines, uint64(size))
	lines.P = append(lines.P, 0, 1, 1) // DW_LNE_end_sequence
	lines.Size = int64(len(lines.P))
	fn.dwarfLines = lines
	fn.dwarfRanges = &Sym{Kind: "SDWARFRANGE"}
}

func (l *Link) addrxFlavor(maxFuncs int) (string, int) {
	for _, f := range l.Meta.DWARF.TextAddrx {
		if maxFuncs <= f.MaxFuncs {
			return f.Reloc, f.Size
		}
	}
	panic(fmt.Sprintf("package has more than %d functions", maxFuncs))
}

func uleb(s *Sym, v uint64) {
	for {
		c := byte(v & 0x7f)
		v >>= 7
		if v != 0 {
			c |= 0x80
		}
		s.P = append(s.P, c)
		if c&0x80 == 0 {
			break
		}
	}
	s.Size = int64(len(s.P))
}
