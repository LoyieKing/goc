package goobjw

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"strings"

	"github.com/LoyieKing/goc/gometa"
)

// Reader decodes a Go object file (the part after the text header) for
// tools: gocobj-diff and the tests.
type Reader struct {
	Meta   *gometa.Meta
	Header string // the "go object ..." line
	b      []byte // from the magic on
	off    map[string]uint32
	nsym   [5]int // Symdef, Hashed64def, Hasheddef, Nonpkgdef, Nonpkgref
}

// RSym is a symbol record.
type RSym struct {
	Name        string
	ABI         uint16
	Type        uint8
	Flag, Flag2 uint8
	Siz, Align  uint32
}

// SymRef is a symbol reference.
type SymRef struct{ PkgIdx, SymIdx uint32 }

// RReloc is a relocation record.
type RReloc struct {
	Off  int32
	Siz  uint8
	Type uint16
	Add  int64
	Sym  SymRef
}

// RAux is an aux symbol record.
type RAux struct {
	Type uint8
	Sym  SymRef
}

const symSize, relocSize, auxSize = 21, 23, 9

// NewReader decodes an object file. The facts of the release named in its
// header are used unless m is given.
func NewReader(file []byte, m *gometa.Meta) (*Reader, error) {
	nl := bytes.IndexByte(file, '\n')
	if nl < 0 || !bytes.HasPrefix(file, []byte("go object ")) {
		return nil, fmt.Errorf("not a Go object file")
	}
	hdr := string(file[:nl])
	if m == nil {
		f := strings.Fields(hdr)
		if len(f) < 5 {
			return nil, fmt.Errorf("bad object header %q", hdr)
		}
		var err error
		if m, err = gometa.ForVersion(f[4]); err != nil {
			return nil, err
		}
	}
	i := bytes.Index(file, []byte(m.Object.Magic))
	if i < 0 {
		return nil, fmt.Errorf("no %q magic", m.Object.Magic)
	}
	r := &Reader{Meta: m, Header: hdr, b: file[i:], off: map[string]uint32{}}
	at := len(m.Object.Magic) + 8 + 4
	if len(r.b) < at+4*len(m.Object.Blocks) {
		return nil, fmt.Errorf("truncated object header")
	}
	for _, name := range m.Object.Blocks {
		r.off[name] = binary.LittleEndian.Uint32(r.b[at:])
		at += 4
	}
	for k, blk := range []string{"Symdef", "Hashed64def", "Hasheddef", "Nonpkgdef", "Nonpkgref"} {
		next := []string{"Hashed64def", "Hasheddef", "Nonpkgdef", "Nonpkgref", "RefFlags"}[k]
		r.nsym[k] = int(r.off[next]-r.off[blk]) / symSize
	}
	return r, nil
}

func (r *Reader) u32(off uint32) uint32 { return binary.LittleEndian.Uint32(r.b[off:]) }

func (r *Reader) str(off uint32) string {
	n, o := r.u32(off), r.u32(off+4)
	return string(r.b[o : o+n])
}

// Flags is the header's object flags.
func (r *Reader) Flags() uint32 { return r.u32(uint32(len(r.Meta.Object.Magic) + 8)) }

func (r *Reader) NSym() int         { return r.nsym[0] }
func (r *Reader) NHashed64def() int { return r.nsym[1] }
func (r *Reader) NHasheddef() int   { return r.nsym[2] }
func (r *Reader) NNonpkgdef() int   { return r.nsym[3] }
func (r *Reader) NNonpkgref() int   { return r.nsym[4] }

// NDef is the number of defined symbols (everything but Nonpkgref).
func (r *Reader) NDef() int { return r.nsym[0] + r.nsym[1] + r.nsym[2] + r.nsym[3] }

// Sym returns symbol i, counting through all the symbol blocks in order.
func (r *Reader) Sym(i uint32) RSym {
	o := r.off["Symdef"] + i*symSize
	return RSym{Name: r.str(o), ABI: binary.LittleEndian.Uint16(r.b[o+8:]), Type: r.b[o+10],
		Flag: r.b[o+11], Flag2: r.b[o+12], Siz: r.u32(o + 13), Align: r.u32(o + 17)}
}

