// elfpack packs an LLVM llc-produced ELF .o into a Go goobj.
//
// Instruction bytes and ELF relocs come from LLVM MC (llc). This program only:
//   - renames MIR symbols to Go ABI names (meta.json)
//   - translates ELF relocs → goobj Reloc (R_CALL / R_PCREL) with addend check
//   - attaches FUNCDATA (Args/Locals pointer maps) via a tiny Prog plist
//   - emits dense PCDATA_StackMapIndex at each CALL safepoint (from meta.calls)
//   - re-relocates the .text references llc baked itself: CALLs (meta.calls)
//     and the non-safepoint JMP/Jcc/LEA refs (meta.branches), kept apart
//   - rebuilds pcsp consistent with LLVM Go frame (PUSH BP + SUB $frame)
//
// Primary path for P12/P13. Does not use mirlower opcode switches for encoding.
package main

import (
	"bufio"
	"bytes"
	"debug/elf"
	"encoding/binary"
	"encoding/json"
	"flag"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"

	"goc.local/p5-machinepass-goobj/goobj/enc/cmd/objlib/bio"
	"goc.local/p5-machinepass-goobj/goobj/enc/cmd/objlib/obj"
	"goc.local/p5-machinepass-goobj/goobj/enc/cmd/objlib/obj/x86"
	"goc.local/p5-machinepass-goobj/goobj/enc/cmd/objlib/objabi"
	"goc.local/p5-machinepass-goobj/goobj/enc/cmd/objlib/src"
	"goc.local/p5-machinepass-goobj/goobj/enc/stdinternal/abi"
)

type metaFile struct {
	Functions []metaFn   `json:"functions"`
	Mircanon  *metaCanon `json:"mircanon"`
	TU        string     `json:"tu,omitempty"`  // translation unit tag (symbol uniqueness)
	ABI       string     `json:"abi,omitempty"` // ABIInternal (default) | ABI0
}

type metaCanon struct {
	Mode         string   `json:"mode"`
	Transforms   []string `json:"transforms"`
	CfgRewrite   bool     `json:"cfg_rewrite"`
	FrameInject  bool     `json:"frame_inject"`
	DialectStrip bool     `json:"dialect_strip"`
}

// metaCall is a CALL safepoint: the offset of the CALL opcode (when the sidecar
// records one) and the stack map index the runtime must see at that PC.
type metaCall struct {
	Callee        string `json:"callee"`
	Off           *int64 `json:"off,omitempty"` // function-relative offset of the CALL opcode
	StackmapIndex int    `json:"stackmap_index"`
}

// metaBranch is a code reference that is *not* a safepoint: a cross-function
// direct JMP (tail jump, goabi thunk) or function-address LEA. The ELF may
// already carry its relocation; otherwise the opcode offset lets elfpack
// restore the baked reference. Neither case may reach PCDATA_StackMapIndex.
type metaBranch struct {
	Callee string `json:"callee"`
	Off    *int64 `json:"off,omitempty"` // function-relative offset of the JMP/LEA opcode
}

// ABIInternal register arguments have caller-allocated spill slots above the
// entry return address. The slow morestack path must save/reload them there;
// morestack itself clobbers argument registers before retrying the function.
type metaArgSpill struct {
	Reg  string `json:"reg"`
	Off  int    `json:"off"`  // positive offset from the thunk's entry SP
	Size int    `json:"size"` // 1, 2, 4 or 8 bytes
	Ptr  bool   `json:"ptr"`
}

// offset reports the recorded opcode offset, if the sidecar carries one. Sidecars
// from the llvmmc fixture path name their call sites but record no offsets: they
// have no baked reference to fix and fall back to name-paired stack maps.
func (c metaCall) offset() (int64, bool) {
	if c.Off == nil {
		return 0, false
	}
	return *c.Off, true
}

func (b metaBranch) offset() (int64, bool) {
	if b.Off == nil {
		return 0, false
	}
	return *b.Off, true
}

type metaFn struct {
	MIRName         string         `json:"mir_name"`
	GoSym           string         `json:"go_sym"`
	Frame           int            `json:"frame"`
	HasSptr         *bool          `json:"has_sptr"`
	CStackAlign     bool           `json:"cstack_align"`
	SptrSlots       []int          `json:"sptr_slots"`
	SysvPointerRegs []string       `json:"sysv_pointer_regs"`
	Flags           string         `json:"flags"`
	Encoding        string         `json:"encoding"`
	Calls           []metaCall     `json:"calls"`
	Branches        []metaBranch   `json:"branches"`
	ArgSpills       []metaArgSpill `json:"go_arg_spills"`
	GoArgArea       int            `json:"go_arg_area"`
	GoStackPtrArgs  []int          `json:"go_stack_ptr_args"`
	// P21 optional: color-vertical per-fn maps (additive; P16 ignores these).
	ColorFP    string `json:"color_fp,omitempty"`
	MapsSubdir string `json:"maps_subdir,omitempty"`
	ArgsMap    string `json:"args_map,omitempty"`
	LocalsMap  string `json:"locals_map,omitempty"`
	ABI        string `json:"abi,omitempty"` // ABIInternal | ABI0 (per function)
}

type textReq struct {
	mirName         string
	goSym           string
	frame           int
	flags           int
	cstackAlign     bool
	wantSptrMap     bool
	bodyUnproved    bool
	sptrSlots       []int
	sysvPointerRegs []string
	stubMapIndex    int32
	localsBlob      []byte
	smapTrans       []pcValue
	argsMap         string
	localsMap       string
	abi             obj.ABI
	calls           []metaCall
	branches        []metaBranch
	argSpills       []metaArgSpill
	goArgArea       int
	argsBlob        []byte
}

// --- Stage B: LLVM stack maps ------------------------------------------------
//
// GocStackMap.cpp passes the addresses of volatile root allocas at call sites.
// LLVM Direct (RBP+off) names that alloca, whose contents hold the pointer.
// An Indirect operand would instead be a spill of the alloca's address, not
// the pointer it contains; do not mistake it for an extra pointer root.

type smapLoc struct {
	typ  uint8
	reg  uint16
	off  int32
	size uint16
}

type smapRecord struct {
	id      uint64
	insnOff uint32
	locs    []smapLoc
}

func alignUp8(n int) int { return (n + 7) &^ 7 }

// parseLLVMStackMaps reads the section and attributes records to functions via
// .rela.llvm_stackmaps (each function record's address field is relocated to the
// function symbol).
func parseLLVMStackMaps(ef *elf.File) map[string][]smapRecord {
	sec := ef.Section(".llvm_stackmaps")
	if sec == nil {
		return nil
	}
	b, err := sec.Data()
	if err != nil || len(b) < 16 || b[0] != 3 {
		fmt.Fprintf(os.Stderr, "elfpack: FATAL invalid .llvm_stackmaps (err=%v len=%d)\n", err, len(b))
		os.Exit(1)
	}
	u32 := func(off int) uint32 { return binary.LittleEndian.Uint32(b[off:]) }
	u64 := func(off int) uint64 { return binary.LittleEndian.Uint64(b[off:]) }
	nfun, nconst, nrec := int(u32(4)), int(u32(8)), int(u32(12))
	if nfun > (len(b)-16)/24 || nconst > (len(b)-16-nfun*24)/8 {
		fmt.Fprintf(os.Stderr, "elfpack: FATAL truncated .llvm_stackmaps header (functions=%d constants=%d len=%d)\n", nfun, nconst, len(b))
		os.Exit(1)
	}

	// function record index -> symbol name (via the section's relocations)
	nameByFunc := map[int]string{}
	syms, _ := ef.Symbols()
	if rs := ef.Section(".rela.llvm_stackmaps"); rs != nil {
		if rd, err := rs.Data(); err == nil {
			for j := 0; j+24 <= len(rd); j += 24 {
				off := int(binary.LittleEndian.Uint64(rd[j:]))
				si := int(binary.LittleEndian.Uint64(rd[j+8:]) >> 32)
				if off < 16 || (off-16)%24 != 0 || (off-16)/24 >= nfun || si <= 0 || si > len(syms) {
					continue
				}
				// debug/elf.Symbols omits the ELF null symbol (index zero).
				nameByFunc[(off-16)/24] = syms[si-1].Name
			}
		}
	}

	pos := 16
	counts := make([]int, nfun)
	sizes := make([]uint64, nfun)
	for i := 0; i < nfun; i++ {
		if pos+24 > len(b) {
			return nil
		}
		sizes[i] = u64(pos + 8)
		counts[i] = int(u64(pos + 16))
		pos += 24
	}
	pos += 8 * nconst
	out := map[string][]smapRecord{}
	total := 0
	for fi := 0; fi < nfun; fi++ {
		name := nameByFunc[fi]
		for k := 0; k < counts[fi] && total < nrec; k++ {
			if pos+16 > len(b) {
				return out
			}
			id := u64(pos)
			insnOff := u32(pos + 8)
			nloc := int(binary.LittleEndian.Uint16(b[pos+14:]))
			pos += 16
			rec := smapRecord{id: id, insnOff: insnOff}
			if rec.id>>40 != 0x474f43 {
				if useSptrMaps {
					fmt.Fprintf(os.Stderr, "elfpack: FATAL foreign LLVM stackmap id %#x in %s\n", rec.id, name)
					os.Exit(1)
				}
				name = "" // parse but do not interpret an unrelated map
			}
			for j := 0; j < nloc; j++ {
				if pos+12 > len(b) {
					return out
				}
				rec.locs = append(rec.locs, smapLoc{
					typ:  b[pos],
					size: binary.LittleEndian.Uint16(b[pos+2:]),
					reg:  binary.LittleEndian.Uint16(b[pos+4:]),
					off:  int32(binary.LittleEndian.Uint32(b[pos+8:])),
				})
				pos += 12
			}
			pos = alignUp8(pos)
			if pos+4 > len(b) {
				return out
			}
			nlo := int(binary.LittleEndian.Uint16(b[pos+2:]))
			pos += 4 + 4*nlo
			pos = alignUp8(pos)
			if name != "" {
				out[name] = append(out[name], rec)
			}
			total++
		}
	}
	return out
}

// makeConstPcsp builds a pctab that maps the entire function to a constant SP delta.
func makeConstPcsp(ctxt *obj.Link, size int64, spdelta int32) *obj.LSym {
	sym := &obj.LSym{
		Type:      objabi.SRODATA,
		Attribute: obj.AttrContentAddressable | obj.AttrPcdata,
	}
	if size <= 0 {
		return sym
	}
	buf := make([]byte, binary.MaxVarintLen64)
	var dst []byte
	n := binary.PutVarint(buf, int64(spdelta-(-1)))
	dst = append(dst, buf[:n]...)
	n = binary.PutUvarint(buf, uint64(size))
	dst = append(dst, buf[:n]...)
	dst = append(dst, 0)
	sym.P = dst
	sym.Size = int64(len(dst))
	return sym
}

// pcValue is a (pc, value) transition for a pctab. Value applies from that PC onward.
type pcValue struct {
	PC    int64
	Value int32
}

// makePcdataFromTransitions builds a Go pctab from sorted PC→value transitions.
// Runtime step(): start val=-1; each (valΔ,pcΔ) applies val then advances pc;
// value holds for PCs in [prevpc, pc). See runtime.step.
func makePcdataFromTransitions(ctxt *obj.Link, size int64, trans []pcValue) *obj.LSym {
	sym := &obj.LSym{
		Type:      objabi.SRODATA,
		Attribute: obj.AttrContentAddressable | obj.AttrPcdata,
	}
	if size <= 0 {
		return sym
	}
	sort.Slice(trans, func(i, j int) bool { return trans[i].PC < trans[j].PC })
	type seg struct {
		val   int32
		start int64
	}
	segs := []seg{{val: -1, start: 0}}
	for _, tr := range trans {
		if tr.PC < 0 || tr.PC >= size {
			continue
		}
		last := &segs[len(segs)-1]
		if tr.PC == last.start {
			last.val = tr.Value
			continue
		}
		if tr.Value == last.val {
			continue
		}
		segs = append(segs, seg{val: tr.Value, start: tr.PC})
	}
	buf := make([]byte, binary.MaxVarintLen64)
	var dst []byte
	prevVal := int32(-1)
	for i, s := range segs {
		end := size
		if i+1 < len(segs) {
			end = segs[i+1].start
		}
		if end <= s.start {
			continue
		}
		n := binary.PutVarint(buf, int64(s.val)-int64(prevVal))
		dst = append(dst, buf[:n]...)
		n = binary.PutUvarint(buf, uint64(end-s.start))
		dst = append(dst, buf[:n]...)
		prevVal = s.val
	}
	dst = append(dst, 0)
	sym.P = dst
	sym.Size = int64(len(dst))
	return sym
}

