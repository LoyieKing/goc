// linux/arm64 goobj packing.
//
// llc emits AArch64. This file does not run the Go assembler on those bytes:
// Preprocess and Assemble are empty, and the ELF text is copied into s.P
// after Flushplist, the same way the amd64 path discards span6. Reloc types
// are the arm64 ones (R_CALLARM64, R_ADDRARM64, ...) because the Go linker
// ORs an immediate into the instruction; an amd64 R_CALL would mis-patch it.
//
// g is X28 for the whole TU (llc -mattr=+reserve-x28). The split check reads
// stackguard0 at [X28, #16] and never loads TLS. Pointer color is unchanged.
//
// Frame-address repair (GocFrameAddrFix / the x86 leaq asm) is not ported.
// GOC_SPTR_MAPS is refused before we get here. A function that arrives with
// sptr slots is a fatal error rather than a wrong bitmap.
package main

import (
	"encoding/binary"
	"fmt"
	"os"
	"sort"

	"goc.local/p5-machinepass-goobj/goobj/enc/cmd/objlib/obj"
	"goc.local/p5-machinepass-goobj/goobj/enc/cmd/objlib/objabi"
	"goc.local/p5-machinepass-goobj/goobj/enc/cmd/objlib/sys"
	"goc.local/p5-machinepass-goobj/goobj/enc/stdinternal/abi"
)

// gocArch is "amd64" or "arm64", taken from the ELF machine and checked
// against GOARCH. The default stays amd64 so a host-built elfpack that is
// handed an x86 object keeps today's bytes. HeaderString follows GOARCH,
// which realbody sets to this same value.
var gocArch = "amd64"

func gocIsArm64() bool { return gocArch == "arm64" }

func arm64Nop(*obj.Link)                              {}
func arm64NopSym(*obj.Link, *obj.LSym, obj.ProgAlloc) {}

// linkARM64 has no encoder. Nil Preprocess/Assemble panics in Flushplist;
// nil ErrorCheck is fine.
var linkARM64 = obj.LinkArch{
	Arch:       sys.ArchARM64,
	Init:       arm64Nop,
	Preprocess: arm64NopSym,
	Assemble:   arm64NopSym,
}

func arm64Fatal(format string, args ...interface{}) {
	fmt.Fprintf(os.Stderr, "elfpack: FATAL "+format+"\n", args...)
	os.Exit(1)
}

func putInsn(b []byte, w uint32) []byte {
	var buf [4]byte
	binary.LittleEndian.PutUint32(buf[:], w)
	return append(b, buf[:]...)
}

func insnAt(b []byte, off int) uint32 {
	return binary.LittleEndian.Uint32(b[off : off+4])
}

func putInsnAt(b []byte, off int, w uint32) {
	binary.LittleEndian.PutUint32(b[off:], w)
}

// Encodings below were checked against llvm-mc -triple=aarch64-unknown-linux-gnu.
const (
	arm64LdrX16X28 = 0xF9400B90 // ldr x16, [x28, #16]  stackguard0
	arm64CmpSpX16  = 0xEB3063FF // cmp sp, x16
	arm64SubsX17   = 0xEB3163F1 // subs x17, sp, x17
	arm64MovX3X30  = 0xAA1E03E3 // mov x3, x30
	arm64BL        = 0x94000000 // bl #0  (imm26 must stay 0; the linker ORs)
	arm64B         = 0x14000000 // b  #0
	arm64BLS       = 0x54000009 // b.ls #0
	arm64BLO       = 0x54000003 // b.lo #0
	arm64Brk1      = 0xD4200020 // brk #1
	arm64Ret       = 0xD65F03C0
)

func arm64Trap() []byte { return putInsn(nil, arm64Brk1) }

// arm64SubImm is SUB Xd, Xn, #imm12 (no shift). SP is register 31.
func arm64SubImm(rd, rn int, imm uint32) uint32 {
	if imm > 4095 {
		arm64Fatal("SUB immediate %d does not fit imm12", imm)
	}
	return 0xD1000000 | (imm << 10) | uint32(rn)<<5 | uint32(rd)
}

// arm64AddImm is ADD Xd, Xn, #imm12.
func arm64AddImm(rd, rn int, imm uint32) uint32 {
	if imm > 4095 {
		arm64Fatal("ADD immediate %d does not fit imm12", imm)
	}
	return 0x91000000 | (imm << 10) | uint32(rn)<<5 | uint32(rd)
}