// NPkg is the number of referenced packages (index 0 is invalid).
func (r *Reader) NPkg() int { return int(r.off["File"]-r.off["PkgIdx"]) / 8 }

// Pkg returns referenced package i.
func (r *Reader) Pkg(i int) string { return r.str(r.off["PkgIdx"] + uint32(i)*8) }

// Files is the file table.
func (r *Reader) Files() []string {
	var out []string
	for o := r.off["File"]; o < r.off["Symdef"]; o += 8 {
		out = append(out, r.str(o))
	}
	return out
}

func (r *Reader) span(blk string, i uint32) (uint32, uint32) {
	o := r.off[blk] + 4*i
	return r.u32(o), r.u32(o + 4)
}

// Relocs returns defined symbol i's relocations.
func (r *Reader) Relocs(i uint32) []RReloc {
	a, b := r.span("RelocIdx", i)
	var out []RReloc
	for k := a; k < b; k++ {
		o := r.off["Reloc"] + k*relocSize
		out = append(out, RReloc{Off: int32(r.u32(o)), Siz: r.b[o+4], Type: binary.LittleEndian.Uint16(r.b[o+5:]),
			Add: int64(binary.LittleEndian.Uint64(r.b[o+7:])), Sym: SymRef{r.u32(o + 15), r.u32(o + 19)}})
	}
	return out
}

// Auxs returns defined symbol i's aux symbols.
func (r *Reader) Auxs(i uint32) []RAux {
	a, b := r.span("AuxIdx", i)
	var out []RAux
	for k := a; k < b; k++ {
		o := r.off["Aux"] + k*auxSize
		out = append(out, RAux{Type: r.b[o], Sym: SymRef{r.u32(o + 1), r.u32(o + 5)}})
	}
	return out
}

// Data returns defined symbol i's contents.
func (r *Reader) Data(i uint32) []byte {
	a, b := r.span("DataIdx", i)
	base := r.off["Data"]
	return r.b[base+a : base+b]
}

// BuiltinName returns builtin i's name and ABI.
func (r *Reader) BuiltinName(i int) (string, int) {
	if i < 0 || i >= len(r.Meta.Object.Builtins) {
		return fmt.Sprintf("<builtin %d>", i), -1
	}
	b := r.Meta.Object.Builtins[i]
	return b.Name, b.ABI
}

// FuncInfo is a decoded FuncInfo symbol.
type DecodedFuncInfo struct {
	Args, Locals     uint32
	FuncID, FuncFlag uint8
	StartLine        int32
	File             []uint32
	InlTree          []RInlNode
}

// RInlNode is a decoded inlining tree node.
type RInlNode struct {
	Parent, File, Line int32
	Func               SymRef
	ParentPC           int32
}

// DecodeFuncInfo decodes a FuncInfo symbol's contents.
func DecodeFuncInfo(b []byte) (DecodedFuncInfo, error) {
	var fi DecodedFuncInfo
	if len(b) < 20 {
		return fi, fmt.Errorf("short FuncInfo")
	}
	u := func(o int) uint32 { return binary.LittleEndian.Uint32(b[o:]) }
	fi.Args, fi.Locals, fi.FuncID, fi.FuncFlag, fi.StartLine = u(0), u(4), b[8], b[9], int32(u(12))
	n := int(u(16))
	o := 20
	if len(b) < o+4*n+4 {
		return fi, fmt.Errorf("short FuncInfo")
	}
	for k := 0; k < n; k++ {
		fi.File = append(fi.File, u(o))
		o += 4
	}
	ni := int(u(o))
	o += 4
	if len(b) < o+24*ni {
		return fi, fmt.Errorf("short FuncInfo")
	}
	for k := 0; k < ni; k++ {
		fi.InlTree = append(fi.InlTree, RInlNode{int32(u(o)), int32(u(o + 4)), int32(u(o + 8)),
			SymRef{u(o + 12), u(o + 16)}, int32(u(o + 20))})
		o += 24
	}
	return fi, nil
}
