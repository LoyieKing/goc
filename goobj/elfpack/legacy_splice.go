package elfpack

import (
	"encoding/binary"
	"fmt"
	"math"
	"os"
	"strconv"
	"strings"

	"github.com/LoyieKing/goc/goobj/enc/stdinternal/abi"
)

// Stack-check splice: the entry check against g.stackguard0 and the
// morestack stub that elfpack writes into each function's machine code.
// Moved here verbatim from elfpack.go (plan row 5). It goes away in plan
// row 6a, when goc-llc emits the check and stub itself (GoFrameLowering).

// gocSplitCheck returns the entry check sequence plus the byte offsets of the
// rel32 fields of its branches to the stub. The stub is emitted after the body,
// so the caller patches them with patchSplitCheck once that offset is known.
func gocSplitCheck(frame int32, preserveArgs bool) ([]byte, []int) {
	if gocIsArm64() {
		// X16/X17 are not argument registers, so preserveArgs is unused.
		return arm64SplitCheck(frame)
	}
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
	if gocIsArm64() {
		arm64PatchCond(b, branches, stub)
		return
	}
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
	if gocIsArm64() {
		return arm64SplitStub(argSpills)
	}
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
// the register itself. GOC_CSR_ADJUST=1 is the other contract: the stub
// adds the copy delta to a saved RBX/R12/R13/R15/R14 only when the value
// lies in the old stack. Small integers and heap pointers are outside that
// range, so they are not rewritten and are still not marked.
func gocSysvSplitStub() (stub []byte, callRel, jmpRel int, sp []pcValue) {
	if os.Getenv("GOC_CSR_ADJUST") == "1" {
		return gocSysvSplitStubAdjust()
	}
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

// gocSysvSplitStubAdjust is gocSysvSplitStub plus a post-morestack rewrite of
// the five saved callee-saved GPRs. The 128-byte XMM area grows by 16 so the
// old stack bounds survive the copy as unmarked integers. Offsets at the
// CALL, from SP: XMM 0..127, old lo 128, old hi 136, then R14, R15, R13,
// R12, RBX. Argument registers stay unmarked here; the stub bitmap already
// covers them, and adjusting them again would apply the delta twice.
func gocSysvSplitStubAdjust() (stub []byte, callRel, jmpRel int, sp []pcValue) {
	pushes := [][]byte{{0x55}, {0x50}, {0x57}, {0x56}, {0x52}, {0x51}, {0x41, 0x50}, {0x41, 0x51}}
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
	stub = append(stub, 0x48, 0x81, 0xec, 0x90, 0, 0, 0) // SUBQ $144, SP
	delta += 144
	sp = append(sp, pcValue{PC: int64(len(stub)), Value: int32(delta)})
	for r := byte(0); r < 8; r++ {
		stub = append(stub, 0xf3, 0x0f, 0x7f) // MOVDQU XMMr, disp(SP)
		if r == 0 {
			stub = append(stub, 0x04, 0x24)
		} else {
			stub = append(stub, 0x44|(r<<3), 0x24, r*16)
		}
	}
	// Save g.stack.lo/hi, then CALL. Bytes are the llvm-mc encoding of
	// MOVQ FS:-8, R14; MOVQ (R14), R11; MOVQ 8(R14), R10;
	// MOVQ R11, 128(SP); MOVQ R10, 136(SP); CALL rel32.
	stub = append(stub,
		0x64, 0x4c, 0x8b, 0x34, 0x25, 0xf8, 0xff, 0xff, 0xff,
		0x4d, 0x8b, 0x1e,
		0x4d, 0x8b, 0x56, 0x08,
		0x4c, 0x89, 0x9c, 0x24, 0x80, 0, 0, 0,
		0x4c, 0x89, 0x94, 0x24, 0x88, 0, 0, 0,
	)
	callRel = len(stub) + 1
	stub = append(stub, 0xe8, 0, 0, 0, 0)
	for r := byte(0); r < 8; r++ {
		stub = append(stub, 0xf3, 0x0f, 0x6f) // MOVDQU disp(SP), XMMr
		if r == 0 {
			stub = append(stub, 0x04, 0x24)
		} else {
			stub = append(stub, 0x44|(r<<3), 0x24, r*16)
		}
	}
	// delta = new_lo - old_lo. A zero delta (preempt, no copy) skips the
	// compares. Each saved CSR is rewritten only when old_lo <= value < old_hi.
	// llvm-mc emitted the JE as rel32 because five slots do not fit in rel8.
	stub = append(stub,
		0x64, 0x4c, 0x8b, 0x1c, 0x25, 0xf8, 0xff, 0xff, 0xff, // MOVQ FS:-8, R11
		0x4d, 0x8b, 0x1b, // MOVQ (R11), R11
		0x4c, 0x2b, 0x9c, 0x24, 0x80, 0, 0, 0, // SUBQ 128(SP), R11
		0x0f, 0x84, 0xb2, 0, 0, 0, // JE skip
		0x4c, 0x8b, 0x94, 0x24, 0x88, 0, 0, 0, // MOVQ 136(SP), R10
	)
	for _, disp := range []byte{0x90, 0x98, 0xa0, 0xa8, 0xb0} {
		stub = append(stub,
			0x48, 0x8b, 0x84, 0x24, disp, 0, 0, 0, // MOVQ disp(SP), AX
			0x48, 0x3b, 0x84, 0x24, 0x80, 0, 0, 0, // CMPQ AX, 128(SP)
			0x72, 0x10, // JB next
			0x4c, 0x39, 0xd0, // CMPQ R10, AX
			0x73, 0x0b, // JAE next
			0x4c, 0x01, 0xd8, // ADDQ R11, AX
			0x48, 0x89, 0x84, 0x24, disp, 0, 0, 0, // MOVQ AX, disp(SP)
		)
	}
	stub = append(stub, 0x48, 0x81, 0xc4, 0x90, 0, 0, 0) // ADDQ $144, SP
	delta -= 144
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
	stub = append(stub, 0xe9, 0, 0, 0, 0)
	if delta != 0 {
		panic(fmt.Sprintf("sysv morestack stub SP delta %d, want 0", delta))
	}
	return stub, callRel, jmpRel, sp
}

var sysvStubCode, sysvStubCallRel, sysvStubJmpRel, sysvStubSP = gocSysvSplitStub()