// arm64CmpReg is CMP Xn, Xm (SUBS XZR, Xn, Xm), neither operand SP.
func arm64CmpReg(rn, rm int) uint32 {
	return 0xEB000000 | uint32(rm)<<16 | uint32(rn)<<5 | 31
}

// arm64MovWide materialises v in Xd with MOVZ + MOVK. X16 stays free
// (it holds the stack guard); the caller passes X17.
func arm64MovWide(rd int, v uint64) []byte {
	var b []byte
	first := true
	for hw := uint32(0); hw < 4; hw++ {
		chunk := uint32((v >> (hw * 16)) & 0xFFFF)
		if chunk == 0 && !first {
			continue
		}
		op := uint32(0xF2800000) // MOVK
		if first {
			op = 0xD2800000 // MOVZ
			first = false
		}
		b = putInsn(b, op|(hw<<21)|(chunk<<5)|uint32(rd))
	}
	if first {
		b = putInsn(b, 0xD2800000|uint32(rd))
	}
	return b
}

// arm64SplitCheck is the entry probe. It does not touch SP.
// X16 holds the guard, X17 is the only other scratch (X27 is Go's REGTMP
// and still holds a callee-saved value). Branches are instruction offsets
// of B.cond, patched by arm64PatchCond once the stub offset is known.
// preserveArgs is unused: X16/X17 are not argument registers.
func arm64SplitCheck(frame int32) ([]byte, []int) {
	var b []byte
	var branches []int
	b = putInsn(b, arm64LdrX16X28)
	switch {
	case frame <= abi.StackSmall:
		b = putInsn(b, arm64CmpSpX16)
	case frame <= abi.StackBig:
		// SUB X17, SP, #(frame-StackSmall); CMP X17, X16.
		// frame-StackSmall is at most 3968, which fits in imm12.
		b = putInsn(b, arm64SubImm(17, 31, uint32(frame-abi.StackSmall)))
		b = putInsn(b, arm64CmpReg(17, 16))
	default:
		b = append(b, arm64MovWide(17, uint64(frame-abi.StackSmall))...)
		b = putInsn(b, arm64SubsX17)
		branches = append(branches, len(b))
		b = putInsn(b, arm64BLO)
		b = putInsn(b, arm64CmpReg(17, 16))
	}
	branches = append(branches, len(b))
	b = putInsn(b, arm64BLS)
	return b, branches
}

// arm64PatchCond writes a 19-bit PC-relative displacement into each B.cond.
// imm19 is a word offset from the instruction itself. Range is ±1MB.
func arm64PatchCond(b []byte, branches []int, stub int) {
	for _, off := range branches {
		dist := stub - off
		if dist%4 != 0 || dist >= 1<<20 || dist < -(1<<20) {
			arm64Fatal("conditional branch at +%d cannot reach stub +%d", off, stub)
		}
		imm19 := uint32(dist>>2) & 0x7ffff
		w := insnAt(b, off)
		w = (w &^ 0x00ffffe0) | (imm19 << 5)
		putInsnAt(b, off, w)
	}
}

// arm64StpPre is STP Xt, Xt2, [sp, #-16]!.
func arm64StpPre(rt, rt2 int) uint32 {
	const imm7 = 0x7e // -2, scaled by 8 → -16
	return 0xA9800000 | imm7<<15 | uint32(rt2)<<10 | 31<<5 | uint32(rt)
}

// arm64LdpPost is LDP Xt, Xt2, [sp], #16.
func arm64LdpPost(rt, rt2 int) uint32 {
	const imm7 = 2
	return 0xA8C00000 | imm7<<15 | uint32(rt2)<<10 | 31<<5 | uint32(rt)
}

// arm64StpQ / arm64LdpQ are the 128-bit signed-offset pair at [sp, #byteOff].
func arm64StpQ(rt, rt2, byteOff int) uint32 {
	return 0xAD000000 | uint32(byteOff/16)<<15 | uint32(rt2)<<10 | 31<<5 | uint32(rt)
}

func arm64LdpQ(rt, rt2, byteOff int) uint32 {
	return 0xAD400000 | uint32(byteOff/16)<<15 | uint32(rt2)<<10 | 31<<5 | uint32(rt)
}

