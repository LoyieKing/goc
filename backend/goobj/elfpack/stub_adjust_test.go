package main

import (
	"bytes"
	"os/exec"
	"testing"
)

func TestSysvSplitStubAdjustEncoding(t *testing.T) {
	stub, callRel, jmpRel, sp := gocSysvSplitStubAdjust()
	if sp[0].Value != 0 || sp[len(sp)-1].Value != 0 {
		t.Fatalf("pcsp does not return to 0: %+v", sp)
	}
	if callRel < 1 || stub[callRel-1] != 0xe8 || !bytes.Equal(stub[callRel:callRel+4], []byte{0, 0, 0, 0}) {
		t.Fatalf("call reloc slot not a zero rel32 after e8 at %d", callRel)
	}
	if jmpRel < 1 || stub[jmpRel-1] != 0xe9 || !bytes.Equal(stub[jmpRel:jmpRel+4], []byte{0, 0, 0, 0}) {
		t.Fatalf("jmp reloc slot not a zero rel32 after e9 at %d", jmpRel)
	}
	// The post-call rewrite must land on ADDQ $144, not inside a slot.
	je := bytes.Index(stub, []byte{0x0f, 0x84, 0xb2, 0, 0, 0})
	if je < 0 {
		t.Fatal("missing JE over the CSR rewrite")
	}
	target := je + 6 + 0xb2
	want := []byte{0x48, 0x81, 0xc4, 0x90, 0, 0, 0}
	if target+len(want) > len(stub) || !bytes.Equal(stub[target:target+len(want)], want) {
		t.Fatalf("JE target %d is not ADDQ $144", target)
	}
	cmd := exec.Command("llvm-objdump-19", "-d", "-")
	cmd.Stdin = bytes.NewReader(elfText(t, stub))
	out, err := cmd.Output()
	if err != nil {
		t.Fatalf("objdump: %v", err)
	}
	for _, insn := range []string{
		"subq\t$0x90, %rsp",
		"addq\t$0x90, %rsp",
		"cmpq\t0x80(%rsp), %rax",
		"addq\t%r11, %rax",
	} {
		if !bytes.Contains(out, []byte(insn)) {
			t.Errorf("objdump missing %q\n%s", insn, out)
		}
	}
}

func TestSysvSplitStubDefaultUnchanged(t *testing.T) {
	stub, _, _, _ := gocSysvSplitStub()
	if bytes.Contains(stub, []byte{0x48, 0x81, 0xec, 0x90, 0, 0, 0}) {
		t.Fatal("default stub reserved the CSR-adjust frame")
	}
	if !bytes.Contains(stub, []byte{0x48, 0x81, 0xec, 0x80, 0, 0, 0}) {
		t.Fatal("default stub lost SUBQ $128")
	}
}

func elfText(t *testing.T, text []byte) []byte {
	t.Helper()
	cmd := exec.Command("llvm-mc-19", "-triple", "x86_64", "-filetype=obj", "-o", "-", "-")
	cmd.Stdin = bytes.NewReader([]byte(".text\n.byte " + byteList(text) + "\n"))
	out, err := cmd.Output()
	if err != nil {
		t.Fatalf("llvm-mc: %v", err)
	}
	return out
}

func byteList(b []byte) string {
	var buf bytes.Buffer
	for i, x := range b {
		if i > 0 {
			buf.WriteByte(',')
		}
		buf.WriteString("0x")
		const hex = "0123456789abcdef"
		buf.WriteByte(hex[x>>4])
		buf.WriteByte(hex[x&0xf])
	}
	return buf.String()
}
