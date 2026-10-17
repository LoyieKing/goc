// Package gocobj reads, writes and checks .gocobj files: relocatable ELF
// objects with extra .goc.* sections. The format is in docs/design/gocobj-format.md.
// Nothing here depends on the Go version except the ABI profile table.
package gocobj

const (
	Magic        = "GOCOBJ\x00\x00"
	Major        = 1
	Minor        = 4
	HeaderSize   = 28
	SecHeader    = ".goc.header"
	SecStrtab    = ".goc.strtab"
	SecFiles     = ".goc.files"
	SecFuncs     = ".goc.funcs"
	SecRelocs    = ".goc.relocs"
	SecStackmaps = ".goc.stackmaps" // 1.3
	SecWrappers  = ".goc.wrappers"  // 1.3
	SecPCValue   = ".goc.pcvalue"
	SecGCMaps    = ".goc.gcmaps"
	SecData      = ".goc.data"
	SecInlTree   = ".goc.inltree" // 1.4
)

// Arch values in the header.
const (
	ArchAMD64 uint8 = 1
	ArchARM64 uint8 = 2
)

// Header flags.
const HeaderStackChecksInCode uint8 = 1 << 0

// ABI values.
const (
	ABI0        uint8 = 0
	ABIInternal uint8 = 1
)

// Func flags.
const (
	FuncNeedsStackCheck uint64 = 1 << iota
	FuncLeaf
	FuncNosplit
	FuncHasFramePointer
	FuncUnsafeEntry
	FuncInternal       // 1.1: internal linkage in the C source
	FuncHasSptr        // 1.1: the IR carries an sptr color
	FuncSysvUnmodelled // 1.1: SysvPtrRegs is not valid for this signature
)

// RelocKind is the Go-neutral relocation kind.
type RelocKind uint8

const (
	RCall RelocKind = iota + 1
	RCallIndirectMark
	RPCRel
	RAddrAbs
	RFuncAddr
	RDataAddr
	RTLSG
	RGOTPCRel
	RJmpTailcall
	RARM64AdrpAdd
	RARM64Call26
	rLast
)

var relocNames = [...]string{"", "CALL", "CALL_INDIRECT_MARK", "PCREL", "ADDR_ABS",
	"FUNC_ADDR", "DATA_ADDR", "TLS_G", "GOT_PCREL", "JMP_TAILCALL", "ARM64_ADRP_ADD", "ARM64_CALL26"}

func (k RelocKind) String() string {
	if k > 0 && k < rLast {
		return relocNames[k]
	}
	return "?"
}

// Reloc flags.
const (
	RelocSafepoint uint8 = 1 << 0
	RelocWeak      uint8 = 1 << 1
)

// PCKind is a pc-value table kind.
type PCKind uint8

const (
	PCSPDelta PCKind = iota + 1
	PCFile
	PCLine
	PCUnsafePoint
	PCInlIndex
	PCStackMapIndex
	pcLast
)

var pcNames = [...]string{"", "SP_DELTA", "FILE", "LINE", "UNSAFE_POINT", "INL_INDEX", "STACKMAP_INDEX"}

// lineKind reports the kinds whose rows come from debug locations.
func (k PCKind) lineKind() bool { return k == PCFile || k == PCLine || k == PCInlIndex }

func (k PCKind) String() string {
	if k > 0 && k < pcLast {
		return pcNames[k]
	}
	return "?"
}

// Data flags.
const (
	DataRodata uint64 = 1 << iota
	DataNoptr
	DataLocal
	DataDupok
)

type Header struct {
	Major, Minor uint16
	Arch         uint8
	Flags        uint8
	Profile      string
	Producer     string
}

type Func struct {
	Name        string
	TextSize    uint64
	FrameSize   uint64
	ArgSize     uint64
	ABI         uint8
	Kind        uint8
	Flags       uint64
	FuncID      uint8
	File        uint64
	StartLine   uint64
	SysvPtrRegs uint64    // 1.1: bit i = SysV integer argument register i holds a pointer
	SptrSlots   []uint64  // 1.2: frame-pointer-relative offsets of sptr-holding slots (BP-off)
	PCValue     []PCTable // decoded from the pcv record (nil if none)
	Inl         []InlNode // 1.4: decoded from the inl record (nil if none)
	GC          *GCMap    // decoded from the gc record (nil if none)
	pcvOff      uint64
	gcOff       uint64
	inlOff      uint64
}

// InlNode is one inlined call in a function (1.4). Func is the inlined C
// function; File and Line are the call site in the parent. ParentPC is an
// offset whose FILE/LINE/INL_INDEX rows are that call site.
type InlNode struct {
	Parent    int64 // index of the enclosing call, -1 for the function itself
	File      uint64
	Line      uint64
	Func      string
	ParentPC  uint64
	StartLine uint64
}

// Stackmap is one LLVM STACKMAP site (1.3), with locations encoded as in
// .llvm_stackmaps version 3 (type 1 Register, 2 Direct, 3 Indirect,
// 4 Constant; reg is a DWARF register number).
type Stackmap struct {
	Func    string
	InsnOff uint64
	ID      uint64
	Locs    []StackmapLoc
}

type StackmapLoc struct {
	Type uint8
	Size uint64
	Reg  uint64
	Off  int64
}

// Wrapper is an ABI wrapper (1.3): the function Sym, listed in .goc.funcs
// with kind 1, takes the FromABI convention and calls Target in ToABI.
type Wrapper struct {
	Sym, Target    string
	FromABI, ToABI uint8 // 0 ABI0, 1 ABIInternal, 2 C (SysV)
	ArgSpills      []ArgSpill
	StackPtrArgs   []uint64 // offsets of pointer arguments passed on the stack
}

// ArgSpill is where the wrapper's morestack path spills a register argument.
type ArgSpill struct {
	Reg       string
	Off, Size uint64
	Ptr       bool
}

type Reloc struct {
	Func   string
	Offset uint64
	Width  uint8
	Kind   RelocKind
	Target string
	Addend int64
	Flags  uint8
}

type PCEntry struct {
	PC    uint64
	Value int64
}

type PCTable struct {
	Kind    PCKind
	Entries []PCEntry
}

type GCMap struct {
	NArgs, NLocals uint64
	Safepoints     []Safepoint
}

type Safepoint struct {
	PC     uint64
	Args   []byte
	Locals []byte
	Color  []byte
}

type Data struct {
	Name      string
	Size      uint64
	Log2Align uint8
	Flags     uint64
	NBits     uint64
	Bitmap    []byte
}

// Symbol is the part of an ELF symbol the checker needs.
type Symbol struct {
	Name    string
	Func    bool
	Defined bool
	Size    uint64
	Section string
	Value   uint64
	Count   int // how many ELF symbols share this name
}

// File is a parsed .gocobj.
type File struct {
	Header    Header
	Files     []string
	Funcs     []Func
	Relocs    []Reloc
	Stackmaps []Stackmap // 1.3
	Wrappers  []Wrapper  // 1.3
	Data      []Data
	Symbols   map[string]*Symbol
	// Text returns a function's machine code (nil if unknown).
	Text map[string][]byte
	// ELFRelocs maps "function+offset" to the target of the ELF relocation
	// there (only R_X86_64_PLT32/PC32 in text), for cross-checking.
	ELFRelocs map[string]string
	// record-start offsets, for checking pcv/gc references
	pcvStarts, gcStarts map[uint64]bool
}