// arm64ArgSpill stores or reloads one ABIInternal register argument at
// [sp, #off]. SP does not change: the slot is in the caller's spill area
// (entry SP+8 and up), so the stub's pcsp stays 0.
func arm64ArgSpill(a metaArgSpill, load bool) []byte {
	if a.Off < 8 || a.Off > 0xfff {
		arm64Fatal("invalid arm64 argument spill %+v", a)
	}
	kind, n, ok := arm64Reg(a.Reg)
	if !ok || n > 30 {
		arm64Fatal("unsupported arm64 argument spill %+v", a)
	}
	var w uint32
	switch {
	case kind == "int" && a.Size == 8:
		if a.Off%8 != 0 {
			arm64Fatal("misaligned x-spill %+v", a)
		}
		base := uint32(0xF9000000)
		if load {
			base = 0xF9400000
		}
		w = base | uint32(a.Off/8)<<10 | 31<<5 | uint32(n)
	case kind == "int" && a.Size == 4:
		if a.Off%4 != 0 {
			arm64Fatal("misaligned w-spill %+v", a)
		}
		base := uint32(0xB9000000)
		if load {
			base = 0xB9400000
		}
		w = base | uint32(a.Off/4)<<10 | 31<<5 | uint32(n)
	case kind == "int" && a.Size == 2:
		if a.Off%2 != 0 {
			arm64Fatal("misaligned h-spill %+v", a)
		}
		base := uint32(0x79000000)
		if load {
			base = 0x79400000
		}
		w = base | uint32(a.Off/2)<<10 | 31<<5 | uint32(n)
	case kind == "int" && a.Size == 1:
		base := uint32(0x39000000)
		if load {
			base = 0x39400000
		}
		w = base | uint32(a.Off)<<10 | 31<<5 | uint32(n)
	case kind == "float" && a.Size == 8:
		if a.Off%8 != 0 {
			arm64Fatal("misaligned d-spill %+v", a)
		}
		base := uint32(0xFD000000)
		if load {
			base = 0xFD400000
		}
		w = base | uint32(a.Off/8)<<10 | 31<<5 | uint32(n)
	case kind == "float" && a.Size == 4:
		if a.Off%4 != 0 {
			arm64Fatal("misaligned s-spill %+v", a)
		}
		base := uint32(0xBD000000)
		if load {
			base = 0xBD400000
		}
		w = base | uint32(a.Off/4)<<10 | 31<<5 | uint32(n)
	default:
		arm64Fatal("unsupported arm64 argument spill %+v", a)
	}
	return putInsn(nil, w)
}

func arm64Reg(name string) (kind string, n int, ok bool) {
	if len(name) < 2 {
		return "", 0, false
	}
	switch name[0] {
	case 'x', 'w':
		kind = "int"
	case 'd', 's', 'q', 'v':
		kind = "float"
	default:
		return "", 0, false
	}
	for _, c := range name[1:] {
		if c < '0' || c > '9' {
			return "", 0, false
		}
		n = n*10 + int(c-'0')
	}
	return kind, n, true
}

// arm64SplitStub is the ABIInternal slow path. Argument registers are
// stored above entry SP, X30 is handed to morestack in X3, and the
// retry branch is a relocatable B with imm26 left 0.
func arm64SplitStub(argSpills []metaArgSpill) (stub []byte, callRel, jmpRel int) {
	for _, a := range argSpills {
		stub = append(stub, arm64ArgSpill(a, false)...)
	}
	stub = putInsn(stub, arm64MovX3X30)
	callRel = len(stub)
	stub = putInsn(stub, arm64BL)
	for _, a := range argSpills {
		stub = append(stub, arm64ArgSpill(a, true)...)
	}
	jmpRel = len(stub)
	stub = putInsn(stub, arm64B)
	return stub, callRel, jmpRel
}

