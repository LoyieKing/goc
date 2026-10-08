package main

import (
	"encoding/binary"
	"testing"

	"goc.local/p5-machinepass-goobj/goobj/enc/cmd/objlib/obj"
	"goc.local/p5-machinepass-goobj/goobj/enc/cmd/objlib/sys"
	"goc.local/p5-machinepass-goobj/goobj/enc/stdinternal/abi"
)

func u32(b []byte) uint32 { return binary.LittleEndian.Uint32(b) }

func TestArm64KnownWords(t *testing.T) {
	// llvm-mc -triple=aarch64-unknown-linux-gnu words.
	if arm64StpPre(0, 1) != 0xA9BF07E0 {
		t.Fatalf("stp x0, x1: %#x", arm64StpPre(0, 1))
	}
	if arm64StpPre(29, 30) != 0xA9BF7BFD {
		t.Fatalf("stp x29, x30: %#x", arm64StpPre(29, 30))
	}
	if arm64StpPre(8, 31) != 0xA9BF7FE8 {
		t.Fatalf("stp x8, xzr: %#x", arm64StpPre(8, 31))
	}
	if arm64LdpPost(0, 1) != 0xA8C107E0 {
		t.Fatalf("ldp x0, x1: %#x", arm64LdpPost(0, 1))
	}
	if arm64LdpPost(8, 31) != 0xA8C17FE8 {
		t.Fatalf("ldp x8, xzr: %#x", arm64LdpPost(8, 31))
	}
	if arm64StpQ(0, 1, 0) != 0xAD0007E0 || arm64StpQ(2, 3, 32) != 0xAD010FE2 ||
		arm64StpQ(4, 5, 64) != 0xAD0217E4 || arm64StpQ(6, 7, 96) != 0xAD031FE6 {
		t.Fatal("stp q")
	}
	if arm64LdpQ(6, 7, 96) != 0xAD431FE6 {
		t.Fatalf("ldp q6, q7: %#x", arm64LdpQ(6, 7, 96))
	}
	if arm64SubImm(31, 31, 128) != 0xD10203FF {
		t.Fatalf("sub sp, sp, #128: %#x", arm64SubImm(31, 31, 128))
	}
	if arm64SubImm(31, 31, 32) != 0xD10083FF {
		t.Fatalf("sub sp, sp, #32: %#x", arm64SubImm(31, 31, 32))
	}
	if arm64AddImm(31, 31, 128) != 0x910203FF {
		t.Fatalf("add sp, sp, #128: %#x", arm64AddImm(31, 31, 128))
	}
	if arm64AddImm(29, 31, 0) != 0x910003FD {
		t.Fatalf("mov x29, sp: %#x", arm64AddImm(29, 31, 0))
	}
	if arm64AddImm(29, 31, 16) != 0x910043FD {
		t.Fatalf("add x29, sp, #16: %#x", arm64AddImm(29, 31, 16))
	}
	spill := arm64ArgSpill(metaArgSpill{Reg: "x8", Off: 8, Size: 8}, false)
	if u32(spill) != 0xF90007E8 {
		t.Fatalf("str x8, [sp, #8]: %#x", u32(spill))
	}
	load := arm64ArgSpill(metaArgSpill{Reg: "x8", Off: 8, Size: 8}, true)
	if u32(load) != 0xF94007E8 {
		t.Fatalf("ldr x8, [sp, #8]: %#x", u32(load))
	}
	if u32(arm64MovWide(17, 1)) != 0xD2800031 {
		t.Fatalf("movz x17, #1: %#x", u32(arm64MovWide(17, 1)))
	}
	mk := arm64MovWide(17, 0x20001)
	if len(mk) != 8 || u32(mk) != 0xD2800031 || u32(mk[4:]) != 0xF2A00051 {
		t.Fatalf("movz/movk x17: %x", mk)
	}
}

