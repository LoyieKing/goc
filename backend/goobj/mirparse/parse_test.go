package mirparse

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestParseHarnessMIR(t *testing.T) {
	root := findP5(t)
	mod, err := ParseFile(filepath.Join(root, "pass", "harness.mir"))
	if err != nil {
		t.Fatal(err)
	}
	if len(mod.Functions) < 9 {
		t.Fatalf("want >=9 fns (incl float), got %d", len(mod.Functions))
	}
	names := map[string]bool{}
	for _, f := range mod.Functions {
		names[f.Name] = true
		if len(f.Blocks) == 0 {
			t.Fatalf("%s: no blocks", f.Name)
		}
	}
	for _, n := range []string{"goc_checked_add", "goc_hold_live", "goc_leaf", "goc_store_gptr"} {
		if !names[n] {
			t.Fatalf("missing %s", n)
		}
	}
	// Hold live should have FS load + CALL morestack
	var hold *Function
	for _, f := range mod.Functions {
		if f.Name == "goc_hold_live" {
			hold = f
			break
		}
	}
	raw := ""
	for _, b := range hold.Blocks {
		for _, in := range b.Instrs {
			raw += in.Raw + "\n"
		}
	}
	if !strings.Contains(raw, "MOV64rm") || !strings.Contains(raw, "morestack_noctxt") {
		t.Fatalf("hold_live body incomplete:\n%s", raw)
	}
	// P14: goc.* keys removed from MIR (sidecar meta); fallbacks supply go_sym/frame.
	opt := DefaultHarnessOptions()
	if opt.GoSymFallback["goc_hold_live"] != "main.GocHoldLive" || opt.FrameFallback["goc_hold_live"] != 24 {
		t.Fatalf("fallback metadata missing")
	}
	if hold.GoSym != "" || hold.Frame != 0 {
		t.Fatalf("P14: harness.mir must not embed goc.* (got go_sym=%q frame=%d)", hold.GoSym, hold.Frame)
	}
	for _, n := range []string{"goc_fadd64", "goc_fadd32"} {
		if !names[n] {
			t.Fatalf("missing %s", n)
		}
	}
}

func TestConvertHarnessToMiLower(t *testing.T) {
	root := findP5(t)
	mod, err := ParseFile(filepath.Join(root, "pass", "harness.mir"))
	if err != nil {
		t.Fatal(err)
	}
	ml, err := ToMiLower(mod, DefaultHarnessOptions())
	if err != nil {
		t.Fatal(err)
	}
	fn := ml.Fns["goc_hold_live"]
	if fn == nil {
		t.Fatal("missing hold_live")
	}
	sig := strings.Join(fn.Ops, "\n")
	for _, need := range []string{"FS:-8", "runtime.morestack_noctxt", "imm=42", "goc_leaf", "SP:16"} {
		if !strings.Contains(sig, need) {
			t.Fatalf("hold_live missing %q in:\n%s", need, sig)
		}
	}
	leaf := ml.Fns["goc_leaf"]
	if leaf == nil || !strings.Contains(strings.Join(leaf.Ops, "\n"), "ADD64rr") {
		t.Fatal("goc_leaf bad")
	}
	wb := ml.Fns["goc_store_gptr"]
	if wb == nil || !strings.Contains(strings.Join(wb.Ops, "\n"), "gcWriteBarrier2") {
		t.Fatal("store_gptr bad")
	}
	f64 := ml.Fns["goc_fadd64"]
	if f64 == nil || f64.GoSym != "main.GocFadd64" || !strings.Contains(strings.Join(f64.Ops, "\n"), "ADDSDrr") {
		t.Fatalf("goc_fadd64 bad: %+v", f64)
	}
}

func TestParseDumpFormat(t *testing.T) {
	dump := `
# Machine code for function goc_leaf: IsSSA, TracksLiveness

bb.0:
  successors: %bb.1(0x80000000); %bb.1(100.00%)

  %0:gr64 = COPY $rdi
  %1:gr64 = COPY $rsi
  %0:gr64 = ADD64rr %0:gr64, %1:gr64, implicit-def $eflags
  $rax = COPY %0:gr64
  RET64

# End machine code for function goc_leaf.
`
	mod, err := Parse(dump)
	if err != nil {
		t.Fatal(err)
	}
	if len(mod.Functions) != 1 || mod.Functions[0].Name != "goc_leaf" {
		t.Fatalf("dump parse: %+v", mod.Functions)
	}
	ml, err := ToMiLower(mod, DefaultHarnessOptions())
	if err != nil {
		t.Fatal(err)
	}
	sig := strings.Join(ml.Fns["goc_leaf"].Ops, "\n")
	// COPY elided; ADD should use DI/SI→ eventually AX
	if !strings.Contains(sig, "ADD64rr") {
		t.Fatalf("expected ADD64rr after vreg COPY resolve:\n%s", sig)
	}
}

func TestParseMemoryAndSuccessors(t *testing.T) {
	src := `
---
name: demo
goc.frame: 16
goc.flags: nosplit
body: |
  bb.0.entry:
    successors: %bb.1(0x40000000), %bb.2(0x40000000)
    ; predecessors: 
    $r11 = MOV64rm $noreg, 1, $noreg, -8, $fs
    CMP64rm $rsp, $r11, 1, $noreg, 16, $noreg, implicit-def $eflags
    JCC_1 %bb.2, 6, implicit $eflags
  bb.1.ok:
    RET64
  bb.2.morestack:
    CALL64pcrel32 @runtime.morestack_noctxt, implicit $rsp
    JMP_1 %bb.0
...
`
	mod, err := Parse(src)
	if err != nil {
		t.Fatal(err)
	}
	fn := mod.Functions[0]
	if len(fn.Blocks) != 3 {
		t.Fatalf("blocks=%d", len(fn.Blocks))
	}
	if len(fn.Blocks[0].Successors) < 2 {
		t.Fatalf("successors not parsed: %v", fn.Blocks[0].Successors)
	}
	ml, err := ToMiLower(mod, DefaultHarnessOptions())
	if err != nil {
		t.Fatal(err)
	}
	sig := strings.Join(ml.Fns["demo"].Ops, "\n")
	if !strings.Contains(sig, "mem=FS:-8") || !strings.Contains(sig, "cond=BE") {
		t.Fatalf("bad lower:\n%s", sig)
	}
}

func findP5(t *testing.T) string {
	t.Helper()
	wd, _ := os.Getwd()
	dir := wd
	for i := 0; i < 8; i++ {
		if _, err := os.Stat(filepath.Join(dir, "pass", "harness.mir")); err == nil {
			return dir
		}
		p := filepath.Dir(dir)
		if p == dir {
			break
		}
		dir = p
	}
	t.Fatal("p5 root not found")
	return ""
}