// arm64SysvSplitStub saves the AAPCS argument registers around morestack.
// There is no Go spill area. X28 (g) is not saved. X19–X28 other than the
// argument set are callee-saved for a normal Go newstack; gogo restores
// X29 and LR from the values morestack recorded, and clobbers X0, so X0
// is in the save set. SP steps are recorded for pcsp. The retry branch
// is a reloc, so jmpRel is the B and not negative.
func arm64SysvSplitStub() (stub []byte, callRel, jmpRel int, sp []pcValue) {
	sp = append(sp, pcValue{PC: 0, Value: 0})
	delta := 0
	pairs := [][2]int{{0, 1}, {2, 3}, {4, 5}, {6, 7}, {8, 31}}
	for _, p := range pairs {
		stub = putInsn(stub, arm64StpPre(p[0], p[1]))
		delta += 16
		sp = append(sp, pcValue{PC: int64(len(stub)), Value: int32(delta)})
	}
	stub = putInsn(stub, arm64SubImm(31, 31, 128))
	delta += 128
	sp = append(sp, pcValue{PC: int64(len(stub)), Value: int32(delta)})
	for i := 0; i < 8; i += 2 {
		stub = putInsn(stub, arm64StpQ(i, i+1, i*16))
	}
	stub = putInsn(stub, arm64MovX3X30)
	callRel = len(stub)
	stub = putInsn(stub, arm64BL)
	for i := 6; i >= 0; i -= 2 {
		stub = putInsn(stub, arm64LdpQ(i, i+1, i*16))
	}
	stub = putInsn(stub, arm64AddImm(31, 31, 128))
	delta -= 128
	sp = append(sp, pcValue{PC: int64(len(stub)), Value: int32(delta)})
	for i := len(pairs) - 1; i >= 0; i-- {
		stub = putInsn(stub, arm64LdpPost(pairs[i][0], pairs[i][1]))
		delta -= 16
		sp = append(sp, pcValue{PC: int64(len(stub)), Value: int32(delta)})
	}
	jmpRel = len(stub)
	stub = putInsn(stub, arm64B)
	if delta != 0 {
		panic(fmt.Sprintf("arm64 sysv morestack stub SP delta %d, want 0", delta))
	}
	return stub, callRel, jmpRel, sp
}

func arm64BodyTerminates(code []byte) bool {
	if len(code) < 4 || len(code)%4 != 0 {
		return false
	}
	w := insnAt(code, len(code)-4)
	if w == arm64Ret || w>>26 == 0x05 {
		return true
	}
	// BRK #imm
	return w&0xFFE0001F == 0xD4200000
}

func arm64IsIndirectCall(code []byte, off int64) bool {
	if off < 0 || off+4 > int64(len(code)) || off%4 != 0 {
		return false
	}
	// BLR Xn
	return insnAt(code, int(off))&0xFFFFFC1F == 0xD63F0000
}

func decodeBakedRefARM64(code []byte, off int64) (bakedRef, bool) {
	if off < 0 || off+4 > int64(len(code)) || off%4 != 0 {
		return bakedRef{}, false
	}
	switch insnAt(code, int(off)) >> 26 {
	case 0x25: // BL
		return bakedRef{dispOff: off, insnLen: 4, isCall: true, pcAtInsn: true, imm26: true}, true
	case 0x05: // B
		return bakedRef{dispOff: off, insnLen: 4, pcAtInsn: true, imm26: true}, true
	}
	return bakedRef{}, false
}

// arm64PrologueDelta sums the SP drop of a clang -frame-pointer=all
// prologue: SUB SP,SP,#N and pre-index STP/STR to SP. mov/add x29 and
// non-writeback stores of callee-saves contribute 0. The first other
// instruction ends the prologue. The constant-pcsp table still covers
// the whole function with this one delta (wrong on the first prologue
// instruction, same class of lie as the amd64 PUSH+SUB table).
func arm64PrologueDelta(code []byte) int {
	delta := 0
	for off := 0; off+4 <= len(code); off += 4 {
		d, ok := arm64PrologueStep(insnAt(code, off))
		if !ok {
			break
		}
		delta += d
	}
	return delta
}

func arm64PrologueStep(w uint32) (int, bool) {
	rd := int(w & 31)
	rn := int((w >> 5) & 31)
	// SUB (immediate) SP, SP. Bit 22 is the optional LSL #12.
	if uint32(w>>23) == 0x1A2 && rd == 31 && rn == 31 {
		imm := int((w >> 10) & 0xFFF)
		if (w>>22)&1 == 1 {
			imm <<= 12
		}
		return imm, true
	}
	// ADD (immediate) X29, SP: mov x29, sp / add x29, sp, #N.
	if uint32(w>>23) == 0x122 && rd == 29 && rn == 31 {
		return 0, true
	}
	// STP 64-bit pre-index to SP. imm7 is scaled by 8; a negative
	// immediate is the amount SP decreases.
	if w&0xFFC00000 == 0xA9800000 && rn == 31 {
		imm7 := int((w >> 15) & 0x7F)
		if imm7 >= 64 {
			imm7 -= 128
		}
		if imm7 >= 0 {
			return 0, false
		}
		return -imm7 * 8, true
	}
	// Non-writeback stores to SP: callee-saved saves after the SUB.
	if rn == 31 && (w&0xFFC00000 == 0xA9000000 || arm64UnsignedStore(w)) {
		return 0, true
	}
	return 0, false
}