// ---------------------------------------------------------------------------
// Go-style stack split (morestack) support for LLVM-encoded TEXT.
//
// Gate: GOC_MORESTACK=1. Off by default, because a growth that copies a frame
// holding stack pointers still cannot adjust them (no real stack maps yet).
//
// goc TEXT bodies come from LLVM: the frame is materialised by the body itself
// (PUSH BP + SUB $frame) and its CALLs are injected as raw bytes, so
// x86.preprocess' leaf heuristic never sees them and marks every goc function
// NOSPLIT. That is what the linker enforces (StackNosplitBase ~= 792B), so any
// frame above that budget needs a real split check + morestack stub, exactly
// like Go's own prologue:
//
//	entry:  <check>                ; JBE stub — frame NOT allocated yet
//	        <LLVM body>            ; frame allocated here (PUSH BP + SUB)
//	stub:   CALL runtime.morestack_noctxt(SB)
//	        JMP  entry             ; re-run the check on the grown stack
//
// Invariant: R14 == g throughout goc-compiled code. Go callers enter through
// the ABIInternal thunk with R14 = g, and SysV code preserves R14 (callee
// saved), so the check may read 16(R14) directly and the stub may call
// morestack without setting g up. (A C caller from outside goc code — e.g. a
// libc callback — would violate this; such entries must stay nosplit.)
//
// The check and the stub run with SP at its entry value, so pcsp is 0 there:
// nothing is live when morestack copies the frame. The body keeps frame+8.
//
// This runs on the post-inline object. An inlined callee is not a TEXT symbol,
// so it has no prologue of its own; the caller's check covers the combined
// frame. A static function whose callers were all inlined is deleted before
// llc and never reaches this stage. A surviving function whose calls were
// inlined away, and whose frame fits StackSmall, is a leaf and skips the probe
// via gocLeafNosplit. Do not move this insertion into the IR: an earlier check
// would be copied into every caller by the inliner and would see the wrong SP.
// ---------------------------------------------------------------------------

// useMorestack enables the split prologue + morestack stub for goc TEXT whose
// frame exceeds the nosplit budget (see the block comment above).
var useMorestack = os.Getenv("GOC_MORESTACK") == "1"

// fixedG means llc was invoked with -reserve-goc-r14, so C bodies never
// allocate R14 and it still holds g at every goc entry. The split check can
// compare against 16(R14) instead of loading g from TLS.
var fixedG = os.Getenv("GOC_FIXED_G") == "1"

// gocLeafNosplit is an ABI0 function with no direct call whose frame fits in
// StackSmall. The caller's split check leaves StackGuard bytes, which covers
// this frame; a TLS probe on every entry is only a tax. The linker still
// charges the frame against the nosplit chain, so the function must be marked
// AttrNoSplit.
func gocLeafNosplit(rq textReq) bool {
	return rq.abi == obj.ABI0 && !rq.cstackAlign && rq.goArgArea == 0 &&
		len(rq.calls) == 0 && rq.frame > 0 && rq.frame <= int(abi.StackSmall)
}

func gocWantsSplit(rq textReq) bool {
	if !useMorestack || rq.flags&obj.NOSPLIT != 0 {
		return false
	}
	if rq.frame <= 0 && !rq.cstackAlign {
		return false
	}
	return !gocLeafNosplit(rq)
}

// useSptrMaps enables the real sptr frame maps (color-pass alloca slots + LLVM
// stack-map tagged root allocas). Off by default: still experimental.
var useSptrMaps = os.Getenv("GOC_SPTR_MAPS") == "1"

// LLVM may synthesize a libcall *after* the IR-level Go ABI rename pass, so
// that reference is bare even if the defining TU emitted <name>.impl. The QJS
// build supplies the eligible names from its shim object; without that explicit
// contract retain the original core integer-libcall binding.
var libcallImpl = func() map[string]bool {
	if names := os.Getenv("GOC_LIBCALL_IMPL"); names != "" {
		out := map[string]bool{}
		for _, name := range strings.Fields(names) {
			out[name] = true
		}
		return out
	}
	return map[string]bool{
		"memcpy": true, "memmove": true, "memset": true, "memcmp": true,
		"bcmp": true, "strlen": true, "strcpy": true, "strcmp": true,
		"vsnprintf": true, "snprintf": true,
	}
}()

// pcRelELF reports whether an ELF reloc type is PC-relative (its symbol-based
// addend carries the -4 convention that makes S+A-P land on the instruction end).
func pcRelELF(t uint32) bool {
	switch t {
	case uint32(elf.R_X86_64_PC32), uint32(elf.R_X86_64_PLT32), 9 /*GOTPCREL*/, 41 /*GOTPCRELX*/, 42 /*REX_GOTPCRELX*/ :
		return true
	}
	return false
}

// gocSplitCheck returns the entry check sequence plus the byte offsets of the
// rel32 fields of its branches to the stub. The stub is emitted after the body,
// so the caller patches them with patchSplitCheck once that offset is known.
func gocSplitCheck(frame int32, preserveArgs bool) ([]byte, []int) {
	var b []byte
	var branches []int
	spBias := int32(0)
	if preserveArgs {
		// ABIInternal may pass arguments in R10/R11. Save them before using
		// these registers as split-check scratch, and compare against the
		// original entry SP rather than the temporarily lowered SP.
		b = append(b, 0x41, 0x52, 0x41, 0x53) // PUSH R10; PUSH R11
		spBias = 16
	}
	// Load g from the Go TLS instead of trusting R14: in SysV, R14 is an ordinary
	// callee-saved register, so LLVM-compiled C bodies use it freely and the
	// "R14 == g" invariant does not hold between calls. The ABI0 morestack
	// (runtime.morestack_noctxt.abi0) loads g itself, so nothing here needs R14.
	// MOVQ FS:-8, R11 unless GOC_FIXED_G=1, in which case R14 is already g.
	gRegR14 := fixedG
	if !gRegR14 {
		b = append(b, 0x64, 0x4c, 0x8b, 0x1c, 0x25, 0xf8, 0xff, 0xff, 0xff)
	}
	cmpR10Disp8 := byte(0x53) // [R11+disp8]
	cmpSPDisp8 := byte(0x63)  // [R11+disp8], reg=SP
	if gRegR14 {
		cmpR10Disp8 = 0x56 // [R14+disp8]
		cmpSPDisp8 = 0x66
	}
	switch {
	case frame <= abi.StackSmall:
		if preserveArgs {
			// LEAQ 16(SP), R10; CMPQ R10, 16(R11)
			b = append(b, 0x4c, 0x8d, 0x54, 0x24, 0x10)
			b = append(b, 0x4d, 0x3b, cmpR10Disp8, 0x10)
		} else {
			// CMPQ SP, 16(R11) or 16(R14)
			b = append(b, 0x49, 0x3b, cmpSPDisp8, 0x10)
		}
	case frame <= abi.StackBig:
		// LEAQ bias-(frame-StackSmall)(SP), R10; CMPQ R10, 16(g)
		b = append(b, 0x4c, 0x8d, 0x94, 0x24)
		b = binary.LittleEndian.AppendUint32(b, uint32(spBias-(frame-abi.StackSmall)))
		b = append(b, 0x4d, 0x3b, cmpR10Disp8, 0x10)
	default:
		// MOVQ SP, R10 ; SUBQ $(frame-StackSmall), R10 ; JCS stub
		// CMPQ R10, 16(g)
		if preserveArgs {
			b = append(b, 0x4c, 0x8d, 0x54, 0x24, 0x10) // LEAQ 16(SP), R10
		} else {
			b = append(b, 0x49, 0x89, 0xe2) // MOVQ SP, R10
		}
		b = append(b, 0x49, 0x81, 0xea)
		b = binary.LittleEndian.AppendUint32(b, uint32(frame-abi.StackSmall))
		b = append(b, 0x0f, 0x82)
		branches = append(branches, len(b))
		b = append(b, 0, 0, 0, 0)
		b = append(b, 0x4d, 0x3b, cmpR10Disp8, 0x10)
	}
	// JBE stub  (SP <= stackguard0)
	b = append(b, 0x0f, 0x86, 0, 0, 0, 0)
	branches = append(branches, len(b)-4)
	if preserveArgs {
		b = append(b, 0x41, 0x5b, 0x41, 0x5a) // POP R11; POP R10
	}
	return b, branches
}

// patchSplitCheck resolves the check's forward branches to the stub offset.
func patchSplitCheck(b []byte, branches []int, stub int) {
	for _, off := range branches {
		binary.LittleEndian.PutUint32(b[off:], uint32(int32(stub-(off+4))))
	}
}

// gocArgSpillInsn encodes a scalar Go register argument to/from the caller's
// ABIInternal spill area. The slots lie above the return PC and are guaranteed
// by the Go calling convention, so the morestack stub does not change SP.
func gocArgSpillInsn(a metaArgSpill, load bool) []byte {
	if a.Off < 8 || a.Off > math.MaxInt32 || (a.Size != 1 && a.Size != 2 && a.Size != 4 && a.Size != 8) {
		fmt.Fprintf(os.Stderr, "elfpack: FATAL invalid Go argument spill %+v\n", a)
		os.Exit(1)
	}
	reg := -1
	float := strings.HasPrefix(a.Reg, "xmm")
	if float {
		n, err := strconv.Atoi(strings.TrimPrefix(a.Reg, "xmm"))
		if err == nil && n >= 0 && n < 15 && (a.Size == 4 || a.Size == 8) {
			reg = n
		}
	} else {
		switch a.Reg {
		case "rax":
			reg = 0
		case "rcx":
			reg = 1
		case "rbx":
			reg = 3
		case "rsi":
			reg = 6
		case "rdi":
			reg = 7
		case "r8":
			reg = 8
		case "r9":
			reg = 9
		case "r10":
			reg = 10
		case "r11":
			reg = 11
		}
	}
	if reg < 0 || (a.Ptr && (float || a.Size != 8)) {
		fmt.Fprintf(os.Stderr, "elfpack: FATAL unsupported Go argument spill %+v\n", a)
		os.Exit(1)
	}
	var b []byte
	if float {
		if a.Size == 4 {
			b = append(b, 0xf3)
		} else {
			b = append(b, 0xf2)
		}
	} else if a.Size == 2 {
		b = append(b, 0x66)
	}
	rex := byte(0x40)
	if !float && a.Size == 8 {
		rex |= 0x08
	}
	if reg >= 8 {
		rex |= 0x04
	}
	if rex != 0x40 || (!float && a.Size == 1 && reg >= 4) {
		b = append(b, rex)
	}
	if float {
		b = append(b, 0x0f)
		if load {
			b = append(b, 0x10)
		} else {
			b = append(b, 0x11)
		}
	} else if a.Size == 1 {
		if load {
			b = append(b, 0x8a)
		} else {
			b = append(b, 0x88)
		}
	} else {
		if load {
			b = append(b, 0x8b)
		} else {
			b = append(b, 0x89)
		}
	}
	b = append(b, 0x84|byte(reg&7)<<3, 0x24) // ModRM disp32(SP), SIB
	b = binary.LittleEndian.AppendUint32(b, uint32(a.Off))
	return b
}

// gocSplitStub returns the slow path plus the two PC-relative relocation
// fields. morestack clobbers every caller-saved register: save the ABIInternal
// arguments in their caller-allocated spill slots and reload after stack copy.
func gocSplitStub(preserveArgs bool, argSpills []metaArgSpill) (stub []byte, callRel, jmpRel int) {
	if preserveArgs {
		// Only the failed check reaches the stub, still holding both pushes.
		stub = append(stub, 0x41, 0x5b, 0x41, 0x5a) // POP R11; POP R10
	}
	for _, a := range argSpills {
		stub = append(stub, gocArgSpillInsn(a, false)...)
	}
	callRel = len(stub) + 1
	stub = append(stub, 0xe8, 0, 0, 0, 0) // CALL rel32
	for _, a := range argSpills {
		stub = append(stub, gocArgSpillInsn(a, true)...)
	}
	jmpRel = len(stub) + 1
	stub = append(stub, 0xe9, 0, 0, 0, 0) // JMP rel32
	return stub, callRel, jmpRel
}

