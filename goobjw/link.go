// Package goobjw writes Go object files (the "\x00go120ld" goobj format)
// without any of the Go toolchain's code. Everything that differs between
// Go releases comes from a gometa.Meta; the symbol model, numbering and
// block layout follow cmd/internal/obj's assembler path, which is what
// gocobj2goobj used to drive (docs/multi-go-version.md lists the
// functions it mirrors; tools/golangc-metadata-resolver checks they did
// not change).
package goobjw

import (
	"encoding/binary"
	"fmt"
	"sort"
	"strings"

	"github.com/LoyieKing/goc/gometa"
)

// ABI is a symbol's calling convention.
type ABI uint16

const (
	ABI0        ABI = 0
	ABIInternal ABI = 1
)

// Reloc is a relocation. Type is a relocation name (objabi's R_*); its
// number comes from the metadata when the object is written.
type Reloc struct {
	Off  int32
	Siz  uint8
	Type string
	Add  int64
	Sym  *Sym
}

// Sym is a symbol, defined or referenced.
type Sym struct {
	Name string
	ABI  ABI
	Kind string // objabi SymKind name; "" is Sxxx
	P    []byte
	Size int64
	R    []Reloc

	Dupok              bool
	NoSplit            bool
	Local              bool
	Static             bool
	ContentAddressable bool
	Pcdata             bool  // a pc-value table
	Align              int16 // written as is under the "explicit" align model

	Func *FuncInfo

	onList  bool
	indexed bool
	pkgIdx  int32
	symIdx  int32
}

// FuncInfo is a TEXT symbol's function metadata.
type FuncInfo struct {
	Args, Locals int32
	FuncID       uint8
	FuncFlag     uint8
	StartLine    int32

	Pcsp, Pcfile, Pcline, Pcinline *Sym
	Pcdata                         []*Sym
	Funcdata                       []*Sym
	UsedFiles                      map[uint32]bool
	InlTree                        []InlNode

	funcdata    map[int]*Sym // FUNCDATA "instructions", before Flush
	pcdataProg  bool
	dwarfInfo   *Sym
	dwarfLines  *Sym
	dwarfRanges *Sym
	funcInfoSym *Sym
}

// InlNode is one inlining tree entry. File is a file table index.
type InlNode struct {
	Parent   int32
	File     uint32
	Line     int32
	Func     *Sym
	ParentPC int32
}

// Link is one object under construction.
type Link struct {
	Meta    *gometa.Meta
	Arch    gometa.Arch
	Pkgpath string
	// DwTextCount is the number of TEXT symbols, an estimate the DWARF5
	// text address index width is chosen from (go1.25+).
	DwTextCount int
	// Dwarf5 selects DWARF 5 forms where the release has them (the
	// dwarf5 experiment; HeaderExperiments tells).
	Dwarf5 bool
	// RegabiWrappers: builtin references must match the builtin's ABI.
	RegabiWrappers bool

	hash, funchash map[string]*Sym
	Text, Data     []*Sym
	files          []string
	fileIdx        map[string]int
	flushed        bool

	defs, hashed64, hashed, nonpkgdefs, nonpkgrefs []*Sym
}

// NewLink starts an object for package pkgpath on arch.
func NewLink(m *gometa.Meta, arch, pkgpath string) (*Link, error) {
	a, ok := m.Arch[arch]
	if !ok {
		return nil, fmt.Errorf("no %s facts for GOARCH=%s", m.Go, arch)
	}
	if pkgpath == "" {
		return nil, fmt.Errorf("empty package path")
	}
	return &Link{Meta: m, Arch: a, Pkgpath: pkgpath, RegabiWrappers: true,
		hash: map[string]*Sym{}, funchash: map[string]*Sym{}, fileIdx: map[string]int{}}, nil
}

// Lookup returns the ABI0 symbol name, creating it.
func (l *Link) Lookup(name string) *Sym { return l.LookupABI(name, ABI0) }