func arm64UnsignedStore(w uint32) bool {
	switch w >> 22 {
	case 0x3E4, 0x2E4, 0x1E4, 0x0E4: // str x/w/h/b
		return true
	}
	return false
}

// arm64ApplyPcsp writes the arm64 pcsp table.
//
// On an LR machine the unwinder uses frame.fp = frame.sp + spdelta with
// no extra +8. Entry SP is the caller's SP. After a prologue that drops
// SP by N, spdelta is N. The check and the ABIInternal stub run at
// spdelta 0 (they do not allocate). An ABI0 stub records each 16-byte
// save. fi.Locals stays the SP decrement.
func arm64ApplyPcsp(ctxt *obj.Link, s *obj.LSym, code []byte, frame int, req textReq) {
	fi := s.Func()
	fi.Locals = int32(frame)
	s.Set(obj.AttrNoFrame, frame == 0)
	if req.cstackAlign {
		arm64Fatal("%s: cstack_align is an x86 thunk and is not used on arm64", s.Name)
	}
	if frame%16 != 0 {
		arm64Fatal("%s: frame %d is not a multiple of 16", s.Name, frame)
	}
	if len(code)%4 != 0 || s.Size%4 != 0 {
		arm64Fatal("%s: arm64 text size %d (body %d) is not a multiple of 4", s.Name, s.Size, len(code))
	}
	if frame > 0 {
		if d := arm64PrologueDelta(code); d != frame {
			arm64Fatal("%s: prologue SP delta %d != meta frame %d", s.Name, d, frame)
		}
	}
	spdelta := int32(frame)
	checkLen := 0
	if gocWantsSplit(req) {
		pre, _ := arm64SplitCheck(int32(frame))
		checkLen = len(pre)
	}
	if checkLen > 0 {
		bodyDelta := int32(frame)
		stubOff := int64(checkLen) + int64(len(code)) + int64(len(arm64Trap()))
		sp := []pcValue{
			{PC: 0, Value: 0},
			{PC: int64(checkLen), Value: bodyDelta},
		}
		if req.abi == obj.ABI0 {
			_, _, _, steps := arm64SysvSplitStub()
			for _, st := range steps {
				sp = append(sp, pcValue{PC: stubOff + st.PC, Value: st.Value})
			}
		} else {
			sp = append(sp, pcValue{PC: stubOff, Value: 0})
		}
		fi.Pcln.Pcsp = makePcdataFromTransitions(ctxt, s.Size, sp)
		s.Set(obj.AttrNoSplit, false)
	} else {
		if gocLeafNosplit(req) {
			s.Set(obj.AttrNoSplit, true)
		}
		fi.Pcln.Pcsp = makeConstPcsp(ctxt, s.Size, spdelta)
	}
	fi.Pcln.Pcfile = makeConstPcsp(ctxt, s.Size, 1)
	fi.Pcln.Pcline = makeConstPcsp(ctxt, s.Size, 1)
	fmt.Printf("elfpack: pcsp %s size=%d spdelta=%d (frame=%d, check=%d)\n", s.Name, s.Size, spdelta, frame, checkLen)
}

func arm64CheckRelocs(rels []rela) int {
	n := 0
	for _, r := range rels {
		switch r.Type {
		case uint32(elfAARCH64CALL26), uint32(elfAARCH64JUMP26):
			if r.Add != 0 {
				arm64Fatal("CALL26/JUMP26 at 0x%x addend=%d want 0 (linker ORs imm26)", r.Off, r.Add)
			}
			n++
		case uint32(elfAARCH64ADRPREL), uint32(elfAARCH64ADDLO),
			uint32(elfAARCH64GOTPAGE), uint32(elfAARCH64GOTLO),
			uint32(elfAARCH64ABS64), uint32(elfAARCH64ABS32),
			uint32(elfAARCH64LDST8), uint32(elfAARCH64LDST16),
			uint32(elfAARCH64LDST32), uint32(elfAARCH64LDST64):
			n++
		}
	}
	return n
}

