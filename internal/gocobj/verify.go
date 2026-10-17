package gocobj

import (
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
		for j, n := range fn.Inl {
			if n.Parent < -1 || n.Parent >= int64(j) {
				bad("%s: inltree %d: parent %d", w, j, n.Parent)
			}
			if n.File >= uint64(len(f.Files)) {
				bad("%s: inltree %d: file %d out of range", w, j, n.File)
			}
			if fn.TextSize > 0 && n.ParentPC >= fn.TextSize {
				bad("%s: inltree %d: parent pc %d outside function", w, j, n.ParentPC)
			}
		}
		for _, t := range fn.PCValue {
			for _, e := range t.Entries {
				if t.Kind == PCFile && (e.Value < 0 || e.Value >= int64(len(f.Files))) {
					bad("%s: FILE %d out of range", w, e.Value)
				}
				if t.Kind == PCInlIndex && (e.Value < -1 || e.Value >= int64(len(fn.Inl))) {
					bad("%s: INL_INDEX %d out of range", w, e.Value)
				}
			}
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
			// The morestack call's map covers the cold block's saves too.
			maxSP := fn.FrameSize
			for _, t := range fn.PCValue {
				if t.Kind == PCSPDelta {
					for _, e := range t.Entries {
						if e.Value > 0 && uint64(e.Value) > maxSP {
							maxSP = uint64(e.Value)
						}
					}
				}
			}
			if g.NLocals*ptr > maxSP {
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
			if !hasStackCheck(fn) {
				bad("%s: needs_stack_check but the tables show no %s entry check", w, prof.Name)
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

// hasStackCheck reports whether the tables describe an entry stack check:
// the code from pc 0 is an unsafe point (the check), and SP_DELTA starts at
// the entry SP. The backend emits the check; the bytes are its business.
func hasStackCheck(fn *Func) bool {
	var sp, up bool
	for _, t := range fn.PCValue {
		if len(t.Entries) == 0 || t.Entries[0].PC != 0 {
			continue
		}
		switch t.Kind {
		case PCSPDelta:
			sp = t.Entries[0].Value == 0
		case PCUnsafePoint:
			up = t.Entries[0].Value == -2
		}
	}
	return sp && up
}
