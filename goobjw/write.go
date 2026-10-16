package goobjw

import (
	"bytes"
	"crypto/sha256"
	"encoding/base64"
	"encoding/binary"
	"fmt"
	"io"
	"sort"
	"strings"
)

// Traversal flags (cmd/internal/obj's traverseSyms).
const (
	travDefs = 1 << iota
	travRefs
	travAux
	travPcdata
	travAll = travDefs | travRefs | travAux | travPcdata
)

func (l *Link) traverse(flag int, fn func(*Sym)) {
	nn := func(s *Sym) {
		if s != nil {
			fn(s)
		}
	}
	for _, list := range [][]*Sym{l.Text, l.Data} {
		for _, s := range list {
			if flag&travDefs != 0 {
				fn(s)
			}
			if flag&travRefs != 0 {
				for _, r := range s.R {
					nn(r.Sym)
				}
			}
			if flag&travAux != 0 && s.Kind == "STEXT" {
				l.traverseFuncAux(flag, s, func(_, aux *Sym) { fn(aux) })
			}
			if flag&travPcdata != 0 && s.Kind == "STEXT" {
				f := s.Func
				nn(f.Pcsp)
				nn(f.Pcfile)
				nn(f.Pcline)
				nn(f.Pcinline)
				for _, d := range f.Pcdata {
					nn(d)
				}
			}
		}
	}
}

func (l *Link) traverseFuncAux(flag int, s *Sym, fn func(parent, aux *Sym)) {
	f := s.Func
	for _, d := range f.Funcdata {
		if d != nil {
			fn(s, d)
		}
	}
	for _, i := range sortedFiles(f.UsedFiles) {
		fn(s, l.Lookup(l.files[i]))
	}
	for _, n := range f.InlTree {
		if n.Func != nil {
			fn(s, n.Func)
		}
	}
	for _, a := range []*Sym{f.dwarfRanges, nil, f.dwarfLines, f.dwarfInfo} {
		if a == nil || a.Size == 0 {
			continue
		}
		fn(s, a)
		if flag&travRefs != 0 {
			for _, r := range a.R {
				if r.Sym != nil {
					fn(a, r.Sym)
				}
			}
		}
	}
}

func (l *Link) hashSection(s *Sym) (byte, error) {
	if s.Pcdata {
		return 'P', nil
	}
	return 0, fmt.Errorf("content-addressable symbol %q is not a pc-value table", s.Name)
}

func (l *Link) builtinIdx(name string, abi ABI) int {
	idx := -1
	for i, b := range l.Meta.Object.Builtins { // builtinMap: the last entry of a name wins
		if b.Name == name {
			idx = i
		}
	}
	if idx < 0 {
		return -1
	}
	if l.RegabiWrappers && l.Meta.Object.Builtins[idx].ABI != int(abi) {
		return -1
	}
	return idx
}

// number assigns every symbol its index (obj.(*Link).NumberSyms for an
// assembler object: every non-static symbol is a non-package symbol).
func (l *Link) number() error {
	pk := l.Meta.Object.PkgIdx
	var err error
	l.traverse(travDefs|travPcdata, func(s *Sym) {
		switch {
		case s.ContentAddressable:
			sec, e := l.hashSection(s)
			if e != nil && err == nil {
				err = e
			}
			if s.Size <= 8 && len(s.R) == 0 && sec == 0 {
				s.pkgIdx, s.symIdx = int32(pk["Hashed64"]), int32(len(l.hashed64))
				l.hashed64 = append(l.hashed64, s)
			} else {
				s.pkgIdx, s.symIdx = int32(pk["Hashed"]), int32(len(l.hashed))
				l.hashed = append(l.hashed, s)
			}
		case !s.Static:
			s.pkgIdx, s.symIdx = int32(pk["None"]), int32(len(l.nonpkgdefs))
			l.nonpkgdefs = append(l.nonpkgdefs, s)
		default:
			s.pkgIdx, s.symIdx = int32(pk["Self"]), int32(len(l.defs))
			l.defs = append(l.defs, s)
		}
		s.indexed = true
	})
	if err != nil {
		return err
	}
	nonpkg := int32(len(l.nonpkgdefs))
	l.traverse(travRefs|travAux, func(rs *Sym) {
		if rs.indexed {
			return
		}
		if i := l.builtinIdx(rs.Name, rs.ABI); i >= 0 {
			rs.pkgIdx, rs.symIdx, rs.indexed = int32(pk["Builtin"]), int32(i), true
			return
		}
		if rs.ContentAddressable && err == nil {
			err = fmt.Errorf("reference to undefined content-addressable symbol %q", rs.Name)
		}
		rs.pkgIdx, rs.symIdx, rs.indexed = int32(pk["None"]), nonpkg, true
		l.nonpkgrefs = append(l.nonpkgrefs, rs)
		nonpkg++
	})
	return err
}

