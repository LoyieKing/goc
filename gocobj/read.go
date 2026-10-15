package gocobj

import (
	"bytes"
	"debug/elf"
	"encoding/binary"
	"fmt"
	"io"
	"os"
)

// Open reads a .gocobj from disk.
func Open(path string) (*File, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	return Parse(f)
}

// Parse reads a .gocobj. It checks the encoding; Verify checks the meaning.
func Parse(ra io.ReaderAt) (*File, error) {
	ef, err := elf.NewFile(ra)
	if err != nil {
		return nil, err
	}
	sec := func(name string) ([]byte, error) {
		s := ef.Section(name)
		if s == nil {
			return nil, nil
		}
		return s.Data()
	}
	out := &File{Symbols: map[string]*Symbol{}, Text: map[string][]byte{},
		pcvStarts: map[uint64]bool{}, gcStarts: map[uint64]bool{}}

	hdr, err := sec(SecHeader)
	if err != nil {
		return nil, err
	}
	if hdr == nil {
		return nil, fmt.Errorf("not a gocobj: no %s section", SecHeader)
	}
	if len(hdr) < HeaderSize || string(hdr[:8]) != Magic {
		return nil, fmt.Errorf("%s: bad magic or size", SecHeader)
	}
	le := binary.LittleEndian
	out.Header.Major = le.Uint16(hdr[8:])
	out.Header.Minor = le.Uint16(hdr[10:])
	if out.Header.Major != Major {
		return nil, fmt.Errorf("gocobj major %d, reader supports %d", out.Header.Major, Major)
	}
	out.Header.Arch = hdr[12]
	out.Header.Flags = hdr[13]

	strtab, err := sec(SecStrtab)
	if err != nil {
		return nil, err
	}
	var strErr error
	str := func(off uint64) string {
		if off >= uint64(len(strtab)) {
			if off != 0 && strErr == nil {
				strErr = fmt.Errorf("strref %d out of range", off)
			}
			return ""
		}
		end := bytes.IndexByte(strtab[off:], 0)
		if end < 0 {
			strErr = fmt.Errorf("strref %d not terminated", off)
			return ""
		}
		return string(strtab[off : off+uint64(end)])
	}
	out.Header.Profile = str(uint64(le.Uint32(hdr[16:])))
	out.Header.Producer = str(uint64(le.Uint32(hdr[20:])))

	syms, err := ef.Symbols()
	if err != nil && err != elf.ErrNoSymbols {
		return nil, err
	}
	for _, s := range syms {
		if s.Name == "" || elf.ST_TYPE(s.Info) == elf.STT_SECTION || elf.ST_TYPE(s.Info) == elf.STT_FILE {
			continue
		}
		e := out.Symbols[s.Name]
		if e == nil {
			e = &Symbol{Name: s.Name}
			out.Symbols[s.Name] = e
		}
		e.Count++
		e.Func = elf.ST_TYPE(s.Info) == elf.STT_FUNC
		e.Defined = s.Section != elf.SHN_UNDEF && int(s.Section) < len(ef.Sections)
		e.Size = s.Size
		e.Value = s.Value
		if e.Defined {
			e.Section = ef.Sections[s.Section].Name
			if e.Func && ef.Sections[s.Section].Type == elf.SHT_PROGBITS {
				if d, err := ef.Sections[s.Section].Data(); err == nil && s.Value+s.Size <= uint64(len(d)) {
					out.Text[s.Name] = d[s.Value : s.Value+s.Size]
				}
			}
		}
	}

	out.ELFRelocs = elfCallRelocs(ef, syms)

	files, _ := sec(SecFiles)
	if err := records(SecFiles, files, func(_ uint64, r *buf) error {
		out.Files = append(out.Files, str(r.uleb()))
		return nil
	}); err != nil {
		return nil, err
	}

	pcv := map[uint64][]PCTable{}
	b, _ := sec(SecPCValue)
	if err := records(SecPCValue, b, func(off uint64, r *buf) error {
		out.pcvStarts[off] = true
		n := r.uleb()
		var ts []PCTable
		for i := uint64(0); i < n && r.err == nil; i++ {
			t := PCTable{Kind: PCKind(r.u8())}
			c := r.uleb()
			if c > uint64(len(r.b)) {
				return errShort
			}
			for j := uint64(0); j < c && r.err == nil; j++ {
				t.Entries = append(t.Entries, PCEntry{r.uleb(), r.sleb()})
			}
			ts = append(ts, t)
		}
		pcv[off] = ts
		return nil
	}); err != nil {
		return nil, err
	}

	gcs := map[uint64]*GCMap{}
	b, _ = sec(SecGCMaps)
	if err := records(SecGCMaps, b, func(off uint64, r *buf) error {
		out.gcStarts[off] = true
		g := &GCMap{NArgs: r.uleb(), NLocals: r.uleb()}
		n := r.uleb()
		if n > uint64(len(r.b)) {
			return errShort
		}
		for i := uint64(0); i < n && r.err == nil; i++ {
			sp := Safepoint{PC: r.uleb()}
			sp.Args = r.bytes((g.NArgs + 7) / 8)
			sp.Locals = r.bytes((g.NLocals + 7) / 8)
			sp.Color = r.bytes(r.uleb())
			g.Safepoints = append(g.Safepoints, sp)
		}
		gcs[off] = g
		return nil
	}); err != nil {
		return nil, err
	}

	b, _ = sec(SecFuncs)
	if err := records(SecFuncs, b, func(_ uint64, r *buf) error {
		fn := Func{Name: str(r.uleb()), TextSize: r.uleb(), FrameSize: r.uleb(), ArgSize: r.uleb(),
			ABI: r.u8(), Kind: r.u8(), Flags: r.uleb(), FuncID: r.u8(), File: r.uleb(), StartLine: r.uleb()}
		fn.pcvOff = r.uleb()
		fn.gcOff = r.uleb()
		r.uleb() // inl, reserved in 1.0
		if fn.pcvOff != 0 {
			fn.PCValue = pcv[fn.pcvOff-1]
		}
		if fn.gcOff != 0 {
			fn.GC = gcs[fn.gcOff-1]
		}
		out.Funcs = append(out.Funcs, fn)
		return nil
	}); err != nil {
		return nil, err
	}

	b, _ = sec(SecRelocs)
	if err := records(SecRelocs, b, func(_ uint64, r *buf) error {
		out.Relocs = append(out.Relocs, Reloc{Func: str(r.uleb()), Offset: r.uleb(), Width: r.u8(),
			Kind: RelocKind(r.u8()), Target: str(r.uleb()), Addend: r.sleb(), Flags: r.u8()})
		return nil
	}); err != nil {
		return nil, err
	}

	b, _ = sec(SecData)
	if err := records(SecData, b, func(_ uint64, r *buf) error {
		d := Data{Name: str(r.uleb()), Size: r.uleb(), Log2Align: r.u8(), Flags: r.uleb(), NBits: r.uleb()}
		d.Bitmap = r.bytes((d.NBits + 7) / 8)
		out.Data = append(out.Data, d)
		return nil
	}); err != nil {
		return nil, err
	}
	if strErr != nil {
		return nil, fmt.Errorf("%s: %w", SecStrtab, strErr)
	}
	return out, nil
}

