package gocobj

import (
	"bytes"
	"fmt"
)

// Verify checks the invariants listed in docs/gocobj-format.md and returns
// every violation found (nil means the file is valid).
func Verify(f *File) []error {
	var errs []error
	bad := func(format string, a ...any) { errs = append(errs, fmt.Errorf(format, a...)) }

	prof, ok := LookupProfile(f.Header.Profile)
	if !ok {
		bad("header: unknown ABI profile %q", f.Header.Profile)
	}
	arch := map[uint8]string{ArchAMD64: "amd64", ArchARM64: "arm64"}[f.Header.Arch]
	if arch == "" {
		bad("header: unknown arch %d", f.Header.Arch)
	} else if ok && prof.Arch != arch {
		bad("header: arch %s does not match profile %s", arch, prof.Name)
	}
	ptr := uint64(8)
	if ok {
		ptr = uint64(prof.PtrSize)
	}
	sym := func(where, name string) *Symbol {
		s := f.Symbols[name]
		switch {
		case name == "":
			bad("%s: empty symbol name", where)
		case s == nil:
			bad("%s: no ELF symbol %q", where, name)
		case s.Count > 1:
			bad("%s: ELF symbol %q is not unique (%d)", where, name, s.Count)
		}
		return s
	}

	funcs := map[string]*Func{}
	for i := range f.Funcs {
		fn := &f.Funcs[i]
		w := "func " + fn.Name
		if funcs[fn.Name] != nil {
			bad("%s: listed twice", w)
		}
		funcs[fn.Name] = fn
		if s := sym(w, fn.Name); s != nil {
			if !s.Func || !s.Defined {
				bad("%s: ELF symbol is not a defined function", w)
			} else if s.Size != fn.TextSize {
				bad("%s: text_size %d, ELF symbol size %d", w, fn.TextSize, s.Size)
			}
		}
		if fn.File >= uint64(len(f.Files)) && len(f.Files) > 0 || len(f.Files) == 0 && fn.File != 0 {
			bad("%s: file index %d out of range", w, fn.File)
		}
		if fn.ABI > ABIInternal {
			bad("%s: bad abi %d", w, fn.ABI)
		}
		if fn.pcvOff != 0 && !f.pcvStarts[fn.pcvOff-1] {
			bad("%s: pcv offset %d is not a record start", w, fn.pcvOff-1)
		}
		if fn.gcOff != 0 && !f.gcStarts[fn.gcOff-1] {
			bad("%s: gc offset %d is not a record start", w, fn.gcOff-1)
		}
		for _, t := range fn.PCValue {
			if t.Kind == 0 || t.Kind >= pcLast {
				bad("%s: unknown pcvalue kind %d", w, t.Kind)
			}
			for j, e := range t.Entries {
				switch {
				case j == 0 && e.PC != 0:
					bad("%s: %s table starts at pc %d, not 0", w, t.Kind, e.PC)
				case j > 0 && e.PC <= t.Entries[j-1].PC:
					bad("%s: %s table not increasing at entry %d", w, t.Kind, j)
				}
				if fn.TextSize > 0 && e.PC >= fn.TextSize {
					bad("%s: %s pc %d outside function (%d bytes)", w, t.Kind, e.PC, fn.TextSize)
				}
			}
		}
		if g := fn.GC; g != nil {
			if g.NLocals*ptr > fn.FrameSize {
				bad("%s: %d locals bits do not fit frame of %d bytes", w, g.NLocals, fn.FrameSize)
			}
			for j, sp := range g.Safepoints {
				if sp.PC == 0 || sp.PC > fn.TextSize {
					bad("%s: safepoint pc %d outside function", w, sp.PC)
				}
				if j > 0 && sp.PC <= g.Safepoints[j-1].PC {
					bad("%s: safepoints not sorted at %d", w, j)
				}
			}
		}
		if ok && f.Header.Flags&HeaderStackChecksInCode != 0 && fn.Flags&FuncNeedsStackCheck != 0 {
			if code := f.Text[fn.Name]; !hasStackCheck(prof, code) {
				bad("%s: needs_stack_check but code does not start with the %s stack check", w, prof.Name)
			}
		}
	}

	for _, wr := range f.Wrappers {
		w := "wrapper " + wr.Sym
		if fn := funcs[wr.Sym]; fn == nil {
			bad("%s: function not in %s", w, SecFuncs)
		} else if fn.Kind != 1 {
			bad("%s: function kind %d, want 1", w, fn.Kind)
		}
		sym(w, wr.Target)
		if wr.FromABI > 2 || wr.ToABI > 2 {
			bad("%s: bad abi %d -> %d", w, wr.FromABI, wr.ToABI)
		}
	}
	for i, sm := range f.Stackmaps {
		w := fmt.Sprintf("stackmap %d (%s+%d)", i, sm.Func, sm.InsnOff)
		if fn := funcs[sm.Func]; fn == nil {
			bad("%s: function not in %s", w, SecFuncs)
		} else if sm.InsnOff >= fn.TextSize {
			bad("%s: past function end %d", w, fn.TextSize)
		}
	}
	for i, r := range f.Relocs {
		w := fmt.Sprintf("reloc %d (%s+%d)", i, r.Func, r.Offset)
		fn := funcs[r.Func]
		if fn == nil {
			bad("%s: function not in %s", w, SecFuncs)
		} else if r.Offset+uint64(r.Width) > fn.TextSize {
			bad("%s: width %d runs past function end %d", w, r.Width, fn.TextSize)
		}
		switch {
		case r.Kind == RCallIndirectMark:
			// width is the call instruction's length; there is no target
			if r.Width == 0 || r.Width > 15 {
				bad("%s: bad call length %d", w, r.Width)
			}
		case r.Width == 1, r.Width == 2, r.Width == 4, r.Width == 8:
		default:
			bad("%s: bad width %d", w, r.Width)
		}
		if r.Kind == 0 || r.Kind >= rLast {
			bad("%s: unknown kind %d", w, r.Kind)
		}
		if r.Kind != RCallIndirectMark {
			sym(w, r.Target)
		}
		if t, ok := f.ELFRelocs[fmt.Sprintf("%s+%d", r.Func, r.Offset)]; ok && t != r.Target {
			bad("%s: ELF relocation there targets %q, table says %q", w, t, r.Target)
		}
		if r.Flags&RelocSafepoint != 0 && fn != nil {
			pc := r.Offset + uint64(r.Width)
			found := false
			if fn.GC != nil {
				for _, sp := range fn.GC.Safepoints {
					found = found || sp.PC == pc
				}
			}
			if !found {
				bad("%s: safepoint call without gc record at pc %d", w, pc)
			}
		}
	}

	for _, d := range f.Data {
		w := "data " + d.Name
		sym(w, d.Name)
		if d.NBits*ptr > d.Size+ptr-1 {
			bad("%s: %d pointer bits do not fit %d bytes", w, d.NBits, d.Size)
		}
	}
	return errs
}