// ELF relocation type numbers. debug/elf's R_AARCH64 values match these;
// the numeric constants keep the reloc switch obvious next to the linker.
const (
	elfAARCH64ABS64   = 257
	elfAARCH64ABS32   = 258
	elfAARCH64PREL64  = 260
	elfAARCH64PREL32  = 261
	elfAARCH64ADRPREL = 275 // ADR_PREL_PG_HI21
	elfAARCH64ADDLO   = 277 // ADD_ABS_LO12_NC
	elfAARCH64LDST8   = 278
	elfAARCH64JUMP26  = 282
	elfAARCH64CALL26  = 283
	elfAARCH64LDST16  = 284
	elfAARCH64LDST32  = 285
	elfAARCH64LDST64  = 286
	elfAARCH64GOTPAGE = 311 // ADR_GOT_PAGE
	elfAARCH64GOTLO   = 312 // LD64_GOT_LO12_NC
)

func arm64DataReloc(typ uint32, add int64, where string, off uint64) (objabi.RelocType, int, int64) {
	switch typ {
	case elfAARCH64ABS64:
		return objabi.R_ADDR, 8, add
	case elfAARCH64ABS32:
		return objabi.R_ADDR, 4, add
	case elfAARCH64PREL64, elfAARCH64PREL32:
		arm64Fatal("PC-relative data reloc type %d at %s+0x%x is not lowered on arm64", typ, where, off)
	default:
		arm64Fatal("unsupported arm64 data reloc type %d at %s+0x%x", typ, where, off)
	}
	return 0, 0, 0
}

type elfRelocTarget func(r rela) (*obj.LSym, int64, string)

// arm64EmitTextRelocs translates one function's .rela.text entries.
// ADRP+ADD (or ADRP+LDST, ADRP+LDR GOT) is one Go reloc of size 8 at the
// ADRP. CALL26 and JUMP26 are R_CALLARM64 of size 4 at the instruction,
// with imm26 cleared: the linker ORs the displacement and does not clear
// a leftover immediate. The addend is a byte offset from the symbol, not
// the x86 A+4 adjustment.
func arm64EmitTextRelocs(ctxt *obj.Link, s *obj.LSym, code []byte, baseOff uint64, checkLen int, rels []rela, resolve elfRelocTarget) {
	var mine []rela
	for _, r := range rels {
		if r.Off >= baseOff && r.Off < baseOff+uint64(len(code)) {
			mine = append(mine, r)
		}
	}
	for i := 1; i < len(mine); i++ {
		j := i
		for j > 0 && mine[j].Off < mine[j-1].Off {
			mine[j], mine[j-1] = mine[j-1], mine[j]
			j--
		}
	}
	used := make([]bool, len(mine))
	for i, r := range mine {
		if used[i] {
			continue
		}
		bodyOff := int(r.Off - baseOff)
		if bodyOff%4 != 0 || bodyOff+4 > len(code) {
			arm64Fatal("%s: reloc at +0x%x is not a 4-byte instruction", s.Name, bodyOff)
		}
		insnOff := bodyOff + checkLen
		target, add, ename := resolve(r)
		if target == nil {
			arm64Fatal("%s: reloc at +0x%x unresolved symbol %q", s.Name, insnOff, ename)
		}
		var next *rela
		nextI := -1
		if i+1 < len(mine) && mine[i+1].Off == r.Off+4 {
			next = &mine[i+1]
			nextI = i + 1
		}
		switch r.Type {
		case elfAARCH64CALL26, elfAARCH64JUMP26:
			if add != 0 {
				arm64Fatal("%s: R_CALLARM64 at +0x%x addend %d want 0", s.Name, insnOff, add)
			}
			w := insnAt(s.P, insnOff)
			putInsnAt(s.P, insnOff, w&^0x03ffffff)
			s.AddRel(ctxt, obj.Reloc{
				Off:  int32(insnOff),
				Siz:  4,
				Type: objabi.R_CALLARM64,
				Add:  0,
				Sym:  target,
			})
		case elfAARCH64ADRPREL:
			if next == nil {
				arm64Fatal("%s: ADRP at +0x%x has no following LO12 reloc", s.Name, insnOff)
			}
			typ, ok := arm64PagePair(next.Type)
			if !ok {
				arm64Fatal("%s: ADRP at +0x%x followed by reloc type %d", s.Name, insnOff, next.Type)
			}
			if next.Sym != r.Sym || next.Add != r.Add {
				arm64Fatal("%s: ADRP/LO12 pair at +0x%x targets differ", s.Name, insnOff)
			}
			arm64ClearPagePair(s.P, insnOff)
			used[nextI] = true
			s.AddRel(ctxt, obj.Reloc{
				Off:  int32(insnOff),
				Siz:  8,
				Type: typ,
				Add:  add,
				Sym:  target,
			})
		case elfAARCH64GOTPAGE:
			if next == nil || next.Type != elfAARCH64GOTLO {
				arm64Fatal("%s: ADRP GOT at +0x%x has no LD64_GOT_LO12", s.Name, insnOff)
			}
			if next.Sym != r.Sym || next.Add != r.Add {
				arm64Fatal("%s: GOT pair at +0x%x targets differ", s.Name, insnOff)
			}
			arm64ClearPagePair(s.P, insnOff)
			used[nextI] = true
			s.AddRel(ctxt, obj.Reloc{
				Off:  int32(insnOff),
				Siz:  8,
				Type: objabi.R_ARM64_GOTPCREL,
				Add:  add,
				Sym:  target,
			})
		case elfAARCH64ABS64:
			s.AddRel(ctxt, obj.Reloc{Off: int32(insnOff), Siz: 8, Type: objabi.R_ADDR, Add: add, Sym: target})
		case elfAARCH64ABS32:
			s.AddRel(ctxt, obj.Reloc{Off: int32(insnOff), Siz: 4, Type: objabi.R_ADDR, Add: add, Sym: target})
		case elfAARCH64ADDLO, elfAARCH64GOTLO, elfAARCH64LDST8, elfAARCH64LDST16, elfAARCH64LDST32, elfAARCH64LDST64:
			arm64Fatal("%s: LO12 reloc type %d at +0x%x has no ADRP", s.Name, r.Type, insnOff)
		default:
			arm64Fatal("%s: unsupported arm64 text reloc type %d at +0x%x", s.Name, r.Type, insnOff)
		}
	}
}