func TestArm64SplitCheck(t *testing.T) {
	small, br := arm64SplitCheck(16)
	if u32(small) != arm64LdrX16X28 || u32(small[4:]) != arm64CmpSpX16 || u32(small[8:]) != arm64BLS {
		t.Fatalf("small check: %x", small)
	}
	if len(br) != 1 || br[0] != 8 {
		t.Fatalf("small branches: %v", br)
	}
	arm64PatchCond(small, br, 16)
	// b.ls from +8 to +16 is #8, imm19=2, cond LS → 0x54000049.
	if u32(small[8:]) != 0x54000049 {
		t.Fatalf("patched b.ls: %#x", u32(small[8:]))
	}

	mid, _ := arm64SplitCheck(abi.StackBig)
	// SUB X17, SP, #(4096-128) then CMP X17, X16 then B.LS.
	if u32(mid[4:]) != arm64SubImm(17, 31, uint32(abi.StackBig-abi.StackSmall)) {
		t.Fatalf("mid sub: %#x", u32(mid[4:]))
	}
	if u32(mid[8:]) != arm64CmpReg(17, 16) {
		t.Fatalf("mid cmp: %#x", u32(mid[8:]))
	}

	big, bigBr := arm64SplitCheck(abi.StackBig + 1)
	if !containsWord(big, arm64SubsX17) || !containsWord(big, arm64BLO) || !containsWord(big, arm64BLS) {
		t.Fatalf("big check missing subs/b.lo/b.ls: %x", big)
	}
	if len(bigBr) != 2 {
		t.Fatalf("big branches: %v", bigBr)
	}
}

func TestArm64PrologueDelta(t *testing.T) {
	// stp x29, x30, [sp, #-16]!; mov x29, sp; ret
	leaf := concat(arm64StpPre(29, 30), 0x910003FD, arm64Ret)
	if d := arm64PrologueDelta(leaf); d != 16 {
		t.Fatalf("leaf delta %d", d)
	}
	// sub sp, sp, #32; stp x29, x30, [sp, #16]; add x29, sp, #16
	// Signed-offset STP is 0xA9017BFD (llvm-mc).
	body := concat(0xD10083FF, 0xA9017BFD, 0x910043FD, 0xD65F03C0)
	if d := arm64PrologueDelta(body); d != 32 {
		t.Fatalf("sub+stp delta %d", d)
	}
	if d := arm64PrologueDelta(concat(arm64Ret)); d != 0 {
		t.Fatalf("ret-only delta %d", d)
	}
}

func TestArm64SysvStubBalanced(t *testing.T) {
	stub, callRel, jmpRel, sp := arm64SysvSplitStub()
	if len(stub)%4 != 0 {
		t.Fatalf("stub length %d", len(stub))
	}
	if u32(stub[callRel:]) != arm64BL || u32(stub[jmpRel:]) != arm64B {
		t.Fatal("stub call/jmp are not bl/b with imm26 0")
	}
	if sp[0].Value != 0 || sp[len(sp)-1].Value != 0 {
		t.Fatalf("pcsp endpoints: %+v", sp)
	}
	// Five 16-byte integer saves plus the 128-byte SIMD area.
	saw208 := false
	for _, st := range sp {
		if st.Value == 208 {
			saw208 = true
		}
		if st.PC%4 != 0 {
			t.Fatalf("pcsp pc %d", st.PC)
		}
	}
	if !saw208 {
		t.Fatalf("stub never reaches SP-208: %+v", sp)
	}
}

func TestPcSpanUnits(t *testing.T) {
	amd := &obj.Link{Arch: &obj.LinkArch{Arch: sys.ArchAMD64}}
	if pcSpanUnits(amd, 5) != 5 {
		t.Fatal("amd64 MinLC")
	}
	arm := &obj.Link{Arch: &obj.LinkArch{Arch: sys.ArchARM64}}
	if pcSpanUnits(arm, 8) != 2 {
		t.Fatal("arm64 MinLC")
	}
}

func TestArm64BakedBL(t *testing.T) {
	// bl #+8 encodes imm26=2. PC-relative from the instruction.
	code := concat(0x94000002, arm64Ret, arm64Ret)
	ref, ok := decodeBakedRefARM64(code, 0)
	if !ok || !ref.isCall || !ref.pcAtInsn || !ref.imm26 {
		t.Fatalf("ref %+v ok %v", ref, ok)
	}
	if arm64IsIndirectCall(concat(0xD63F0000), 0) != true {
		t.Fatal("blr")
	}
	if arm64IsIndirectCall(concat(arm64BL), 0) {
		t.Fatal("bl is not indirect")
	}
	if !arm64BodyTerminates(concat(arm64Ret)) || !arm64BodyTerminates(concat(arm64B)) || !arm64BodyTerminates(arm64Trap()) {
		t.Fatal("terminators")
	}
}

func containsWord(b []byte, w uint32) bool {
	for off := 0; off+4 <= len(b); off += 4 {
		if u32(b[off:]) == w {
			return true
		}
	}
	return false
}

func concat(words ...uint32) []byte {
	var b []byte
	for _, w := range words {
		b = putInsn(b, w)
	}
	return b
}
