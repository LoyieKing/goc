package lower

import (
	"strings"
	"testing"
)

func TestParseGoExterns(t *testing.T) {
	ir := `
@.str = private unnamed_addr constant [28 x i8] c"goc.attr.go_extern:main.Add\00", section "llvm.metadata"
@.str.1 = private unnamed_addr constant [9 x i8] c"ann_test\00", section "llvm.metadata"
@llvm.global.annotations = appending global [1 x { ptr, ptr, ptr, i32, ptr }] [{ ptr, ptr, ptr, i32, ptr } { ptr @Add, ptr @.str, ptr @.str.1, i32 1, ptr null }], section "llvm.metadata"
declare i64 @Add(i64 noundef, i64 noundef) #0
`
	m := parseGoExterns(ir)
	if m["Add"] != "main.Add" {
		t.Fatalf("got %#v", m)
	}
}

func TestGoExternStubEmission(t *testing.T) {
	ir := `
@.str = private unnamed_addr constant [28 x i8] c"goc.attr.go_extern:main.Add\00", section "llvm.metadata"
@.str.1 = private unnamed_addr constant [5 x i8] c"test\00", section "llvm.metadata"
@llvm.global.annotations = appending global [1 x { ptr, ptr, ptr, i32, ptr }] [{ ptr, ptr, ptr, i32, ptr } { ptr @Add, ptr @.str, ptr @.str.1, i32 1, ptr null }], section "llvm.metadata"

define i32 @c_only(i32 %a) {
  ret i32 %a
}

declare i64 @Add(i64 noundef, i64 noundef)
`
	ll, asm, _, logText, err := RunGoABI(ir, "amd64")
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(logText, "go_extern: 1 stubs") {
		t.Fatalf("log: %s", logText)
	}
	if !strings.Contains(asm, "call\tmain.Add.goabi") {
		t.Fatalf("asm missing call:\n%s", asm)
	}
	if !strings.Contains(asm, "movq\t%fs:-8, %r14") {
		t.Fatalf("asm missing g load:\n%s", asm)
	}
	if !strings.Contains(asm, "pxor\t%xmm15, %xmm15") {
		t.Fatalf("asm missing xmm15 clear:\n%s", asm)
	}
	// Add must not be renamed to .impl (go_extern declare)
	if strings.Contains(ll, "@Add.impl") {
		t.Fatalf("Add should not be renamed to .impl:\n%s", ll)
	}
	// c_only should still get a Go→C thunk
	if !strings.Contains(asm, "call\tc_only.impl") {
		t.Fatalf("c_only thunk missing:\n%s", asm)
	}
}

func TestGoExternPairResultMove(t *testing.T) {
	ir := `
@.str = private unnamed_addr constant [30 x i8] c"goc.attr.go_extern:main.Clock\00", section "llvm.metadata"
@.str.1 = private unnamed_addr constant [5 x i8] c"test\00", section "llvm.metadata"
@llvm.global.annotations = appending global [1 x { ptr, ptr, ptr, i32, ptr }] [{ ptr, ptr, ptr, i32, ptr } { ptr @Clock, ptr @.str, ptr @.str.1, i32 1, ptr null }], section "llvm.metadata"
declare { i64, i64 } @Clock(i64 noundef)
`
	_, asm, _, _, err := RunGoABI(ir, "amd64")
	if err != nil {
		t.Fatal(err)
	}
	// Go returns (AX, BX); SysV struct uses (AX, DX) → move BX→DX
	if !strings.Contains(asm, "movq\t%rbx, %rdx") {
		t.Fatalf("expected rbx→rdx after Go call:\n%s", asm)
	}
}