// funcInfoSyms makes the FuncInfo symbols and indexes the DWARF ones
// (genFuncInfoSyms).
func (l *Link) funcInfoSyms() {
	self := int32(l.Meta.Object.PkgIdx["Self"])
	idx := int32(len(l.defs))
	var out []*Sym
	for _, s := range l.Text {
		f := s.Func
		var b bytes.Buffer
		u32 := func(v uint32) { binary.Write(&b, binary.LittleEndian, v) }
		u32(uint32(f.Args))
		u32(uint32(f.Locals))
		b.Write([]byte{f.FuncID, f.FuncFlag, 0, 0})
		u32(uint32(f.StartLine))
		files := sortedFiles(f.UsedFiles)
		u32(uint32(len(files)))
		for _, x := range files {
			u32(x)
		}
		u32(uint32(len(f.InlTree)))
		for _, n := range f.InlTree {
			u32(uint32(n.Parent))
			u32(n.File)
			u32(uint32(n.Line))
			p, i := symRef(n.Func)
			u32(p)
			u32(i)
			u32(uint32(n.ParentPC))
		}
		is := &Sym{Kind: "SDATA", P: b.Bytes(), Size: int64(b.Len()), pkgIdx: self, symIdx: idx, indexed: true}
		idx++
		out = append(out, is)
		f.funcInfoSym = is
		for _, a := range []*Sym{f.dwarfRanges, nil, f.dwarfLines, f.dwarfInfo} {
			if a == nil || a.Size == 0 {
				continue
			}
			a.pkgIdx, a.symIdx, a.indexed = self, idx, true
			idx++
			out = append(out, a)
		}
	}
	l.defs = append(l.defs, out...)
}

func symRef(s *Sym) (uint32, uint32) {
	if s == nil {
		return 0, 0
	}
	if !s.indexed {
		panic("unindexed symbol reference " + s.Name)
	}
	return uint32(s.pkgIdx), uint32(s.symIdx)
}

type writer struct {
	b    bytes.Buffer
	strs map[string]uint32
}

func (w *writer) off() uint32 { return uint32(w.b.Len()) }
func (w *writer) u8(v uint8)  { w.b.WriteByte(v) }
func (w *writer) u16(v uint16) {
	var t [2]byte
	binary.LittleEndian.PutUint16(t[:], v)
	w.b.Write(t[:])
}
func (w *writer) u32(v uint32) {
	var t [4]byte
	binary.LittleEndian.PutUint32(t[:], v)
	w.b.Write(t[:])
}
func (w *writer) u64(v uint64) {
	var t [8]byte
	binary.LittleEndian.PutUint64(t[:], v)
	w.b.Write(t[:])
}
func (w *writer) addString(s string) {
	if _, ok := w.strs[s]; ok {
		return
	}
	w.strs[s] = w.off()
	w.b.WriteString(s)
}
func (w *writer) stringRef(s string) {
	off, ok := w.strs[s]
	if !ok {
		panic("string not added: " + s)
	}
	w.u32(uint32(len(s)))
	w.u32(off)
}
func (w *writer) ref(s *Sym) {
	p, i := symRef(s)
	w.u32(p)
	w.u32(i)
}