// SysV arguments have no Go caller spill area. Save all argument GPRs and
// XMM0–7 around morestack, plus RBX/R12/R13/R15/R14. The check runs before
// the body prologue, so those SysV callee-saved regs are still the caller's.
// Go's newstack clobbers R12/R13/R15 and gogo forces R14 = g on resume.
// The split check reads g from TLS into R11, so R14 is free to hold a C
// value; the stub reloads g into R14 only for the newstack call, then pops
// the caller's R14. Saving BP first lets runtime recognize the caller's
// saved frame pointer at varp. Below it: AX, DI, SI, DX, CX, R8, R9, then
// BX, R12, R13, R15, R14. Do not mark the callee-saved slots: they often
// hold small integers, and a marked 0x40 is an invalid pointer.
// A stack pointer the caller kept in one of those registers is reloaded by
// goc-reanchor from an adjusted frame slot; the stub must not try to mark
// the register itself.
func gocSysvSplitStub() (stub []byte, callRel, jmpRel int, sp []pcValue) {
	pushes := [][]byte{{0x55}, {0x50}, {0x57}, {0x56}, {0x52}, {0x51}, {0x41, 0x50}, {0x41, 0x51}}
	// SysV callee-saved that Go code clobbers. R14 is last so it can be
	// reloaded from TLS after the push and restored by the first pop.
	saved := [][]byte{{0x53}, {0x41, 0x54}, {0x41, 0x55}, {0x41, 0x57}, {0x41, 0x56}}
	sp = append(sp, pcValue{PC: 0, Value: 0})
	delta := 0
	for _, insn := range pushes {
		stub = append(stub, insn...)
		delta += 8
		sp = append(sp, pcValue{PC: int64(len(stub)), Value: int32(delta)})
	}
	for _, insn := range saved {
		stub = append(stub, insn...)
		delta += 8
		sp = append(sp, pcValue{PC: int64(len(stub)), Value: int32(delta)})
	}
	stub = append(stub, 0x48, 0x81, 0xec, 0x80, 0, 0, 0) // SUBQ $128, SP
	delta += 128
	sp = append(sp, pcValue{PC: int64(len(stub)), Value: int32(delta)})
	for r := byte(0); r < 8; r++ {
		stub = append(stub, 0xf3, 0x0f, 0x7f) // MOVDQU XMMr, disp(SP)
		if r == 0 {
			stub = append(stub, 0x04, 0x24)
		} else {
			stub = append(stub, 0x44|(r<<3), 0x24, r*16)
		}
	}
	// newstack requires R14 == g. The caller's value is already on the stack.
	stub = append(stub, 0x64, 0x4c, 0x8b, 0x34, 0x25, 0xf8, 0xff, 0xff, 0xff) // MOVQ FS:-8, R14
	callRel = len(stub) + 1
	stub = append(stub, 0xe8, 0, 0, 0, 0) // CALL morestack
	for r := byte(0); r < 8; r++ {
		stub = append(stub, 0xf3, 0x0f, 0x6f) // MOVDQU disp(SP), XMMr
		if r == 0 {
			stub = append(stub, 0x04, 0x24)
		} else {
			stub = append(stub, 0x44|(r<<3), 0x24, r*16)
		}
	}
	stub = append(stub, 0x48, 0x81, 0xc4, 0x80, 0, 0, 0) // ADDQ $128, SP
	delta -= 128
	sp = append(sp, pcValue{PC: int64(len(stub)), Value: int32(delta)})
	popsSaved := [][]byte{{0x41, 0x5e}, {0x41, 0x5f}, {0x41, 0x5d}, {0x41, 0x5c}, {0x5b}}
	for _, insn := range popsSaved {
		stub = append(stub, insn...)
		delta -= 8
		sp = append(sp, pcValue{PC: int64(len(stub)), Value: int32(delta)})
	}
	pops := [][]byte{{0x41, 0x59}, {0x41, 0x58}, {0x59}, {0x5a}, {0x5e}, {0x5f}, {0x58}, {0x5d}}
	for _, insn := range pops {
		stub = append(stub, insn...)
		delta -= 8
		sp = append(sp, pcValue{PC: int64(len(stub)), Value: int32(delta)})
	}
	jmpRel = len(stub) + 1
	stub = append(stub, 0xe9, 0, 0, 0, 0) // JMP entry
	if delta != 0 {
		panic(fmt.Sprintf("sysv morestack stub SP delta %d, want 0", delta))
	}
	return stub, callRel, jmpRel, sp
}

var sysvStubCode, sysvStubCallRel, sysvStubJmpRel, sysvStubSP = gocSysvSplitStub()

// gocBodyTerminates reports whether the LLVM body cannot fall through into an
// appended stub: it must end in a return/jump/trap.
func gocBodyTerminates(code []byte) bool {
	if len(code) == 0 {
		return false
	}
	switch code[len(code)-1] {
	case 0xc3, 0xc2, 0xe9, 0xeb, 0xcc, 0x0b:
		return true
	}
	return false
}

func mustRead(path string) []byte {
	b, err := os.ReadFile(path)
	if err != nil {
		panic(err)
	}
	return b
}

func readOpt(path string) []byte {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil
	}
	return b
}

func emitStackmapROData(ctxt *obj.Link, name string, b []byte) {
	s := ctxt.Lookup(name)
	ctxt.GloblPos(s, int64(len(b)), obj.RODATA|obj.DUPOK, src.NoXPos)
	off := 0
	for off+4 <= len(b) {
		v := int64(binary.LittleEndian.Uint32(b[off : off+4]))
		s.WriteInt(ctxt, int64(off), 4, v)
		off += 4
	}
	for off < len(b) {
		s.WriteInt(ctxt, int64(off), 1, int64(b[off]))
		off++
	}
}

// bakedRef describes one code reference whose displacement llc resolved itself
// (references to .text symbols in the same object carry no ELF reloc): the
// opcode fixes the field width, and the class fixes the relocation kind the Go
// linker has to apply. isCall separates the safepoints (CALL) from the refs
// that can never grow the stack (JMP/Jcc/LEA).
type bakedRef struct {
	dispOff int64 // rel8/rel32 field, function-relative
	insnLen int64
	rel8    bool
	isCall  bool
}

// decodeBakedRef decodes the branch / address-materialization instruction that
// starts at off. Only the forms llc emits for intra-object .text references are
// accepted; anything else is reported by the caller, never guessed at.
func decodeBakedRef(code []byte, off int64) (bakedRef, bool) {
	if off < 0 || off >= int64(len(code)) {
		return bakedRef{}, false
	}
	n := int64(len(code))
	op := code[off]
	switch {
	case op == 0xe8: // CALL rel32
		return bakedRef{dispOff: off + 1, insnLen: 5, isCall: true}, true
	case op == 0xe9: // JMP rel32
		return bakedRef{dispOff: off + 1, insnLen: 5}, true
	case op == 0xeb || (op >= 0x70 && op <= 0x7f): // JMP/Jcc rel8
		return bakedRef{dispOff: off + 1, insnLen: 2, rel8: true}, true
	case op == 0x0f && off+1 < n && code[off+1]&0xf0 == 0x80: // Jcc rel32
		return bakedRef{dispOff: off + 2, insnLen: 6}, true
	case op == 0x8d && off+1 < n && code[off+1]&0xc7 == 0x05: // LEA r32, disp32(%rip)
		return bakedRef{dispOff: off + 2, insnLen: 6}, true
	case op >= 0x40 && op <= 0x4f && off+2 < n &&
		code[off+1] == 0x8d && code[off+2]&0xc7 == 0x05: // REX + LEA r64, disp32(%rip)
		return bakedRef{dispOff: off + 3, insnLen: 7}, true
	}
	return bakedRef{}, false
}

// isIndirectCall recognizes a near indirect CALL that has no function
// displacement to bake. A RIP-relative memory operand may still have an ELF
// relocation, which the ordinary relocation pass handles.
func isIndirectCall(code []byte, off int64) bool {
	if off < 0 || off >= int64(len(code)) {
		return false
	}
	i := off
	for i < int64(len(code)) {
		b := code[i]
		if b == 0x26 || b == 0x2e || b == 0x36 || b == 0x3e ||
			b == 0x64 || b == 0x65 || b == 0x66 || b == 0x67 ||
			(b >= 0x40 && b <= 0x4f) {
			i++
			continue
		}
		break
	}
	return i+1 < int64(len(code)) && code[i] == 0xff && ((code[i+1]>>3)&7) == 2
}

// findCallSites returns file-relative offsets of CALL rel32 (0xe8) in code,
// using ELF PLT32/PC32 relocs whose preceding byte is 0xe8.
func findCallSites(code []byte, baseOff uint64, rels []rela, syms []elf.Symbol) []callSite {
	var out []callSite
	for _, r := range rels {
		if r.Off < baseOff || r.Off >= baseOff+uint64(len(code)) {
			continue
		}
		off := int(r.Off - baseOff)
		if off < 1 || code[off-1] != 0xe8 {
			continue
		}
		if r.Type != uint32(elf.R_X86_64_PLT32) && r.Type != uint32(elf.R_X86_64_PC32) {
			continue
		}
		ename := ""
		if r.Sym > 0 && int(r.Sym-1) < len(syms) {
			ename = syms[r.Sym-1].Name
		}
		out = append(out, callSite{Off: off - 1, RelOff: off, Callee: ename, Add: r.Add, Type: r.Type})
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Off < out[j].Off })
	return out
}

type callSite struct {
	Off    int // offset of 0xe8
	RelOff int // offset of reloc field
	Callee string
	Add    int64
	Type   uint32
}

type rela struct {
	Off  uint64
	Type uint32
	Sym  uint32
	Add  int64
}