// elfCallRelocs indexes the 4-byte PC-relative relocations of every text
// section by "function+offset", where function is the enclosing STT_FUNC.
func elfCallRelocs(ef *elf.File, syms []elf.Symbol) map[string]string {
	out := map[string]string{}
	if ef.Machine != elf.EM_X86_64 {
		return out
	}
	for si, s := range ef.Sections {
		if s.Flags&elf.SHF_EXECINSTR == 0 {
			continue
		}
		rs := ef.Section(".rela" + s.Name)
		if rs == nil {
			continue
		}
		data, err := rs.Data()
		if err != nil {
			continue
		}
		var funcs []elf.Symbol
		for _, y := range syms {
			if int(y.Section) == si && elf.ST_TYPE(y.Info) == elf.STT_FUNC {
				funcs = append(funcs, y)
			}
		}
		for i := 0; i+24 <= len(data); i += 24 {
			off := binary.LittleEndian.Uint64(data[i:])
			info := binary.LittleEndian.Uint64(data[i+8:])
			typ := elf.R_X86_64(info & 0xffffffff)
			idx := info >> 32
			if (typ != elf.R_X86_64_PLT32 && typ != elf.R_X86_64_PC32) || idx == 0 || int(idx) > len(syms) {
				continue
			}
			for _, f := range funcs {
				if off >= f.Value && off < f.Value+f.Size {
					out[fmt.Sprintf("%s+%d", f.Name, off-f.Value)] = syms[idx-1].Name
				}
			}
		}
	}
	return out
}