// LookupABI returns the symbol name with the given ABI, creating it.
func (l *Link) LookupABI(name string, abi ABI) *Sym {
	h := l.hash
	if abi == ABIInternal {
		h = l.funchash
	}
	s := h[name]
	if s == nil {
		s = &Sym{Name: name, ABI: abi}
		h[name] = s
	}
	return s
}

// AddFile registers a source file name and returns its file table index.
func (l *Link) AddFile(name string) int {
	if i, ok := l.fileIdx[name]; ok {
		return i
	}
	i := len(l.files)
	l.fileIdx[name] = i
	l.files = append(l.files, name)
	return i
}

// Files is the file table.
func (l *Link) Files() []string { return l.files }

// Globl flags.
const (
	DUPOK  = 1 << 0
	RODATA = 1 << 1
	NOPTR  = 1 << 2
)

// Globl declares a data symbol of the given size (obj.(*Link).GloblPos).
func (l *Link) Globl(s *Sym, size int64, flag int) error {
	if s.onList {
		return fmt.Errorf("symbol %s redeclared", s.Name)
	}
	s.onList = true
	l.Data = append(l.Data, s)
	s.Size = size
	if s.Kind == "" {
		s.Kind = "SBSS"
	}
	if flag&DUPOK != 0 {
		s.Dupok = true
	}
	switch {
	case flag&RODATA != 0:
		s.Kind = "SRODATA"
	case flag&NOPTR != 0:
		if s.Kind == "SDATA" {
			s.Kind = "SNOPTRDATA"
		} else {
			s.Kind = "SNOPTRBSS"
		}
	}
	return nil
}

func (s *Sym) prepwrite(off int64, siz int) {
	switch s.Kind {
	case "", "SBSS":
		s.Kind = "SDATA"
	case "SNOPTRBSS":
		s.Kind = "SNOPTRDATA"
	}
	n := off + int64(siz)
	if int64(len(s.P)) < n {
		s.P = append(s.P, make([]byte, n-int64(len(s.P)))...)
	}
	if n > s.Size {
		s.Size = n
	}
}

// WriteInt stores a little-endian integer of siz bytes at off.
func (s *Sym) WriteInt(off int64, siz int, v int64) {
	s.prepwrite(off, siz)
	switch siz {
	case 1:
		s.P[off] = byte(v)
	case 2:
		binary.LittleEndian.PutUint16(s.P[off:], uint16(v))
	case 4:
		binary.LittleEndian.PutUint32(s.P[off:], uint32(v))
	case 8:
		binary.LittleEndian.PutUint64(s.P[off:], uint64(v))
	default:
		panic(fmt.Sprintf("WriteInt: bad size %d", siz))
	}
}

// WriteBytes stores b at off.
func (s *Sym) WriteBytes(off int64, b []byte) {
	if len(b) == 0 {
		return
	}
	s.prepwrite(off, len(b))
	copy(s.P[off:], b)
}

// AddRel appends a relocation.
func (s *Sym) AddRel(r Reloc) { s.R = append(s.R, r) }

// Text flags.
const (
	TextNoSplit = 1 << 0
	TextDupok   = 1 << 1
)

// InitText makes s a TEXT symbol (obj.(*Link).InitTextSym with the
// assembler's flags) starting at line 1 of file 0.
func (l *Link) InitText(s *Sym, flag int) error {
	if s.Func != nil || s.onList {
		return fmt.Errorf("symbol %s redeclared", s.Name)
	}
	m := l.Meta
	s.Func = &FuncInfo{funcdata: map[int]*Sym{}, StartLine: 1}
	s.Func.FuncID = uint8(l.funcID(s.Name))
	s.Func.FuncFlag = uint8(m.Writer.FuncFlags["Asm"])
	s.onList = true
	s.Dupok = flag&TextDupok != 0
	s.NoSplit = flag&TextNoSplit != 0
	s.Kind = "STEXT"
	l.Text = append(l.Text, s)
	return nil
}

