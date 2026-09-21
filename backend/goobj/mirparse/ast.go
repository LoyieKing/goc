// Package mirparse parses LLVM Machine IR text (YAML-ish MIR and MF dump)
// into a structured Module that can be converted for mirlower → goobj.
package mirparse

// Module is a multi-function MIR file.
type Module struct {
	Functions []*Function
	Source    string // path or label
}

// Function is one MachineFunction.
type Function struct {
	Name     string
	GoSym    string // optional goc.go_sym
	Frame    int32  // optional goc.frame (Go locals bytes)
	Args     int32
	Flags    string // e.g. "nosplit"
	Alignment int
	TracksLiveness bool
	StackObjects []StackObject
	Blocks   []*Block
	// Registers lists vreg classes when present (YAML registers:).
	VRegClass map[int]string
}

// StackObject is a frame index / %stack.N descriptor.
type StackObject struct {
	ID        int
	Size      int
	Align     int
	Offset    int64 // Go/SP offset if known (at location [SP+N] or fixedStack offset)
	IsFixed   bool
	Name      string // optional
}

// Block is one MBB.
type Block struct {
	ID            int
	Label         string // e.g. "entry" from bb.0.entry
	Successors    []int
	Predecessors  []int
	LiveIns       []string
	Instrs        []*Instr
}

// Instr is one MachineInstr line.
type Instr struct {
	Raw     string
	Defs    []Op // left of '='
	Opcode  string
	Args    []Op // explicit operands
	Traits  []string // killed, implicit-def, etc. trailing tokens we keep as strings
}

// OpKind classifies an operand.
type OpKind int

const (
	OpNone OpKind = iota
	OpPhysReg
	OpVReg
	OpImm
	OpMBB
	OpSymbol
	OpStack // %stack.N
	OpNoreg
	OpMem // decoded 5-tuple already folded into Mem fields; Kind still OpMem for composite
	OpCond // numeric or named condition code (for JCC)
	OpOther
)

// Op is one MIR operand.
type Op struct {
	Kind   OpKind
	Reg    string // phys without $; or ""
	VReg   int    // virtual register number
	VClass string // optional :gr64
	Imm    int64
	MBB    int
	MBBLabel string // optional named target
	Symbol string // without leading @ or &
	Stack  int    // %stack.N
	// Memory addressing (X86 5-tuple), used when this Op represents a mem ref
	// already folded by the converter, or when Kind==OpMem.
	MemBase  string // phys or empty; "$rsp" stripped to "rsp"
	MemScale int
	MemIndex string
	MemDisp  int64
	MemSeg   string // "fs", "gs", or ""
	MemSym   string // disp as symbol (&foo)
	Raw      string
}