// Write writes the object: header is the text line(s) before the goobj
// data (objabi.HeaderString's "go object ...\n"); "!\n" follows it.
func (l *Link) Write(out io.Writer, header string) error {
	if !l.flushed {
		return fmt.Errorf("Write before Flush")
	}
	m := l.Meta
	o := &m.Object
	if err := l.number(); err != nil {
		return err
	}
	l.funcInfoSyms()

	w := &writer{strs: map[string]uint32{}}
	blk := map[string]int{}
	for i, b := range o.Blocks {
		blk[b] = i
	}
	if _, ok := blk["End"]; !ok || len(o.Blocks) != blk["End"]+1 {
		return fmt.Errorf("%s: unexpected goobj block list %v", m.Go, o.Blocks)
	}
	offsets := make([]uint32, len(o.Blocks))
	// Header: magic, fingerprint (zero), flags, block offsets (patched
	// at the end).
	w.b.WriteString(o.Magic)
	w.b.Write(make([]byte, 8))
	w.u32(uint32(o.ObjFlags["FromAssembly"]))
	for _, x := range offsets {
		w.u32(x)
	}
	name := func(s *Sym) string {
		if m.Writer.TrimInlineHash {
			return TrimInlineHash(s.Name)
		}
		return s.Name
	}
	builtin := int32(o.PkgIdx["Builtin"])

	// Strings: "", the package list (just the invalid package 0), every
	// symbol name but builtins', then the files.
	w.addString("")
	l.traverse(travAll, func(s *Sym) {
		if s.pkgIdx == builtin {
			return
		}
		w.addString(name(s))
	})
	for _, f := range l.files {
		w.addString(f)
	}

	mark := func(b string) { offsets[blk[b]] = w.off() }
	mark("Autolib")
	mark("PkgIdx")
	w.stringRef("")
	mark("File")
	for _, f := range l.files {
		w.stringRef(f)
	}
	sym := func(s *Sym) error { return l.writeSym(w, s, name(s)) }
	for _, g := range []struct {
		blk  string
		syms []*Sym
	}{{"Symdef", l.defs}, {"Hashed64def", l.hashed64}, {"Hasheddef", l.hashed}, {"Nonpkgdef", l.nonpkgdefs}, {"Nonpkgref", l.nonpkgrefs}} {
		mark(g.blk)
		for _, s := range g.syms {
			if err := sym(s); err != nil {
				return err
			}
		}
	}
	mark("RefFlags") // no referenced symbol is UsedInIface
	mark("Hash64")
	for _, s := range l.hashed64 {
		var h [8]byte
		copy(h[:], s.P)
		w.b.Write(h[:o.Hash64Size])
	}
	mark("Hash")
	for _, s := range l.hashed {
		h, err := l.contentHash(s)
		if err != nil {
			return err
		}
		w.b.Write(h)
	}
	lists := [][]*Sym{l.defs, l.hashed64, l.hashed, l.nonpkgdefs}
	mark("RelocIdx")
	n := uint32(0)
	for _, list := range lists {
		for _, s := range list {
			w.u32(n)
			n += uint32(len(s.R))
		}
	}
	w.u32(n)
	mark("AuxIdx")
	n = 0
	for _, list := range lists {
		for _, s := range list {
			w.u32(n)
			n += uint32(len(l.aux(s)))
		}
	}
	w.u32(n)
	mark("DataIdx")
	n = 0
	for _, list := range lists {
		for _, s := range list {
			w.u32(n)
			n += uint32(len(s.P))
		}
	}
	w.u32(n)
	mark("Reloc")
	for _, list := range lists {
		for _, s := range list {
			sort.SliceStable(s.R, func(i, j int) bool { return s.R[i].Off < s.R[j].Off })
			for _, r := range s.R {
				t, ok := m.Relocs[r.Type]
				if !ok {
					return fmt.Errorf("%s has no relocation type %s", m.Go, r.Type)
				}
				w.u32(uint32(r.Off))
				w.u8(r.Siz)
				w.u16(uint16(t))
				w.u64(uint64(r.Add))
				w.ref(r.Sym)
			}
		}
	}
	mark("Aux")
	for _, list := range lists {
		for _, s := range list {
			for _, a := range l.aux(s) {
				w.u8(uint8(a.typ))
				w.ref(a.sym)
			}
		}
	}
	mark("Data")
	for _, list := range lists {
		for _, s := range list {
			w.b.Write(s.P)
		}
	}
	mark("RefName") // assembler objects reference no other package by index
	mark("End")

	all := w.b.Bytes()
	at := len(o.Magic) + 8 + 4
	for _, x := range offsets {
		binary.LittleEndian.PutUint32(all[at:], x)
		at += 4
	}
	if _, err := io.WriteString(out, header+"!\n"); err != nil {
		return err
	}
	_, err := out.Write(all)
	return err
}

type auxEnt struct {
	typ int
	sym *Sym
}