// funcID is objabi.GetFuncID for a non-wrapper.
func (l *Link) funcID(name string) int {
	if rest, ok := strings.CutPrefix(name, "runtime."); ok {
		if id, ok := l.Meta.Writer.RuntimeFuncIDs[rest]; ok {
			return id
		}
	}
	return l.Meta.Writer.FuncIDs["Normal"]
}

// Funcdata records a FUNCDATA of index idx pointing at sym.
func (l *Link) Funcdata(s *Sym, idx int, sym *Sym) { s.Func.funcdata[idx] = sym }

// Flush does what cmd/internal/obj's Flushplist does for the assembler's
// TEXT symbols when they hold only TEXT, FUNCDATA and PCDATA pseudo
// instructions at line 1 of file 0: the arguments-funcdata references of
// ABI0 functions, the function's funcdata slots, the (empty) inlining
// table, the used-files set and the DWARF entries, which describe the
// function as it is at this point (no code yet).
func (l *Link) Flush() error {
	if l.flushed {
		return fmt.Errorf("Flush called twice")
	}
	l.flushed = true
	m := l.Meta
	prefix := PathToPrefix(l.Pkgpath) + "."
	argMaps := m.ABI.Funcdata["ArgsPointerMaps"]
	argInfo := m.ABI.Funcdata["ArgInfo"]
	for _, s := range l.Text {
		fn := s.Func
		hasProgs := len(fn.funcdata) > 0 || fn.pcdataProg
		if m.Writer.AsmArgsStackmap && strings.HasPrefix(s.Name, prefix) && s.ABI == ABI0 &&
			s.Name != "runtime.addmoduledata" {
			if _, ok := fn.funcdata[argMaps]; !ok {
				fn.funcdata[argMaps] = l.derived(s, s.Name+".args_stackmap")
			}
			if _, ok := fn.funcdata[argInfo]; !ok {
				fn.funcdata[argInfo] = l.derived(s, fmt.Sprintf("%s.arginfo%d", s.Name, s.ABI))
			}
		}
		n := 0
		for i := range fn.funcdata {
			if i+1 > n {
				n = i + 1
			}
		}
		fn.Funcdata = make([]*Sym, n)
		for i, d := range fn.funcdata {
			fn.Funcdata[i] = d
		}
		// The pseudo-instructions after TEXT carry file 0; TEXT itself
		// does not count (pctofileline).
		fn.UsedFiles = map[uint32]bool{}
		if hasProgs {
			fn.UsedFiles[0] = true
		}
		// pctoinline over instructions that occupy no bytes.
		fn.Pcinline = l.pcsym([]byte{0, 0, 0})
		l.dwarf(s)
	}
	return nil
}

func (l *Link) derived(s *Sym, name string) *Sym {
	if s.Static {
		return nil // static TEXT symbols are not made by this package
	}
	return l.Lookup(name)
}

// PCDATA notes that s has PCDATA pseudo-instructions (with value -1).
func (l *Link) PCDATA(s *Sym) { s.Func.pcdataProg = true }

// Pctab returns a new pc-value table symbol holding p.
func (l *Link) Pctab(p []byte) *Sym { return l.pcsym(p) }

func (l *Link) pcsym(p []byte) *Sym {
	return &Sym{Kind: "SRODATA", ContentAddressable: true, Pcdata: true, Align: 1, P: p, Size: int64(len(p))}
}

// PathToPrefix is objabi.PathToPrefix.
func PathToPrefix(s string) string {
	slash := strings.LastIndex(s, "/")
	const hex = "0123456789abcdef"
	var b []byte
	for i := 0; i < len(s); i++ {
		c := s[i]
		if c <= ' ' || (c == '.' && i > slash) || c == '%' || c == '"' || c >= 0x7F {
			b = append(b, '%', hex[c>>4], hex[c&0xF])
		} else {
			b = append(b, c)
		}
	}
	return string(b)
}

func sortedFiles(m map[uint32]bool) []uint32 {
	out := make([]uint32, 0, len(m))
	for f := range m {
		out = append(out, f)
	}
	sort.Slice(out, func(i, j int) bool { return out[i] < out[j] })
	return out
}