func arm64PagePair(second uint32) (objabi.RelocType, bool) {
	switch second {
	case elfAARCH64ADDLO:
		return objabi.R_ADDRARM64, true
	case elfAARCH64LDST8:
		return objabi.R_ARM64_PCREL_LDST8, true
	case elfAARCH64LDST16:
		return objabi.R_ARM64_PCREL_LDST16, true
	case elfAARCH64LDST32:
		return objabi.R_ARM64_PCREL_LDST32, true
	case elfAARCH64LDST64:
		return objabi.R_ARM64_PCREL_LDST64, true
	}
	return 0, false
}

// arm64ClearPagePair zeros the ADRP immediate (bits 30-29 and 23-5) and
// the following instruction's imm12 (bits 21-10). The linker ORs the
// page and page-offset in and does not clear leftover bits. An ADD also
// loses the LSL #12 shift bit.
func arm64ClearPagePair(code []byte, off int) {
	w0 := insnAt(code, off)
	w0 &^= 0x60FFFFE0
	putInsnAt(code, off, w0)
	w1 := insnAt(code, off+4)
	w1 &^= 0x003FFC00
	if w1>>24 == 0x91 {
		w1 &^= 1 << 22
	}
	putInsnAt(code, off+4, w1)
}

func findCallSitesARM64(code []byte, baseOff uint64, rels []rela, syms []string) []callSite {
	var out []callSite
	for _, r := range rels {
		if r.Type != elfAARCH64CALL26 {
			continue
		}
		if r.Off < baseOff || r.Off+4 > baseOff+uint64(len(code)) {
			continue
		}
		off := int(r.Off - baseOff)
		if insnAt(code, off)>>26 != 0x25 {
			continue
		}
		ename := ""
		if r.Sym > 0 && int(r.Sym-1) < len(syms) {
			ename = syms[r.Sym-1]
		}
		out = append(out, callSite{Off: off, RelOff: off, Callee: ename, Add: r.Add, Type: r.Type})
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Off < out[j].Off })
	return out
}