func main() {
	outO := flag.String("out-o", "", "output goobj path")
	elfPath := flag.String("elf", "", "llc-produced ELF .o")
	metaPath := flag.String("meta", "", "harness.meta.json (P14 sidecar; mirguard identity)")
	mapsDir := flag.String("maps", "", "pass-out dir with maps")
	pkg := flag.String("p", "main", "package path")
	checkOnly := flag.Bool("check-relocs", false, "validate ELF reloc addends then exit 0")
	flag.Parse()
	if *outO == "" || *elfPath == "" || *metaPath == "" || *mapsDir == "" {
		fmt.Fprintf(os.Stderr, "usage: elfpack -elf F.o -meta meta.json -maps DIR -out-o OUT.o\n")
		os.Exit(2)
	}

	var meta metaFile
	if err := json.Unmarshal(mustRead(*metaPath), &meta); err != nil {
		fmt.Fprintf(os.Stderr, "elfpack: meta: %v\n", err)
		os.Exit(1)
	}
	if meta.Mircanon != nil {
		if meta.Mircanon.CfgRewrite || meta.Mircanon.FrameInject || meta.Mircanon.DialectStrip {
			fmt.Fprintf(os.Stderr, "elfpack: FATAL meta says cfg_rewrite/frame_inject/dialect_strip still enabled (P14 requires identity)\n")
			os.Exit(1)
		}
		if len(meta.Mircanon.Transforms) != 0 {
			fmt.Fprintf(os.Stderr, "elfpack: FATAL meta.mircanon.transforms must be empty (P14 identity), got %v\n", meta.Mircanon.Transforms)
			os.Exit(1)
		}
	}

	ef, err := elf.Open(*elfPath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "elfpack: elf open: %v\n", err)
		os.Exit(1)
	}
	defer ef.Close()

	textSec := ef.Section(".text")
	if textSec == nil {
		fmt.Fprintf(os.Stderr, "elfpack: FATAL no .text in %s\n", *elfPath)
		os.Exit(1)
	}
	textData, err := textSec.Data()
	if err != nil {
		panic(err)
	}
	syms, err := ef.Symbols()
	if err != nil {
		panic(err)
	}
	var rels []rela
	if relaSec := ef.Section(".rela.text"); relaSec != nil {
		rd, err := relaSec.Data()
		if err != nil {
			panic(err)
		}
		for i := 0; i+24 <= len(rd); i += 24 {
			off := binary.LittleEndian.Uint64(rd[i : i+8])
			info := binary.LittleEndian.Uint64(rd[i+8 : i+16])
			add := int64(binary.LittleEndian.Uint64(rd[i+16 : i+24]))
			rels = append(rels, rela{
				Off:  off,
				Type: uint32(info & 0xffffffff),
				Sym:  uint32(info >> 32),
				Add:  add,
			})
		}
	} else {
		// No relocations (e.g. SSE leaf with only physregs) — allowed.
		fmt.Printf("elfpack: note: no .rela.text in %s (zero relocs)\n", *elfPath)
	}

	// P13 reloc hardening: PLT32 addend must be -4; PC32 typically -4 or -5 (with rex/modrm).
	relocOK := 0
	for _, r := range rels {
		switch r.Type {
		case uint32(elf.R_X86_64_PLT32):
			if r.Add != -4 {
				fmt.Fprintf(os.Stderr, "elfpack: FATAL PLT32 at 0x%x addend=%d want -4 (llc PIC call)\n", r.Off, r.Add)
				os.Exit(1)
			}
			relocOK++
		case uint32(elf.R_X86_64_PC32):
			// RIP-relative refs: -4/-5 for direct refs, but local symbol refs
			// (e.g. .rodata / jump-table offsets in large TUs) carry any addend.
			// The translation (A' = A+4) is addend-agnostic; only range matters.
			if r.Add < math.MinInt32 || r.Add > math.MaxInt32 {
				fmt.Fprintf(os.Stderr, "elfpack: FATAL PC32 at 0x%x addend=%d out of int32 range\n", r.Off, r.Add)
				os.Exit(1)
			}
			relocOK++
		}
	}
	fmt.Printf("elfpack: reloc check OK (%d PLT32/PC32 addends match llc PIC expectations)\n", relocOK)
	if *checkOnly {
		fmt.Println("elfpack: -check-relocs done")
		return
	}

	type elfFn struct {
		name string
		off  uint64
		size uint64
	}
	byMIR := map[string]elfFn{}
	for _, s := range syms {
		if elf.ST_TYPE(s.Info) != elf.STT_FUNC {
			continue
		}
		byMIR[s.Name] = elfFn{name: s.Name, off: s.Value, size: s.Size}
	}

	argsMap := mustRead(filepath.Join(*mapsDir, "args_map.bin"))
	localsMap := mustRead(filepath.Join(*mapsDir, "locals_map.bin"))
	holdTwoLocals := readOpt(filepath.Join(*mapsDir, "hold_two", "locals_map.bin"))
	holdTwoArgs := readOpt(filepath.Join(*mapsDir, "hold_two", "args_map.bin"))
	holdRegLocals := readOpt(filepath.Join(*mapsDir, "hold_regonly", "locals_map.bin"))
	holdRegArgs := readOpt(filepath.Join(*mapsDir, "hold_regonly", "args_map.bin"))

	// P21: optional per-fn maps from meta.maps_subdir / args_map / locals_map.
	type p21Maps struct {
		argsSym, localsSym string
		args, locals       []byte
	}
	p21ByMIR := map[string]p21Maps{}
	for _, fn := range meta.Functions {
		if fn.MapsSubdir == "" && fn.ArgsMap == "" && fn.LocalsMap == "" {
			continue
		}
		sub := fn.MapsSubdir
		var a, l []byte
		if sub != "" {
			a = readOpt(filepath.Join(*mapsDir, sub, "args_map.bin"))
			l = readOpt(filepath.Join(*mapsDir, sub, "locals_map.bin"))
		}
		if len(a) == 0 {
			a = argsMap
		}
		if len(l) == 0 {
			l = localsMap
		}
		as := fn.ArgsMap
		ls := fn.LocalsMap
		if as == "" {
			as = "gclocals." + fn.MIRName + "Args"
		}
		if ls == "" {
			ls = "gclocals." + fn.MIRName + "Locals"
		}
		p21ByMIR[fn.MIRName] = p21Maps{argsSym: as, localsSym: ls, args: a, locals: l}
	}

	emitStackmap := func(ctxt *obj.Link) {
		// Empty map (n=1, nbit=0): "this frame holds no GC pointer". Used for
		// functions the color pass proved sptr-free, so a stack growth may copy
		// their frame without adjusting anything.
		emitStackmapROData(ctxt, "gclocals.gocEmpty", []byte{1, 0, 0, 0, 0, 0, 0, 0})
		emitStackmapROData(ctxt, "gclocals.gocHoldArgs", argsMap)
		emitStackmapROData(ctxt, "gclocals.gocHoldLive", localsMap)
		if len(holdTwoLocals) > 0 {
			emitStackmapROData(ctxt, "gclocals.gocHoldTwo", holdTwoLocals)
		}
		if len(holdTwoArgs) > 0 {
			emitStackmapROData(ctxt, "gclocals.gocHoldTwoArgs", holdTwoArgs)
		}
		if len(holdRegLocals) > 0 {
			emitStackmapROData(ctxt, "gclocals.gocHoldRegOnly", holdRegLocals)
		}
		if len(holdRegArgs) > 0 {
			emitStackmapROData(ctxt, "gclocals.gocHoldRegOnlyArgs", holdRegArgs)
		} else if len(holdRegLocals) > 0 {
			emitStackmapROData(ctxt, "gclocals.gocHoldRegOnlyArgs", argsMap)
		}
		for _, pm := range p21ByMIR {
			if len(pm.args) > 0 {
				emitStackmapROData(ctxt, pm.argsSym, pm.args)
			}
			if len(pm.locals) > 0 {
				emitStackmapROData(ctxt, pm.localsSym, pm.locals)
			}
		}
	}

	reqs := make([]textReq, 0, len(meta.Functions))
	for _, fn := range meta.Functions {
		flags := 0
		if strings.Contains(fn.Flags, "nosplit") {
			flags |= obj.NOSPLIT
		}
		tr := textReq{
			cstackAlign:     fn.CStackAlign,
			mirName:         fn.MIRName,
			goSym:           fn.GoSym,
			frame:           fn.Frame,
			flags:           flags,
			abi:             obj.ABIInternal,
			calls:           fn.Calls,
			branches:        fn.Branches,
			argSpills:       fn.ArgSpills,
			goArgArea:       fn.GoArgArea,
			sysvPointerRegs: fn.SysvPointerRegs,
			stubMapIndex:    -1,
		}
		if fn.GoArgArea > 0 {
			if fn.GoArgArea%8 != 0 {
				fmt.Fprintf(os.Stderr, "elfpack: FATAL %s has unaligned Go argument area %d\n", fn.GoSym, fn.GoArgArea)
				os.Exit(1)
			}
			words := fn.GoArgArea / 8
			mapBytes := (words + 7) / 8
			blob := make([]byte, 8+2*mapBytes)
			binary.LittleEndian.PutUint32(blob[0:], 2) // slow stub, normal body
			binary.LittleEndian.PutUint32(blob[4:], uint32(words))
			for _, off := range fn.GoStackPtrArgs {
				if off < 0 || off%8 != 0 || off/8 >= words {
					fmt.Fprintf(os.Stderr, "elfpack: FATAL %s has invalid stack pointer argument +%d\n", fn.GoSym, off)
					os.Exit(1)
				}
				bit := uint(off / 8)
				blob[8+int(bit)/8] |= 1 << (bit % 8)
				blob[8+mapBytes+int(bit)/8] |= 1 << (bit % 8)
			}
			for _, arg := range fn.ArgSpills {
				if !arg.Ptr {
					continue
				}
				off := arg.Off - 8 // argp is entry SP+8
				if off < 0 || off%8 != 0 || off/8 >= words {
					fmt.Fprintf(os.Stderr, "elfpack: FATAL %s has invalid pointer register spill +%d\n", fn.GoSym, arg.Off)
					os.Exit(1)
				}
				bit := uint(off / 8)
				blob[8+int(bit)/8] |= 1 << (bit % 8)
			}
			tr.argsBlob = blob
			tr.argsMap = "gclocals.gocGoArgs." + fn.GoSym
		}
		tr.sptrSlots = fn.SptrSlots
		tr.bodyUnproved = !useSptrMaps && (fn.HasSptr == nil || *fn.HasSptr)
		if tr.frame > 8 && (len(fn.SptrSlots) > 0 ||
			(useSptrMaps && (fn.HasSptr == nil || *fn.HasSptr))) {
			tr.wantSptrMap = true
		}
		if len(fn.SptrSlots) > 0 && tr.frame > 8 {
			// Real locals map: bit zero covers the *lowest* word in the
			// locals region, which Go scans starting at varp-nbit*8. An
			// alloca at -off(%rbp) is bit words-off/8 (varp == rbp).
			words := (tr.frame - 8) / 8
			blob := make([]byte, 8+(words+7)/8)
			binary.LittleEndian.PutUint32(blob[0:], 1) // n = 1
			binary.LittleEndian.PutUint32(blob[4:], uint32(words))
			for _, off := range fn.SptrSlots {
				if off < 8 || off/8 > words || off%8 != 0 {
					fmt.Fprintf(os.Stderr, "elfpack: FATAL %s: sptr slot +%d cannot fit an aligned word in %d-byte locals\n", tr.goSym, off, tr.frame-8)
					os.Exit(1)
				}
				bit := words - off/8
				blob[8+bit/8] |= 1 << (uint(bit) % 8)
			}
			tr.localsBlob = blob
		} else if useMorestack && tr.flags&obj.NOSPLIT == 0 && tr.frame > 0 &&
			fn.HasSptr != nil && !*fn.HasSptr {
			// No sptr value in this function: its frame is pointer-free for the
			// GC, so growth may copy it (the runtime requires a map with n>0).
			tr.localsMap = "gclocals.gocEmpty"
		}
		if fn.MIRName == "goc_leaf" {
			tr.abi = obj.ABI0
		}
		if meta.ABI == "ABI0" {
			// C-side (SysV) entry points: referenced as ABI0 from other TUs.
			tr.abi = obj.ABI0
		}
		if fn.ABI == "ABI0" {
			tr.abi = obj.ABI0
		} else if fn.ABI == "ABIInternal" {
			tr.abi = obj.ABIInternal
		}
		switch fn.MIRName {
		case "goc_hold_live", "goc_hold_arg":
			tr.argsMap = "gclocals.gocHoldArgs"
			tr.localsMap = "gclocals.gocHoldLive"
		case "goc_hold_two":
			tr.localsMap = "gclocals.gocHoldTwo"
			tr.argsMap = "gclocals.gocHoldArgs"
			if len(holdTwoArgs) > 0 {
				tr.argsMap = "gclocals.gocHoldTwoArgs"
			}
		case "goc_hold_regonly":
			tr.argsMap = "gclocals.gocHoldRegOnlyArgs"
			tr.localsMap = "gclocals.gocHoldRegOnly"
		default:
			// Fixture / unknown: attach root maps when calls carry stackmap indices.
			hasSM := false
			for _, c := range fn.Calls {
				if c.StackmapIndex >= 0 {
					hasSM = true
					break
				}
			}
			if hasSM && len(localsMap) > 0 {
				tr.localsMap = "gclocals.gocHoldLive"
				tr.argsMap = "gclocals.gocHoldArgs"
			}
		}
		// P21 color-vertical: explicit maps from meta override switch defaults.
		if pm, ok := p21ByMIR[fn.MIRName]; ok {
			if pm.argsSym != "" && len(pm.args) > 0 {
				tr.argsMap = pm.argsSym
			}
			if pm.localsSym != "" && len(pm.locals) > 0 {
				tr.localsMap = pm.localsSym
			}
		}

		reqs = append(reqs, tr)
	}

	// P29: relocation targets that are our own TEXT symbols must be looked up
	// with the symbol's ABI (ABIInternal for goc functions, ABI0 for goc_leaf),
	// otherwise thunk `jmp <fn>.impl` relocs fail as "not defined for ABI0".
	abiByGoSym := map[string]obj.ABI{}
	for _, r := range reqs {
		abiByGoSym[r.goSym] = r.abi
	}

	ctxt := obj.Linknew(&x86.Linkamd64)
	ctxt.IsAsm = true
	ctxt.Pkgpath = *pkg
	ctxt.DiagFunc = func(format string, args ...interface{}) {
		fmt.Fprintf(os.Stderr, "elfpack diag: "+format+"\n", args...)
		os.Exit(1)
	}
	ctxt.Bso = bufio.NewWriter(os.Stdout)
	defer ctxt.Bso.Flush()
	if ctxt.Arch.Init != nil {
		ctxt.Arch.Init(ctxt)
	}
	fileBase := src.NewFileBase("goc_elfpack_llvmmc", "goc_elfpack_llvmmc")
	pos := ctxt.PosTable.XPos(src.MakePos(fileBase, 1, 0))
	emitStackmap(ctxt)
	for _, rq := range reqs {
		if len(rq.argsBlob) != 0 {
			emitStackmapROData(ctxt, rq.argsMap, rq.argsBlob)
		}
	}

	var plist obj.Plist
	var head, tail *obj.Prog
	add := func(p *obj.Prog) {
		if p == nil {
			return
		}
		if head == nil {
			head = p
		} else {
			tail.Link = p
		}
		for p.Link != nil {
			p = p.Link
		}
		tail = p
	}

	nSptrMaps := 0

	// Stage B: LLVM stack maps, keyed by the ELF function name, which is the
	// meta's mir_name. (Stripping ".impl" would hand a C body's records to its
	// Go-ABI thunk "X", whose code offsets are unrelated.)
	recsByName := parseLLVMStackMaps(ef)

	// Emit the locals maps: color-pass alloca slots and tagged stack-map root
	// allocas, one map per distinct call-site state.
	for i := range reqs {
		rq := &reqs[i]
		sysvSplit := gocWantsSplit(*rq) && rq.abi == obj.ABI0
		if rq.frame <= 8 && !sysvSplit {
			continue
		}
		words := (rq.frame - 8) / 8
		if sysvSplit && words < 7 {
			// AX + six SysV arg GPRs below the stub's saved BP.
			// Callee-saved saves sit further down and are not pointer slots.
			words = 7
		}
		if words <= 0 {
			continue
		}
		base := make([]byte, (words+7)/8)
		// Dedicated pointer slots (goc.spill.root / goc.anchor / color slots).
		// Safe as a function-wide base only because llc is passed
		// -no-stack-slot-sharing: a later call cannot reuse the slot for a
		// scalar. Per-call Direct locations are still OR'd on top.
		if useSptrMaps {
			for _, off := range rq.sptrSlots {
				// Same indexing as Direct locations: bit 0 is the word at
				// varp-size, so a slot at -off(%rbp) is bit (words - off/8).
				if off < 8 || off%8 != 0 || off/8 > words {
					fmt.Fprintf(os.Stderr, "elfpack: FATAL %s: sptr slot +%d not a frame word (frame=%d)\n", rq.goSym, off, rq.frame)
					os.Exit(1)
				}
				bit := words - off/8
				if bit >= 0 && bit < words {
					base[bit/8] |= 1 << (bit % 8)
				}
			}
		}
		recs := recsByName[rq.mirName]
		if !rq.wantSptrMap && len(recs) == 0 && !sysvSplit {
			continue
		}
		if sysvSplit && rq.bodyUnproved {
			// Without per-call maps the body is unproved. Retain the previous
			// missing-stackmap failure rather than bless it with a zero map.
			continue
		}
		maps := [][]byte{base}
		trans := []pcValue{{PC: 0, Value: 0}}
		for _, rec := range recs {
			bits := append([]byte(nil), base...)
			for _, l := range rec.locs {
				// This pass's tagged records contain the addresses of
				// pointer-typed root allocas (Direct RBP+off names the slot
				// whose *contents* hold the raw sptr) and, as Constant
				// locations, byte offsets from SP of stack-passed pointer
				// arguments. Frames have a fixed size (no realignment), so
				// SP at a call is RBP-(frame-8).
				var off int
				switch {
				case l.typ == 2 && l.reg == 6 && l.off < 0 && l.size == 8:
					off = int(-l.off)
				case l.typ == 4 && l.off >= 0 && l.off%8 == 0:
					off = rq.frame - 8 - int(l.off)
				default:
					fmt.Fprintf(os.Stderr, "elfpack: FATAL %s at call +0x%x: pointer root is not a Direct RBP word (type=%d reg=%d off=%d size=%d)\n", rq.goSym, rec.insnOff, l.typ, l.reg, l.off, l.size)
					os.Exit(1)
				}
				if off < 8 || off/8 > words || off%8 != 0 {
					// off < 8 is the saved-BP word, which the copier adjusts
					// itself, or an outgoing push this frame does not reserve.
					// It is not a local. A misaligned or past-the-end slot is
					// still a real map bug.
					if off >= 8 && (off%8 != 0 || off/8 > words) {
						fmt.Fprintf(os.Stderr, "elfpack: FATAL %s at call +0x%x: stackmap slot +%d not in frame (type=%d reg=%d raw=%d size=%d frame=%d words=%d)\n", rq.goSym, rec.insnOff, off, l.typ, l.reg, l.off, l.size, rq.frame, words)
						os.Exit(1)
					}
					continue
				}
				bit := words - off/8
				bits[bit/8] |= 1 << (uint(bit) % 8)
			}
			idx := 0
			for j, m := range maps {
				if bytes.Equal(m, bits) {
					idx = j
					break
				}
				if j == len(maps)-1 {
					maps = append(maps, bits)
					idx = len(maps) - 1
				}
			}
			trans = append(trans, pcValue{PC: int64(rec.insnOff), Value: int32(idx)})
		}
		if sysvSplit {
			// The body map describes actual frame slots; applying it while the
			// stub holds argument registers would adjust unrelated scalar data.
			// At the morestack return PC the saved BP is varp, and below it lie
			// AX, DI, SI, DX, CX, R8, R9 (in that order).
			stub := make([]byte, len(base))
			regWord := map[string]int{"rdi": 2, "rsi": 3, "rdx": 4, "rcx": 5, "r8": 6, "r9": 7}
			for _, reg := range rq.sysvPointerRegs {
				word := regWord[reg]
				if word == 0 {
					fmt.Fprintf(os.Stderr, "elfpack: FATAL %s: invalid SysV pointer register %q\n", rq.goSym, reg)
					os.Exit(1)
				}
				bit := words - word
				stub[bit/8] |= 1 << (uint(bit) % 8)
			}
			rq.stubMapIndex = int32(len(maps))
			maps = append(maps, stub)
		}
		blob := make([]byte, 8+len(maps)*(words+7)/8)
		binary.LittleEndian.PutUint32(blob[0:], uint32(len(maps)))
		binary.LittleEndian.PutUint32(blob[4:], uint32(words))
		for j, m := range maps {
			copy(blob[8+j*(words+7)/8:], m)
		}
		name := "gclocals.gocSptr." + rq.goSym
		emitStackmapROData(ctxt, name, blob)
		rq.localsMap = name
		if len(trans) > 1 {
			rq.smapTrans = trans
		}
		nSptrMaps++
	}
	if nSptrMaps > 0 {
		fmt.Printf("elfpack: %d functions with real sptr frame maps\n", nSptrMaps)
	}

	type pending struct {
		sym      *obj.LSym
		code     []byte
		baseOff  uint64
		frame    int
		calls    []metaCall
		branches []metaBranch
		req      textReq
	}
	var pend []pending

	for _, req := range reqs {
		efn, ok := byMIR[req.mirName]
		if !ok || efn.size == 0 {
			fmt.Fprintf(os.Stderr, "elfpack: FATAL missing LLVM-encoded TEXT for %s in ELF\n", req.mirName)
			os.Exit(1)
		}
		if efn.off+efn.size > uint64(len(textData)) {
			fmt.Fprintf(os.Stderr, "elfpack: FATAL %s out of .text range\n", req.mirName)
			os.Exit(1)
		}
		code := append([]byte(nil), textData[efn.off:efn.off+efn.size]...)

		s := ctxt.LookupABI(req.goSym, req.abi)
		// Frame already in LLVM bytes; tell Go frame=0 so assembler does not double-prologue.
		ctxt.InitTextSym(s, req.flags, pos)
		tp := ctxt.NewProg()
		tp.As = obj.ATEXT
		tp.Pos = pos
		tp.Ctxt = ctxt
		tp.From = obj.Addr{Type: obj.TYPE_MEM, Name: obj.NAME_EXTERN, Sym: s}
		tp.To = obj.Addr{Type: obj.TYPE_TEXTSIZE, Offset: 0}
		tp.To.Val = int32(0)
		s.Func().Text = tp
		s.Func().Locals = 0
		s.Func().Args = 0
		add(tp)

		if req.argsMap != "" {
			p := ctxt.NewProg()
			p.As = obj.AFUNCDATA
			p.Pos = pos
			p.From = obj.Addr{Type: obj.TYPE_CONST, Offset: int64(abi.FUNCDATA_ArgsPointerMaps)}
			p.To = obj.Addr{Type: obj.TYPE_MEM, Name: obj.NAME_EXTERN, Sym: ctxt.Lookup(req.argsMap)}
			add(p)
		}
		if req.localsMap != "" {
			p := ctxt.NewProg()
			p.As = obj.AFUNCDATA
			p.Pos = pos
			p.From = obj.Addr{Type: obj.TYPE_CONST, Offset: int64(abi.FUNCDATA_LocalsPointerMaps)}
			p.To = obj.Addr{Type: obj.TYPE_MEM, Name: obj.NAME_EXTERN, Sym: ctxt.Lookup(req.localsMap)}
			add(p)
		}
		// Seed PCDATA_StackMapIndex=-1 at entry; dense table rebuilt after LLVM bytes.
		p := ctxt.NewProg()
		p.As = obj.APCDATA
		p.Pos = pos
		p.From = obj.Addr{Type: obj.TYPE_CONST, Offset: int64(abi.PCDATA_StackMapIndex)}
		p.To = obj.Addr{Type: obj.TYPE_CONST, Offset: -1}
		add(p)

		pend = append(pend, pending{sym: s, code: code, baseOff: efn.off, frame: req.frame, calls: req.calls, branches: req.branches, req: req})
		fmt.Printf("elfpack: %s ← LLVM MC %s (%d bytes, %d annotated CALLs)\n", req.goSym, req.mirName, len(code), len(req.calls))
	}

	plist.Firstpc = head
	obj.Flushplist(ctxt, &plist, nil)
	for _, pe := range pend {
		if pe.req.goArgArea > 0 {
			pe.sym.Func().Args = int32(pe.req.goArgArea)
		}
	}

	mirToGo := map[string]string{}
	for _, fn := range meta.Functions {
		mirToGo[fn.MIRName] = fn.GoSym
	}

	pcdataProof := []string{}

	// P29: reloc targets outside the meta TEXT set. Real code references data
	// (string literals, jump tables, globals) and .text local labels, so emit
	// Go DATA/BSS symbols per allocated data section and fold .text labels into
	// their enclosing meta TEXT symbol (+offset).
	elfSecs := ef.Sections
	textSecIdx := -1
	for i, sc := range elfSecs {
		if sc == textSec {
			textSecIdx = i
			break
		}
	}
	sanitize := func(name string) string {
		out := make([]byte, 0, len(name))
		for i := range name {
			c := name[i]
			if c == '_' || c == '.' || c == '$' ||
				(c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
				(c >= '0' && c <= '9') {
				out = append(out, c)
			} else {
				out = append(out, '_')
			}
		}
		return string(out)
	}
	tuTag := meta.TU
	if tuTag == "" {
		// Fall back to the output object's name: several TUs may carry no tag
		// (single-function builds, shim), and section symbols are named after
		// it, so a shared default would collide at link time.
		tuTag = sanitize(strings.TrimSuffix(filepath.Base(*outO), ".o"))
		if tuTag == "" {
			tuTag = "tu"
		}
	}
	// P29: per-named-symbol data objects. Globals referenced across TUs by
	// name (e.g. stdout/stderr in the freestanding shim) need their own Go
	// symbol: section-level symbols cannot satisfy a name reference.
	dataSym := map[int]*obj.LSym{}
	type nameRange struct {
		start, end uint64
		sec        int
		sym        *obj.LSym
	}
	var nameRanges []nameRange
	for i := range syms {
		es := syms[i]
		if es.Section == elf.SHN_UNDEF || es.Section == elf.SHN_ABS ||
			int(es.Section) >= len(elfSecs) || es.Size == 0 || es.Name == "" {
			continue
		}
		if int(es.Section) == textSecIdx {
			continue
		}
		ty := elf.ST_TYPE(es.Info)
		if ty != elf.STT_OBJECT && ty != elf.STT_NOTYPE {
			continue
		}
		bind := elf.ST_BIND(es.Info)
		if bind != elf.STB_GLOBAL && bind != elf.STB_WEAK {
			continue
		}
		sc := elfSecs[es.Section]
		if sc == nil || sc.Flags&elf.SHF_ALLOC == 0 {
			continue
		}
		ls := ctxt.Lookup(*pkg + "." + es.Name)
		if sc.Type == elf.SHT_NOBITS {
			ctxt.GloblPos(ls, int64(es.Size), obj.NOPTR, src.NoXPos)
		} else {
			b, err := sc.Data()
			if err != nil {
				panic(err)
			}
			end := es.Value + es.Size
			if end > uint64(len(b)) {
				continue
			}
			chunk := b[es.Value:end]
			ls.Type = objabi.SDATA
			ctxt.GloblPos(ls, int64(len(chunk)), obj.NOPTR, src.NoXPos)
			off := 0
			for ; off+4 <= len(chunk); off += 4 {
				ls.WriteInt(ctxt, int64(off), 4, int64(binary.LittleEndian.Uint32(chunk[off:off+4])))
			}
			for ; off < len(chunk); off++ {
				ls.WriteInt(ctxt, int64(off), 1, int64(chunk[off]))
			}
		}
		dataSym[i] = ls
		nameRanges = append(nameRanges, nameRange{start: es.Value, end: es.Value + es.Size, sec: int(es.Section), sym: ls})
	}

	secSym := map[int]*obj.LSym{}
	secName := map[int]string{}
	for i, sc := range elfSecs {
		secName[i] = sc.Name
		if sc.Flags&elf.SHF_ALLOC == 0 || i == textSecIdx || sc.Size == 0 {
			continue
		}
		if sc.Type != elf.SHT_PROGBITS && sc.Type != elf.SHT_NOBITS {
			continue
		}
		// Program data only: ELF/linker metadata sections (.init_array,
		// .dynamic, .got*, .eh_frame, .rela*) must not be copied into the Go
		// object — the host loader would execute bogus init entries.
		if !strings.HasPrefix(sc.Name, ".rodata") && !strings.HasPrefix(sc.Name, ".data") &&
			!strings.HasPrefix(sc.Name, ".bss") && !strings.HasPrefix(sc.Name, ".sdata") &&
			!strings.HasPrefix(sc.Name, ".sbss") && !strings.HasPrefix(sc.Name, ".tdata") &&
			!strings.HasPrefix(sc.Name, ".tbss") {
			continue
		}
		ls := ctxt.Lookup("goc.data." + tuTag + "." + sanitize(sc.Name))
		if sc.Type == elf.SHT_NOBITS {
			// C data holds C pointers, not Go pointers: NOPTR (GC never scans).
			ctxt.GloblPos(ls, int64(sc.Size), obj.NOPTR, src.NoXPos)
		} else {
			b, err := sc.Data()
			if err != nil {
				panic(err)
			}
			fl := int(obj.RODATA | obj.DUPOK)
			if sc.Flags&elf.SHF_WRITE != 0 {
				ls.Type = objabi.SDATA // writable → SNOPTRDATA via NOPTR
				fl = obj.NOPTR
			}
			ctxt.GloblPos(ls, int64(len(b)), fl, src.NoXPos)
			off := 0
			for ; off+4 <= len(b); off += 4 {
				ls.WriteInt(ctxt, int64(off), 4, int64(binary.LittleEndian.Uint32(b[off:off+4])))
			}
			for ; off < len(b); off++ {
				ls.WriteInt(ctxt, int64(off), 1, int64(b[off]))
			}
		}
		secSym[i] = ls
	}
	// Ranges of our meta TEXT symbols, built up front from the meta + ELF
	// symbol table (pend is only populated by the TEXT emit loop below, but
	// data/text relocs need these ranges while resolving).
	type textRange struct {
		off, end uint64
		sym      *obj.LSym
	}
	textRanges := make([]textRange, 0, len(reqs))
	for _, rq := range reqs {
		efn, ok := byMIR[rq.mirName]
		if !ok {
			continue
		}
		textRanges = append(textRanges, textRange{efn.off, efn.off + efn.size, ctxt.LookupABI(rq.goSym, rq.abi)})
	}
	sort.Slice(textRanges, func(i, j int) bool { return textRanges[i].off < textRanges[j].off })

	// Split preambles are inserted *at* each function's entry, so the entry
	// address keeps its position while the body and everything after it move
	// down by checkLen. Two things must be re-based by the running total:
	//   - relocs whose addend points into .text (e.g. computed-goto label
	//     tables in .data.rel.ro), via shiftAt in resolveTarget;
	//   - direct branches llc resolved without a relocation (calls/jumps to
	//     local symbols: the section is contiguous inside its own object), via
	//     patchBakedBranches.
	type insertion struct {
		at uint64 // ELF .text offset of the function entry
		n  int64  // bytes inserted there
	}
	var inserts []insertion
	if useMorestack {
		for _, rq := range reqs {
			if !gocWantsSplit(rq) {
				continue
			}
			efn, ok := byMIR[rq.mirName]
			if !ok || efn.size == 0 {
				continue
			}
			checkFrame := int32(rq.frame)
			if rq.cstackAlign {
				checkFrame = 16
			}
			pre, _ := gocSplitCheck(checkFrame, rq.abi == obj.ABIInternal)
			inserts = append(inserts, insertion{at: efn.off, n: int64(len(pre))})
		}
		sort.Slice(inserts, func(i, j int) bool { return inserts[i].at < inserts[j].at })
	}
	insPrefix := make([]int64, len(inserts)+1)
	for i, ins := range inserts {
		insPrefix[i+1] = insPrefix[i] + ins.n
	}
	// shiftAt reports how many inserted bytes precede an ELF .text address.
	shiftAt := func(addr uint64) int64 {
		i := sort.Search(len(inserts), func(i int) bool { return inserts[i].at >= addr })
		return insPrefix[i]
	}

	// textSymAt resolves a .text address to the enclosing meta TEXT symbol plus
	// the delta to add to the reloc addend. Addresses in alignment padding
	// between functions (e.g. one past a function end) resolve to the next
	// function with a negative delta.
	textSymAt := func(addr uint64) (*obj.LSym, int64) {
		for _, tr := range textRanges {
			if addr >= tr.off && addr < tr.end {
				return tr.sym, int64(addr - tr.off)
			}
		}
		for _, tr := range textRanges {
			if tr.off > addr {
				return tr.sym, -int64(tr.off - addr)
			}
		}
		return nil, 0
	}
	// resolveText maps an ELF .text address to (Go symbol, offset inside the
	// emitted symbol). Split preambles insert bytes at each entry, so the body
	// and everything after it moves down while the entry keeps its position.
	resolveText := func(addr uint64) (*obj.LSym, int64) {
		ts, delta := textSymAt(addr)
		if ts == nil {
			return nil, 0
		}
		if delta == 0 && strings.HasPrefix(ts.Name, *pkg+".") {
			base := strings.TrimPrefix(ts.Name, *pkg+".")
			if libcallImpl[base] {
				impl := *pkg + "." + base + ".impl"
				if ab, ok := abiByGoSym[impl]; ok && ab == obj.ABI0 {
					// A same-TU ELF call or function pointer resolves by address
					// to the Go thunk. C uses the SysV implementation instead.
					return ctxt.LookupABI(impl, obj.ABI0), 0
				}
			}
		}
		return ts, delta + shiftAt(addr) - shiftAt(addr-uint64(delta))
	}
	// resolveCName maps a C symbol name (ELF name or llc callee name) to the
	// Go symbol that defines it. The goabi preprocessor appends ".impl" to
	// C-side declarations, so strip that suffix before deciding: a C function
	// with an ABI0 body resolves to "<pkg>.<base>.impl", anything else to the
	// package-qualified name.
	resolveCName := func(name string) *obj.LSym {
		name = strings.TrimSuffix(name, "@plt")
		if strings.HasSuffix(name, ".goabi") {
			// Inline C wrappers may call a Go function explicitly after
			// restoring g in R14. The suffix describes the relocation ABI;
			// it is not part of the linked Go symbol name.
			return ctxt.LookupABI(strings.TrimSuffix(name, ".goabi"), obj.ABIInternal)
		}
		if libcallImpl[name] {
			// llc can synthesize a libcall after the Go-ABI rename pass. If
			// this TU also defines it, mirToGo would otherwise resolve the
			// bare name to its ABIInternal thunk. C callers must enter SysV.
			return ctxt.LookupABI(*pkg+"."+name+".impl", obj.ABI0)
		}
		if goName, ok := mirToGo[name]; ok {
			name = goName
		}
		if ab, ok := abiByGoSym[name]; ok {
			return ctxt.LookupABI(name, ab)
		}
		if name == "llvm.trap" || name == "llvm.trap.impl" {
			// llc lowers __builtin_trap() (the shim's stubs) to this function.
			return ctxt.Lookup("runtime.abort")
		}
		if name == "" || strings.HasPrefix(name, *pkg+".") {
			return ctxt.Lookup(name)
		}
		base, suffix := name, ""
		if strings.HasSuffix(base, ".impl") {
			base, suffix = strings.TrimSuffix(base, ".impl"), ".impl"
		}
		if strings.Contains(base, ".") {
			return ctxt.Lookup(name) // Go/runtime symbol: verbatim
		}
		if suffix == ".impl" || libcallImpl[base] {
			// ".impl" comes from the IR renaming pass (the target has an ABI0
			// body); libcallImpl are llc libcalls with int/ptr signatures, which
			// the goabi subset also covers. Everything else (float/struct
			// signatures, data) keeps the plain name.
			return ctxt.LookupABI(*pkg+"."+base+".impl", obj.ABI0)
		}
		return ctxt.Lookup(*pkg + "." + base + suffix)
	}
	// resolveTarget maps an ELF reloc to a Go symbol + the effective addend
	// (section symbols carry the target address in the reloc addend, e.g.
	// `R_X86_64_64 .text+0x3E220` for a static function pointer, or
	// `.data.rel.ro+0x558` for a global): fold the section offset into the
	// symbol and keep only the residual in the Go addend.
	// resolveTarget returns the target symbol plus the *normalized absolute*
	// addend: the offset of the referenced address from the symbol's start. ELF
	// symbol relocs carry a -4 for PC-relative forms, section relocs carry the
	// raw target offset; both must end up meaning the same thing for the caller.
	resolveTarget := func(r rela) (*obj.LSym, int64, string) {
		abs := r.Add
		if pcRelELF(r.Type) {
			abs += 4
		}
		ename := ""
		if r.Sym > 0 && int(r.Sym-1) < len(syms) {
			ename = syms[r.Sym-1].Name
		}
		if r.Sym == 0 || int(r.Sym-1) >= len(syms) {
			// No symbol table entry: the reloc carries everything it needs.
			return nil, 0, ""
		}
		es := syms[r.Sym-1]
		switch {
		case es.Section == elf.SHN_UNDEF:
			// C symbols are package-qualified on both sides (no dot in the
			// name), so cross-TU refs match the defining TU's go_sym; dotted
			// names are Go/runtime symbols and stay verbatim.
			//
			// The goabi preprocessor appends ".impl" to C-side declarations,
			// so strip that suffix before deciding: "lre_realloc.impl" is still
			// a C symbol and must resolve to the defining TU's
			// "main.lre_realloc.impl", not to a bare local name.
			return resolveCName(ename), abs, ename
		case elf.ST_TYPE(es.Info) == elf.STT_SECTION && secName[int(es.Section)] == ".text":
			// Static function address: the addend is the offset within .text,
			// normalized through abs (not raw r.Add) so a PC-relative form loses
			// its ELF -4 instead of resolving 4 bytes into the previous symbol.
			if abs < 0 {
				return nil, 0, ename
			}
			ts, delta := resolveText(uint64(abs))
			return ts, delta, ename
		case int(es.Section) == textSecIdx:
			// Named .text label: fold into the enclosing meta TEXT symbol. The
			// normalized addend is the offset inside the label (0 for a symbol
			// reloc, the intra-function delta for a `sym+N` reference).
			addr := int64(es.Value) + abs
			if addr < 0 {
				return nil, 0, ename
			}
			ts, delta := resolveText(uint64(addr))
			return ts, delta, ename
		default:
			if ls, ok := dataSym[int(r.Sym-1)]; ok {
				return ls, abs, ename
			}
			if ls, ok := secSym[int(es.Section)]; ok {
				// A local .LCPI* constant (or other named data symbol) may start
				// partway into a section. When there is no per-name Go symbol, the
				// relocation targets the section copy, so include the ELF symbol's
				// section-relative value as well as its normalized addend.
				return ls, int64(es.Value) + abs, ename
			}
			return nil, 0, ename
		}
	}
	// GOTPCREL/GOTPCRELX: the instruction reads the symbol's GOT slot, so
	// materialize a Go data slot holding the target address and reference it
	// PC-relatively (handles every instruction form, no byte patching).
	gotSyms := map[string]*obj.LSym{}
	gotSlot := func(target *obj.LSym, extraAdd int64) *obj.LSym {
		key := fmt.Sprintf("%s+%d", target.Name, extraAdd)
		if ls, ok := gotSyms[key]; ok {
			return ls
		}
		ls := ctxt.Lookup("goc.got." + tuTag + "." + sanitize(target.Name))
		ls.Type = objabi.SDATA
		ctxt.GloblPos(ls, 8, obj.NOPTR, src.NoXPos)
		// The slot must own its 8 bytes: a reloc at 0+8 inside a zero-length
		// symbol is rejected by the linker ("invalid relocation ... not in [0,0)").
		ls.WriteInt(ctxt, 0, 8, 0)
		ls.AddRel(ctxt, obj.Reloc{Off: 0, Siz: 8, Type: objabi.R_ADDR, Add: extraAdd, Sym: target})
		gotSyms[key] = ls
		return ls
	}

	// Data-section relocs (e.g. QJS class tables holding function pointers):
	// the section bytes were copied verbatim, so their internal relocs must be
	// re-emitted on the Go data symbol or the pointers stay zero.
	nJumpTableRelocs := 0
	for i, ls := range secSym {
		relaSec := ef.Section(".rela" + secName[i])
		if relaSec == nil {
			continue
		}
		// Symbols built from SHT_NOBITS carry size but no bytes; relocs need
		// the bytes to exist or the linker rejects them ("not in [0,0)").
		for int64(len(ls.P)) < ls.Size {
			ls.WriteInt(ctxt, int64(len(ls.P)), 1, 0)
		}
		rd, err := relaSec.Data()
		if err != nil {
			panic(err)
		}
		dataRels := make([]rela, 0, len(rd)/24)
		pcTextAt := make(map[uint64]bool)
		for j := 0; j+24 <= len(rd); j += 24 {
			off := binary.LittleEndian.Uint64(rd[j : j+8])
			info := binary.LittleEndian.Uint64(rd[j+8 : j+16])
			add := int64(binary.LittleEndian.Uint64(rd[j+16 : j+24]))
			r := rela{Off: off, Type: uint32(info & 0xffffffff), Sym: uint32(info >> 32), Add: add}
			dataRels = append(dataRels, r)
			if r.Type == uint32(elf.R_X86_64_PC32) && r.Sym > 0 && int(r.Sym-1) < len(syms) &&
				int(syms[r.Sym-1].Section) == textSecIdx {
				pcTextAt[off] = true
			}
		}

		// LLVM emits `.long case_label - table_base` as R_X86_64_PC32
		// .text+(case_address + field_offset - table_base). The addend is
		// NOT a text address once field_offset advances far enough to cross
		// a function boundary. Each referenced table base is an ELF PCREL
		// code reference to this section and a text relocation at its first
		// word; reconstruct the actual label before resolving its Go TEXT.
		type tableBase struct {
			off   uint64
			owner *obj.LSym
		}
		var tables []tableBase
		for _, cr := range rels {
			if cr.Type != uint32(elf.R_X86_64_PC32) || cr.Sym == 0 || int(cr.Sym-1) >= len(syms) {
				continue
			}
			es := syms[cr.Sym-1]
			if int(es.Section) != i {
				continue
			}
			base := int64(es.Value) + cr.Add + 4
			if base < 0 || !pcTextAt[uint64(base)] {
				continue
			}
			owner, _ := textSymAt(cr.Off)
			if owner == nil {
				fmt.Fprintf(os.Stderr, "elfpack: FATAL jump table %s+0x%x has no owning TEXT\n", secName[i], base)
				os.Exit(1)
			}
			tables = append(tables, tableBase{off: uint64(base), owner: owner})
		}
		sort.Slice(tables, func(a, b int) bool { return tables[a].off < tables[b].off })
		for j := 1; j < len(tables); j++ {
			if tables[j-1].off == tables[j].off && tables[j-1].owner != tables[j].owner {
				fmt.Fprintf(os.Stderr, "elfpack: FATAL jump table %s+0x%x has two owners\n", secName[i], tables[j].off)
				os.Exit(1)
			}
		}

		for _, r := range dataRels {
			off := r.Off
			var target *obj.LSym
			var baseAdd int64
			var ename string
			if pcTextAt[off] {
				j := sort.Search(len(tables), func(j int) bool { return tables[j].off > off }) - 1
				if j < 0 {
					fmt.Fprintf(os.Stderr, "elfpack: FATAL no base for jump table %s+0x%x\n", secName[i], off)
					os.Exit(1)
				}
				indexBytes := int64(off - tables[j].off)
				actual := int64(syms[r.Sym-1].Value) + r.Add - indexBytes
				if actual < 0 {
					fmt.Fprintf(os.Stderr, "elfpack: FATAL negative jump table target %s+0x%x\n", secName[i], off)
					os.Exit(1)
				}
				var delta int64
				target, delta = resolveText(uint64(actual))
				if target != tables[j].owner {
					fmt.Fprintf(os.Stderr, "elfpack: FATAL jump table %s+0x%x targets different TEXT from owner\n", secName[i], off)
					os.Exit(1)
				}
				// Go R_PCREL subtracts the field address+4. Restore the
				// field displacement so the word remains casePC-tableBase.
				baseAdd = delta + indexBytes + 4
				ename = target.Name
				nJumpTableRelocs++
			} else if r.Sym == 0 {
				// Section-relative reloc (e.g. jump-table `.long .L1-.L0`):
				// the target is the section itself.
				target, baseAdd, ename = secSym[i], r.Add, secName[i]
			} else {
				// Data-section reference (e.g. a function pointer in a class
				// table): not a call, so only the known-C-function set decides
				// whether it binds to an ABI0 body.
				target, baseAdd, ename = resolveTarget(r)
			}
			if target == nil {
				if _, emitted := secSym[i]; !emitted {
					// Section not repacked (e.g. .eh_frame): irrelevant here.
					continue
				}
				fmt.Fprintf(os.Stderr, "elfpack: FATAL data reloc at %s+0x%x: unresolved %q\n", secName[i], off, ename)
				os.Exit(1)
			}
			// Relocs inside a per-name global must be emitted on that symbol
			// (code references it by name); section copies get the rest.
			relOff := int64(off)
			hostSym := ls
			for _, nr := range nameRanges {
				if nr.sec == i && off >= nr.start && off < nr.end {
					hostSym = nr.sym
					relOff = int64(off - nr.start)
					break
				}
			}
			var typ objabi.RelocType
			siz := 4
			goAdd := baseAdd
			switch r.Type {
			case uint32(elf.R_X86_64_64):
				typ, siz = objabi.R_ADDR, 8
			case uint32(elf.R_X86_64_32), uint32(elf.R_X86_64_32S):
				typ = objabi.R_ADDR
			case uint32(elf.R_X86_64_PC32), uint32(elf.R_X86_64_PLT32):
				// resolveTarget already converted ELF's S+A-P into an absolute
				// symbol-relative addend (A+4). Go R_PCREL subtracts Siz=4 at
				// link time, so another +4 would jump four bytes into each
				// switch case (QJS .rodata jump tables use .text+addend).
				typ, goAdd = objabi.R_PCREL, baseAdd
			default:
				fmt.Fprintf(os.Stderr, "elfpack: FATAL unsupported data reloc type %d in %s at +0x%x\n", r.Type, secName[i], off)
				os.Exit(1)
			}
			hostSym.AddRel(ctxt, obj.Reloc{
				Off:  int32(relOff),
				Siz:  uint8(siz),
				Type: typ,
				Add:  goAdd,
				Sym:  target,
			})
		}
	}
	if nJumpTableRelocs > 0 {
		fmt.Printf("elfpack: re-based %d PC-relative jump-table entries\n", nJumpTableRelocs)
	}

	nMorestack := 0
	// Reloc field offsets per function, in ELF .text space: baked branches are
	// re-based below, relocated ones are left to the linker.
	relocAt := map[int64]bool{}
	for _, r := range rels {
		relocAt[int64(r.Off)] = true
	}
	nBaked := 0
	bakedRels := make([][]obj.Reloc, len(pend))

	// bakeReloc handles one metadata-addressed .text reference. If its field
	// already has an ELF relocation, the generic relocation pass owns it;
	// otherwise the baked displacement is decoded, zeroed, and recorded against
	// the target symbol. Indirect CALLs have no call displacement to bake.
	// wantCall enforces the calls/branches split — a CALL is a safepoint, a
	// JMP/Jcc/LEA is not, and confusing the two would put a stack map index on
	// an instruction that can never grow the stack.
	bakeReloc := func(pi int, pe pending, off int64, callee string, wantCall bool, what string) {
		if off < 0 || off >= int64(len(pe.code)) {
			fmt.Fprintf(os.Stderr, "elfpack: FATAL %s: meta %s offset +0x%x outside %d-byte body\n", pe.sym.Name, what, off, len(pe.code))
			os.Exit(1)
		}
		if wantCall && isIndirectCall(pe.code, off) {
			// There is no call displacement to bake. Any RIP-relative operand
			// relocation is preserved and rewritten by the ELF relocation pass.
			return
		}
		r, ok := decodeBakedRef(pe.code, off)
		if !ok {
			fmt.Fprintf(os.Stderr, "elfpack: FATAL %s: meta %s at +0x%x is not a CALL/JMP/Jcc/LEA (0x%02x)\n", pe.sym.Name, what, off, pe.code[off])
			os.Exit(1)
		}
		if r.isCall != wantCall {
			if r.isCall {
				fmt.Fprintf(os.Stderr, "elfpack: FATAL %s: meta branch at +0x%x is a CALL: a safepoint belongs in meta.calls\n", pe.sym.Name, off)
			} else {
				fmt.Fprintf(os.Stderr, "elfpack: FATAL %s: meta call at +0x%x is not a CALL (0x%02x): a non-safepoint branch must not carry a stack map index\n", pe.sym.Name, off, pe.code[off])
			}
			os.Exit(1)
		}
		if off+r.insnLen > int64(len(pe.code)) {
			fmt.Fprintf(os.Stderr, "elfpack: FATAL %s: truncated %s at +0x%x\n", pe.sym.Name, what, off)
			os.Exit(1)
		}
		if relocAt[int64(pe.baseOff)+r.dispOff] {
			return // already relocated by the ELF: the reloc loop below rewrites it
		}
		// Decode the baked displacement and resolve by address: static
		// (TU-qualified) and global targets then both land on the defining
		// symbol without guessing from the name. The offsets come from the
		// meta's disassembly, so this cannot mis-decode an unrelated 0xe8.
		var disp int64
		if r.rel8 {
			disp = int64(int8(pe.code[r.dispOff]))
		} else {
			disp = int64(int32(binary.LittleEndian.Uint32(pe.code[r.dispOff:])))
		}
		targetOff := int64(pe.baseOff) + off + r.insnLen + disp
		if targetOff >= int64(pe.baseOff) &&
			targetOff < int64(pe.baseOff)+int64(len(pe.code)) &&
			targetOff != int64(pe.baseOff) {
			return // an interior target shifts with the body; the entry does not
		}
		if r.rel8 {
			fmt.Fprintf(os.Stderr, "elfpack: FATAL %s: rel8 %s at +0x%x targets outside its function\n", pe.sym.Name, what, off)
			os.Exit(1)
		}
		if targetOff < 0 {
			fmt.Fprintf(os.Stderr, "elfpack: FATAL %s: %s at +0x%x resolves before .text\n", pe.sym.Name, what, off)
			os.Exit(1)
		}
		var target *obj.LSym
		var add int64
		if wantCall && libcallImpl[strings.TrimSuffix(callee, "@plt")] {
			// llc may bake a call to this TU's bare libcall symbol. That
			// address is the Go-facing thunk when goabi rewrote the definition;
			// a C caller must instead enter the ABI0 .impl body.
			target = resolveCName(callee)
		} else {
			target, add = resolveText(uint64(targetOff))
		}
		if target == nil {
			// Target outside this object (a cross-TU reference without a reloc
			// is not expected): fall back to the disassembly name.
			target, add = resolveCName(callee), 0
		}
		if target == nil {
			fmt.Fprintf(os.Stderr, "elfpack: FATAL %s: unresolved %s target %q at +0x%x\n", pe.sym.Name, what, callee, off)
			os.Exit(1)
		}
		binary.LittleEndian.PutUint32(pe.code[r.dispOff:], 0)
		relType := objabi.R_PCREL
		if r.isCall {
			relType = objabi.R_CALL
		}
		bakedRels[pi] = append(bakedRels[pi], obj.Reloc{
			Off:  int32(r.dispOff),
			Siz:  4,
			Type: relType,
			Add:  add,
			Sym:  target,
		})
		nBaked++
	}

	for pi, pe := range pend {
		s := pe.sym
		checkLen, stubOff := 0, 0
		stubCallRel, stubJmpRel := 0, 0
		preserveArgs := pe.req.abi == obj.ABIInternal
		// llc resolves every .text reference inside its own object itself (no
		// relocation): direct CALLs, direct JMPs (tail jumps, goabi thunks) and
		// function-address LEAs. The meta carries the disassembly-derived
		// offsets, so hand each one to the linker. The naive byte scan cannot
		// be used here — the linker may place symbols in any order, and a
		// mis-decoded 0xe8 would silently rewrite an unrelated instruction.
		//
		// calls and branches are disjoint by contract: only the former are
		// safepoints. A tail jump listed as a call (or a call listed as a
		// branch) fails loudly here instead of shifting the stack map index
		// table under a PC that cannot trigger a growth.
		for _, mc := range pe.calls {
			if off, ok := mc.offset(); ok {
				bakeReloc(pi, pe, off, mc.Callee, true, "call")
			}
		}
		for _, mb := range pe.branches {
			if off, ok := mb.offset(); ok {
				bakeReloc(pi, pe, off, mb.Callee, false, "branch")
			}
		}
		// Alignment thunks reserve their C stack inside their own prologue, so
		// the check must cover that reservation (plus the alignment slack).
		checkFrame := int32(pe.frame)
		if pe.req.cstackAlign {
			checkFrame = 16 // the thunk's two pushes; the reserve is gone
		}
		if gocWantsSplit(pe.req) {
			if !gocBodyTerminates(pe.code) {
				fmt.Fprintf(os.Stderr, "elfpack: note %s frame=%d: body tail is not a ret/jmp/trap (tail byte 0x%02x); trapping fallthrough\n", s.Name, pe.frame, pe.code[len(pe.code)-1])
			}
			pre, branches := gocSplitCheck(checkFrame, preserveArgs)
			var stub []byte
			var callRel, jmpRel int
			if pe.req.abi == obj.ABI0 {
				stub, callRel, jmpRel = sysvStubCode, sysvStubCallRel, sysvStubJmpRel
			} else {
				stub, callRel, jmpRel = gocSplitStub(preserveArgs, pe.req.argSpills)
			}
			stubCallRel, stubJmpRel = callRel, jmpRel
			checkLen = len(pre)
			// 0f 0b (UD2): the stub must never be entered by fallthrough.
			stubOff = checkLen + len(pe.code) + 2
			patchSplitCheck(pre, branches, stubOff)
			code := make([]byte, 0, stubOff+len(stub))
			code = append(code, pre...)
			code = append(code, pe.code...)
			code = append(code, 0x0f, 0x0b)
			code = append(code, stub...)
			s.P = code
			nMorestack++
		} else {
			s.P = append([]byte(nil), pe.code...)
		}
		s.Size = int64(len(s.P))
		s.R = nil

		sites := findCallSites(pe.code, pe.baseOff, rels, syms)
		if checkLen > 0 {
			// PCs shift by the prepended check sequence.
			for i := range sites {
				sites[i].Off += checkLen
			}
		}

		// Reloc translation
		for _, r := range rels {
			if r.Off < pe.baseOff || r.Off >= pe.baseOff+uint64(len(pe.code)) {
				continue
			}
			off := int64(r.Off-pe.baseOff) + int64(checkLen)
			target, baseAdd, ename := resolveTarget(r)
			if target == nil {
				fmt.Fprintf(os.Stderr, "elfpack: FATAL reloc at %s+0x%x with unresolved symbol %q\n", s.Name, off, ename)
				os.Exit(1)
			}
			// GOTPCREL/GOTPCRELX (9/41/42): redirect through a materialized slot.
			if r.Type == 9 || r.Type == 41 || r.Type == 42 {
				target = gotSlot(target, baseAdd)
				baseAdd = 0
				r.Type = uint32(elf.R_X86_64_PC32)
			}
			var typ objabi.RelocType
			switch r.Type {
			case uint32(elf.R_X86_64_PLT32), uint32(elf.R_X86_64_PC32):
				if off > 0 && s.P[off-1] == 0xe8 {
					typ = objabi.R_CALL
				} else {
					typ = objabi.R_PCREL
				}
			case uint32(elf.R_X86_64_32S), uint32(elf.R_X86_64_32):
				typ = objabi.R_ADDR
			default:
				fmt.Fprintf(os.Stderr, "elfpack: FATAL unsupported ELF reloc type %d for %s at +0x%x\n", r.Type, s.Name, off)
				os.Exit(1)
			}
			// resolveTarget already normalized the addend to an absolute
			// offset from the symbol start, which is exactly Go's A'.
			goAdd := baseAdd
			if dbg := os.Getenv("GOC_DBG_RELOC"); dbg != "" && (strings.Contains(s.Name, dbg) || strings.Contains(ename, dbg) || strings.Contains(target.Name, dbg)) {
				fmt.Fprintf(os.Stderr, "elfpack-DBG sym=%s off=0x%x prev=0x%02x ename=%q target=%q elfType=%d baseAdd=%d goAdd=%d typ=%v\n",
					s.Name, off, s.P[off-1], ename, target.Name, r.Type, baseAdd, goAdd, typ)
			}
			if typ == objabi.R_CALL && goAdd != 0 {
				fmt.Fprintf(os.Stderr, "elfpack: FATAL R_CALL addend for %s+0x%x: goAdd=%d want 0 (from ELF A=-4)\n", s.Name, off, goAdd)
				os.Exit(1)
			}
			s.AddRel(ctxt, obj.Reloc{
				Off:  int32(off),
				Siz:  4,
				Type: typ,
				Add:  goAdd,
				Sym:  target,
			})
		}

		// Branches llc had resolved itself, now handed to the linker.
		for _, br := range bakedRels[pi] {
			br.Off += int32(checkLen)
			s.AddRel(ctxt, br)
		}

		// Slow-path stub relocations: CALL runtime.morestack_noctxt (ABI0, as
		// in Go's own split prologue) and JMP back to the function entry.
		if stubOff > 0 {
			s.AddRel(ctxt, obj.Reloc{
				Off:  int32(stubOff + stubCallRel),
				Siz:  4,
				Type: objabi.R_CALL,
				Sym:  ctxt.Lookup("runtime.morestack_noctxt"),
			})
			s.AddRel(ctxt, obj.Reloc{
				Off:  int32(stubOff + stubJmpRel),
				Siz:  4,
				Type: objabi.R_PCREL,
				Sym:  s,
			})
		}

		// Dense PCDATA at CALL safepoints
		fi := s.Func()
		trans := []pcValue{} // start at -1 implicitly
		nSites := len(sites)
		nMeta := len(pe.calls)
		// Only a real CALL opcode is a safepoint: pe.branches (cross-function
		// JMP/Jcc, function-address LEA) is deliberately never consulted here,
		// because a transition under a branch PC would make the runtime read a
		// stack map index for an instruction that cannot grow the stack.
		//
		// A real-body sidecar records the opcode offset of every CALL, so the
		// safepoints are known exactly and indices pair by offset (baked calls
		// included, not just the ones the ELF relocated). Sidecars without
		// offsets (llvmmc fixtures) keep the name/order pairing over relocated
		// CALL sites.
		hasCalls := len(pe.calls) > 0
		exact := hasCalls
		for _, c := range pe.calls {
			if _, ok := c.offset(); !ok {
				exact = false
				break
			}
		}
		if pe.req.localsMap != "" || pe.req.argsMap != "" || hasCalls {
			if exact {
				covered := make(map[int64]bool, nMeta)
				for _, mc := range pe.calls {
					off, _ := mc.offset()
					covered[off] = true
					// A site without a real stack map uses the frame's own map
					// (index 0), exactly as the name-paired path below does: the
					// index only ever comes from meta.calls.
					idx := int32(mc.StackmapIndex)
					if idx < 0 {
						idx = 0
					}
					pc := off + int64(checkLen)
					trans = append(trans, pcValue{PC: pc, Value: idx})
					pcdataProof = append(pcdataProof, fmt.Sprintf("%s+0x%x -> idx %d callee=%s", s.Name, pc, idx, mc.Callee))
				}
				for _, cs := range sites {
					if covered[int64(cs.Off)-int64(checkLen)] {
						continue
					}
					// A relocated CALL the sidecar did not list: keep the
					// safepoint visible with the frame's own map.
					fmt.Fprintf(os.Stderr, "elfpack: note %s: relocated CALL at +0x%x has no meta entry; index 0\n", s.Name, cs.Off)
					trans = append(trans, pcValue{PC: int64(cs.Off), Value: 0})
					pcdataProof = append(pcdataProof, fmt.Sprintf("%s+0x%x -> idx 0 callee=%s (no meta entry)", s.Name, cs.Off, cs.Callee))
				}
			} else {
				// Match CALL sites to meta call annotations by order.
				if nMeta > 0 && nSites != nMeta {
					// Tolerate: meta may only annotate safepoint CALLs; sites includes all CALLs.
					// Pair by callee name when possible.
					fmt.Printf("elfpack: %s CALL sites=%d meta.calls=%d (pairing by order/callee)\n", s.Name, nSites, nMeta)
				}
				used := make([]bool, nMeta)
				for _, cs := range sites {
					idx := -1
					// Prefer matching unused meta entry with same callee
					for i, mc := range pe.calls {
						if used[i] {
							continue
						}
						if mc.Callee == cs.Callee || strings.HasSuffix(cs.Callee, mc.Callee) || strings.HasSuffix(mc.Callee, cs.Callee) {
							idx = mc.StackmapIndex
							used[i] = true
							break
						}
					}
					if idx < 0 {
						// Fallback: next unused meta, else 0 if has maps
						for i, mc := range pe.calls {
							if !used[i] {
								idx = mc.StackmapIndex
								used[i] = true
								break
							}
						}
					}
					if idx < 0 {
						idx = 0
					}
					trans = append(trans, pcValue{PC: int64(cs.Off), Value: int32(idx)})
					pcdataProof = append(pcdataProof, fmt.Sprintf("%s+0x%x -> idx %d callee=%s", s.Name, cs.Off, idx, cs.Callee))
				}
			}
		}
		// Ensure Pcdata slice has Slot for StackMapIndex; fill nil holes (linker SymSize panics on nil).
		need := abi.PCDATA_StackMapIndex + 1
		if len(fi.Pcln.Pcdata) < need {
			n := make([]*obj.LSym, need)
			copy(n, fi.Pcln.Pcdata)
			fi.Pcln.Pcdata = n
		}
		emptyPc := func() *obj.LSym {
			return &obj.LSym{Type: objabi.SRODATA, Attribute: obj.AttrContentAddressable | obj.AttrPcdata}
		}
		for i := range fi.Pcln.Pcdata {
			if fi.Pcln.Pcdata[i] == nil {
				fi.Pcln.Pcdata[i] = emptyPc()
			}
		}
		if checkLen > 0 {
			// The split check (including temporary argument saves) and its
			// retry stub must not be asynchronously preempted mid-sequence.
			fi.Pcln.Pcdata[abi.PCDATA_UnsafePoint] = makePcdataFromTransitions(ctxt, s.Size, []pcValue{
				{PC: 0, Value: abi.UnsafePointUnsafe},
				{PC: int64(checkLen), Value: abi.UnsafePointSafe},
				{PC: int64(stubOff), Value: abi.UnsafePointUnsafe},
			})
		}
		switch {
		case len(pe.req.smapTrans) > 0 || pe.req.stubMapIndex >= 0:
			// Real per-call-site maps: index 0 (the color-pass slots) everywhere,
			// switching to the stack-map state at each recorded call site. The
			// offsets are LLVM's function-relative PCs, so they shift by the
			// prepended check sequence.
			tr := make([]pcValue, 0, len(pe.req.smapTrans)+2)
			tr = append(tr, pcValue{PC: 0, Value: 0})
			for _, tv := range pe.req.smapTrans {
				tr = append(tr, pcValue{PC: tv.PC + int64(checkLen), Value: tv.Value})
			}
			if pe.req.stubMapIndex >= 0 {
				if stubOff == 0 {
					fmt.Fprintf(os.Stderr, "elfpack: FATAL %s has a stub pointer map but no split stub\n", s.Name)
					os.Exit(1)
				}
				tr = append(tr, pcValue{PC: int64(stubOff), Value: pe.req.stubMapIndex})
			}
			fi.Pcln.Pcdata[abi.PCDATA_StackMapIndex] = makePcdataFromTransitions(ctxt, s.Size, tr)
		case len(trans) == 0:
			if pe.req.localsMap != "" || pe.req.argsMap != "" {
				fi.Pcln.Pcdata[abi.PCDATA_StackMapIndex] = makeConstPcsp(ctxt, s.Size, 0)
			} else {
				fi.Pcln.Pcdata[abi.PCDATA_StackMapIndex] = makeConstPcsp(ctxt, s.Size, -1)
			}
		default:
			fi.Pcln.Pcdata[abi.PCDATA_StackMapIndex] = makePcdataFromTransitions(ctxt, s.Size, trans)
		}
		if pe.req.goArgArea > 0 {
			if stubOff == 0 {
				fmt.Fprintf(os.Stderr, "elfpack: FATAL %s has Go register arguments but no stack-split slow path\n", s.Name)
				os.Exit(1)
			}
			// The caller's register spill area is uninitialized on the fast
			// path. Index 0 scans it only after the slow stub stores the
			// arguments; index 1 scans only true stack arguments in the body.
			fi.Pcln.Pcdata[abi.PCDATA_StackMapIndex] = makePcdataFromTransitions(ctxt, s.Size, []pcValue{
				{PC: 0, Value: 0},
				{PC: int64(checkLen), Value: 1},
				{PC: int64(stubOff), Value: 0},
			})
		}
	}

	// pcsp: PUSH BP (+8) + SUB $frame → spdelta = frame+8 when frame>0
	for _, pe := range pend {
		s := pe.sym
		fi := s.Func()
		fi.Locals = int32(pe.frame)
		s.Set(obj.AttrNoFrame, pe.frame == 0)
		spdelta := int32(pe.frame)
		if pe.frame > 0 {
			// Prove LLVM bytes contain Go-style prologue: 55 (PUSH BP) near start
			if len(pe.code) < 4 || pe.code[0] != 0x55 {
				fmt.Fprintf(os.Stderr, "elfpack: FATAL %s frame=%d but TEXT does not start with PUSH BP (0x55); pcsp would lie\n", s.Name, pe.frame)
				os.Exit(1)
			}
			spdelta = int32(pe.frame) + 8
		}
		checkLen := 0
		if gocWantsSplit(pe.req) {
			cf := int32(pe.frame)
			if pe.req.cstackAlign {
				cf = 16
			}
			pre, _ := gocSplitCheck(cf, pe.req.abi == obj.ABIInternal)
			checkLen = len(pre)
		}
		if checkLen > 0 {
			// The unwinder computes frame.fp = sp + pcsp + 8 and reads the saved
			// frame pointer at fp-16, so pcsp must equal (the caller's frame
			// boundary) - sp - 8 at every PC it may stop at:
			//   check/stub: 0 on entry, +8/+16 while the R10/R11 saves are
			//               pushed, then 0 again before morestack
			//   body:       SP = entry-8-BP-locals at a call      -> frame
			// (frame from the meta = 8 for the pushed BP + the LLVM locals).
			bodyDelta := int32(pe.frame)
			if pe.req.cstackAlign {
				// The alignment thunk's frame: [A-8] caller SP, [A-16] caller BP,
				// so the caller's boundary (fp) is A and sp at the inner call is
				// A-16 -> pcsp = 8 (A cancels out, keeping this a constant).
				bodyDelta = 8
			}
			sp := []pcValue{
				{PC: 0, Value: 0},
				{PC: int64(checkLen), Value: bodyDelta},
			}
			stubOff := int64(checkLen) + int64(len(pe.code)) + 2
			if pe.req.abi == obj.ABIInternal {
				sp = append(sp,
					pcValue{PC: 2, Value: 8},
					pcValue{PC: 4, Value: 16},
					pcValue{PC: int64(checkLen) - 2, Value: 8},
					pcValue{PC: stubOff, Value: 16},
					pcValue{PC: stubOff + 2, Value: 8},
					pcValue{PC: stubOff + 4, Value: 0})
			} else {
				for _, state := range sysvStubSP {
					sp = append(sp, pcValue{PC: stubOff + state.PC, Value: state.Value})
				}
			}
			fi.Pcln.Pcsp = makePcdataFromTransitions(ctxt, s.Size, sp)
			// The function now splits instead of relying on the nosplit budget
			// that x86.preprocess' leaf heuristic forced on it.
			s.Set(obj.AttrNoSplit, false)
		} else {
			if gocLeafNosplit(pe.req) {
				s.Set(obj.AttrNoSplit, true)
			}
			fi.Pcln.Pcsp = makeConstPcsp(ctxt, s.Size, spdelta)
		}
		fi.Pcln.Pcfile = makeConstPcsp(ctxt, s.Size, 1)
		fi.Pcln.Pcline = makeConstPcsp(ctxt, s.Size, 1)
		fmt.Printf("elfpack: pcsp %s size=%d spdelta=%d (frame=%d, check=%d)\n", s.Name, s.Size, spdelta, pe.frame, checkLen)
	}
	if useMorestack {
		fmt.Printf("elfpack: morestack preamble on %d/%d TEXT symbols; relocated %d baked calls\n", nMorestack, len(pend), nBaked)
	}

	ctxt.NumberSyms()
	buf, err := bio.Create(*outO)
	if err != nil {
		panic(err)
	}
	buf.WriteString(objabi.HeaderString())
	fmt.Fprintf(buf, "!\n")
	obj.WriteObjFile(ctxt, buf)
	if err := buf.Close(); err != nil {
		panic(err)
	}
	fmt.Println("elfpack: wrote goobj from LLVM MC ELF →", *outO)
	fmt.Println("elfpack: encoding=llvm-llc-mc (AsmPrinter/MCCodeEmitter); dense PCDATA at CALLs")
	// Write proof sidecar for build.sh
	proofPath := filepath.Join(filepath.Dir(*outO), "pcdata_proof.txt")
	_ = os.WriteFile(proofPath, []byte(strings.Join(pcdataProof, "\n")+"\n"), 0644)
	fmt.Println("elfpack: PCDATA proof →", proofPath, "lines=", len(pcdataProof))
}