// hasStackCheck reports whether code starts with the profile's stack check.
// amd64, g in R14, stackguard0 at 16:
//
//	cmp rsp, [r14+16]          49 3b 66 10
//	lea r12, [rsp-n]; cmp r12, [r14+16]
//	                           4c 8d 64 24 xx / 4c 8d a4 24 xx xx xx xx ; 4d 3b 66 10
func hasStackCheck(p Profile, code []byte) bool {
	if p.Arch != "amd64" || p.GReg != "R14" || p.StackGuard0Off != 16 {
		return false // no matcher for this profile yet
	}
	cmp := []byte{0x3b, 0x66, 0x10}
	if len(code) >= 4 && code[0] == 0x49 && bytes.Equal(code[1:4], cmp) {
		return true
	}
	for _, lea := range []struct {
		pfx []byte
		n   int
	}{{[]byte{0x4c, 0x8d, 0x64, 0x24}, 1}, {[]byte{0x4c, 0x8d, 0xa4, 0x24}, 4}} {
		k := len(lea.pfx) + lea.n
		if len(code) >= k+4 && bytes.Equal(code[:len(lea.pfx)], lea.pfx) &&
			code[k] == 0x4d && bytes.Equal(code[k+1:k+4], cmp) {
			return true
		}
	}
	return false
}