func (l *Link) aux(s *Sym) []auxEnt {
	f := s.Func
	if f == nil {
		return nil
	}
	at := l.Meta.Object.AuxTypes
	var out []auxEnt
	add := func(t string, sym *Sym) { out = append(out, auxEnt{at[t], sym}) }
	add("FuncInfo", f.funcInfoSym)
	for _, d := range f.Funcdata {
		add("Funcdata", d)
	}
	nz := func(t string, a *Sym) {
		if a != nil && a.Size != 0 {
			add(t, a)
		}
	}
	nz("DwarfInfo", f.dwarfInfo)
	nz("DwarfRanges", f.dwarfRanges)
	nz("DwarfLines", f.dwarfLines)
	nz("Pcsp", f.Pcsp)
	nz("Pcfile", f.Pcfile)
	nz("Pcline", f.Pcline)
	nz("Pcinline", f.Pcinline)
	for _, p := range f.Pcdata {
		add("Pcdata", p)
	}
	return out
}

func (l *Link) writeSym(w *writer, s *Sym, name string) error {
	m := l.Meta
	o := &m.Object
	abi := uint16(s.ABI)
	if s.Static {
		abi = uint16(o.SymABIStatic)
	}
	var flag, flag2 int
	if s.Dupok {
		flag |= o.SymFlags["Dupok"]
	}
	if s.Local {
		flag |= o.SymFlags["Local"]
	}
	if s.NoSplit {
		flag |= o.SymFlags["NoSplit"]
	}
	if strings.HasPrefix(s.Name, "type:") && len(s.Name) > 5 && s.Name[5] != '.' && s.Kind == "SRODATA" {
		flag |= o.SymFlags["GoType"]
	}
	if strings.HasPrefix(s.Name, "go:itab.") && s.Kind == "SRODATA" {
		flag2 |= o.SymFlags2["Itab"]
	}
	if rest, ok := strings.CutPrefix(s.Name, l.Pkgpath+"."); ok && strings.HasPrefix(rest, ".dict") {
		flag2 |= o.SymFlags2["Dict"]
	}
	if (m.Writer.AsmNamedSymsLinkname && s.Name != "") || s.Name == "main.main" {
		flag2 |= o.SymFlags2["Linkname"]
	}
	var align uint32
	switch m.Writer.Align {
	case "explicit":
		align = uint32(s.Align)
	default:
		if s.ContentAddressable && s.Size != 0 {
			switch {
			case s.Size%8 == 0 && l.Arch.PtrSize == 8:
				align = 8
			case s.Size%4 == 0:
				align = 4
			case s.Size%2 == 0:
				align = 2
			default:
				align = 1
			}
			if name != "" {
				return fmt.Errorf("named content-addressable symbol %q: alignment rule not modelled", name)
			}
		}
	}
	kind, ok := m.Kinds[kindName(s.Kind)]
	if !ok {
		return fmt.Errorf("%s has no symbol kind %s", m.Go, s.Kind)
	}
	w.stringRef(name)
	w.u16(abi)
	w.u8(uint8(kind))
	w.u8(uint8(flag))
	w.u8(uint8(flag2))
	w.u32(uint32(s.Size))
	w.u32(align)
	return nil
}

func kindName(k string) string {
	if k == "" {
		return "Sxxx"
	}
	return k
}

// contentHash is cmd/internal/obj's content hash of a symbol without
// relocations.
func (l *Link) contentHash(s *Sym) ([]byte, error) {
	if len(s.R) != 0 {
		return nil, fmt.Errorf("content-addressable symbol with relocations")
	}
	sec, err := l.hashSection(s)
	if err != nil {
		return nil, err
	}
	var in []byte
	in = binary.LittleEndian.AppendUint64(in, uint64(s.Size))
	in = append(in, sec)
	in = append(in, bytes.TrimRight(s.P, "\x00")...)
	var sum [32]byte
	switch l.Meta.Writer.ContentHash {
	case "notsha256":
		sum = sha256.Sum256(in)
		for i := range sum {
			sum[i] = ^sum[i]
		}
	case "sha256-prefix01":
		sum = sha256.Sum256(append([]byte{1}, in...))
	default:
		return nil, fmt.Errorf("unknown content hash %q", l.Meta.Writer.ContentHash)
	}
	return sum[:l.Meta.Object.HashSize], nil
}

// TrimInlineHash is cmd/internal/obj's: it drops a #<base64 of 8 bytes>#
// inline marker from a name.
func TrimInlineHash(name string) string {
	a, b, ok := strings.Cut(name, "#")
	if !ok {
		return name
	}
	n := base64.StdEncoding.EncodedLen(8)
	if len(b) < n+1 || b[n] != '#' {
		return name
	}
	return a + b[n+1:]
}
