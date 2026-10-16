package main

import (
	"debug/elf"
	"fmt"
	"sort"
	"strings"

	"github.com/LoyieKing/goc/goobj/enc/cmd/objlib/obj"
	"github.com/LoyieKing/goc/goobj/enc/cmd/objlib/objabi"
	"github.com/LoyieKing/goc/goobj/enc/cmd/objlib/src"
)

func isDataSection(name string) bool {
	for _, p := range []string{".rodata", ".data", ".bss", ".sdata", ".sbss", ".tdata", ".tbss"} {
		if strings.HasPrefix(name, p) {
			return true
		}
	}
	return false
}

// emitData copies the program data: one Go symbol per named global (other
// objects reference it by name) and one per allocated data section for the
// rest. C data holds C pointers, not Go pointers, so all of it is NOPTR.
func (c *converter) emitData() error {
	ctxt := c.ctxt
	secs := c.ef.Sections
	type nameRange struct {
		start, end uint64
		sym        *obj.LSym
	}
	ranges := map[int][]nameRange{}
	for i, es := range c.syms {
		sec := int(es.Section)
		if es.Section == elf.SHN_UNDEF || es.Section >= elf.SHN_LORESERVE || sec >= len(secs) || es.Size == 0 || es.Name == "" || c.isText(sec) {
			continue
		}
		if ty := elf.ST_TYPE(es.Info); ty != elf.STT_OBJECT && ty != elf.STT_NOTYPE {
			continue
		}
		if b := elf.ST_BIND(es.Info); b != elf.STB_GLOBAL && b != elf.STB_WEAK {
			continue
		}
		sc := secs[sec]
		if sc.Flags&elf.SHF_ALLOC == 0 {
			continue
		}
		ls := ctxt.Lookup(c.pkg + "." + es.Name)
		if sc.Type == elf.SHT_NOBITS {
			ctxt.GloblPos(ls, int64(es.Size), obj.NOPTR, src.NoXPos)
		} else {
			b, err := sc.Data()
			if err != nil {
				return err
			}
			if es.Value+es.Size > uint64(len(b)) {
				continue
			}
			ls.Type = objabi.SDATA
			ctxt.GloblPos(ls, int64(es.Size), obj.NOPTR, src.NoXPos)
			writeBytes(ctxt, ls, b[es.Value:es.Value+es.Size])
		}
		c.dataSym[i] = ls
		ranges[sec] = append(ranges[sec], nameRange{es.Value, es.Value + es.Size, ls})
	}
	for i, sc := range secs {
		if sc.Flags&elf.SHF_ALLOC == 0 || c.isText(i) || sc.Size == 0 ||
			(sc.Type != elf.SHT_PROGBITS && sc.Type != elf.SHT_NOBITS) || !isDataSection(sc.Name) {
			continue
		}
		ls := ctxt.Lookup("goc.data." + c.tu + "." + sanitize(sc.Name))
		if sc.Type == elf.SHT_NOBITS {
			ctxt.GloblPos(ls, int64(sc.Size), obj.NOPTR, src.NoXPos)
		} else {
			b, err := sc.Data()
			if err != nil {
				return err
			}
			fl := int(obj.RODATA | obj.DUPOK)
			if sc.Flags&elf.SHF_WRITE != 0 {
				ls.Type = objabi.SDATA
				fl = obj.NOPTR
			}
			ctxt.GloblPos(ls, int64(len(b)), fl, src.NoXPos)
			writeBytes(ctxt, ls, b)
		}
		c.secSym[i] = ls
	}

	// Jump tables: LLVM emits `.long case - table` as R_X86_64_PC32 against
	// the text section with addend case + (field - table). Each table base
	// is the target of a PC-relative code reference; recover the case
	// address from it before resolving the function.
	type tableBase struct {
		sec   int
		off   uint64
		owner *obj.LSym
	}
	var codeRels []struct {
		sec int
		r   rela
	}
	for _, sec := range c.textSecs() {
		rs, err := c.relas(sec)
		if err != nil {
			return err
		}
		for _, r := range rs {
			codeRels = append(codeRels, struct {
				sec int
				r   rela
			}{sec, r})
		}
	}
	for _, i := range sortedKeys(c.secSym) {
		ls := c.secSym[i]
		rs, err := c.relas(i)
		if err != nil {
			return err
		}
		if len(rs) == 0 {
			continue
		}
		for int64(len(ls.P)) < ls.Size {
			ls.WriteInt(ctxt, int64(len(ls.P)), 1, 0)
		}
		pcTextAt := map[uint64]bool{}
		for _, r := range rs {
			if !c.arm64 && elf.R_X86_64(r.Type) == elf.R_X86_64_PC32 && r.Sym > 0 && int(r.Sym-1) < len(c.syms) && c.isText(int(c.syms[r.Sym-1].Section)) {
				pcTextAt[r.Off] = true
			}
		}
		var tables []tableBase
		for _, cr := range codeRels {
			r := cr.r
			if c.arm64 || elf.R_X86_64(r.Type) != elf.R_X86_64_PC32 || r.Sym == 0 || int(r.Sym-1) >= len(c.syms) {
				continue
			}
			es := c.syms[r.Sym-1]
			if int(es.Section) != i {
				continue
			}
			base := int64(es.Value) + r.Add + 4
			if base < 0 || !pcTextAt[uint64(base)] {
				continue
			}
			owner, _ := c.resolveText(cr.sec, r.Off)
			if owner == nil {
				return fmt.Errorf("jump table %s+%#x has no owning function", secs[i].Name, base)
			}
			tables = append(tables, tableBase{cr.sec, uint64(base), owner})
		}
		sort.Slice(tables, func(a, b int) bool { return tables[a].off < tables[b].off })
		for j := 1; j < len(tables); j++ {
			if tables[j-1].off == tables[j].off && tables[j-1].owner != tables[j].owner {
				return fmt.Errorf("jump table %s+%#x has two owners", secs[i].Name, tables[j].off)
			}
		}
		for _, r := range rs {
			var target *obj.LSym
			var add int64
			var ename string
			switch {
			case pcTextAt[r.Off]:
				j := sort.Search(len(tables), func(j int) bool { return tables[j].off > r.Off }) - 1
				if j < 0 {
					return fmt.Errorf("no base for jump table entry %s+%#x", secs[i].Name, r.Off)
				}
				idx := int64(r.Off - tables[j].off)
				es := c.syms[r.Sym-1]
				actual := int64(es.Value) + r.Add - idx
				if actual < 0 {
					return fmt.Errorf("negative jump table target %s+%#x", secs[i].Name, r.Off)
				}
				var delta int64
				target, delta = c.resolveText(int(es.Section), uint64(actual))
				if target != tables[j].owner {
					return fmt.Errorf("jump table %s+%#x targets another function than its owner", secs[i].Name, r.Off)
				}
				// Go's R_PCREL subtracts the field address+4; keep the
				// word equal to case - table.
				add, ename = delta+idx+4, target.Name
			case r.Sym == 0:
				target, add, ename = ls, r.Add, secs[i].Name
			default:
				target, add, ename = c.resolveTarget(r)
			}
			if target == nil {
				return fmt.Errorf("data reloc at %s+%#x: unresolved %q", secs[i].Name, r.Off, ename)
			}
			host, off := ls, int64(r.Off)
			for _, nr := range ranges[i] {
				if r.Off >= nr.start && r.Off < nr.end {
					host, off = nr.sym, int64(r.Off-nr.start)
					break
				}
			}
			var typ objabi.RelocType
			siz := uint8(4)
			if c.arm64 {
				var ok bool
				if typ, siz, ok = arm64DataReloc(r.Type); !ok {
					return fmt.Errorf("unsupported arm64 data reloc type %d in %s at %#x", r.Type, secs[i].Name, r.Off)
				}
				host.GocAddRel(ctxt, obj.Reloc{Off: int32(off), Siz: siz, Type: typ, Add: add, Sym: target})
				continue
			}
			d, ok := gv.X86Data[elf.R_X86_64(r.Type)]
			if !ok {
				return fmt.Errorf("unsupported data reloc type %d in %s at %#x", r.Type, secs[i].Name, r.Off)
			}
			typ, siz = d.Type, d.Siz
			host.GocAddRel(ctxt, obj.Reloc{Off: int32(off), Siz: siz, Type: typ, Add: add, Sym: target})
		}
	}
	return nil
}
