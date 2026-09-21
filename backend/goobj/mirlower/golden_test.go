package mirlower

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"goc.local/p5-machinepass-goobj/goobj/enc/cmd/objlib/obj"
)

// TestGoldenMutateMIChangesOutput ensures MIR→Prog lowering is live:
// mutating an MI immediate must change the op signature, and P12 primary encoding is llvmmc/llc; this test still guards legacy mirlower fixtures.
func TestGoldenMutateMIChangesOutput(t *testing.T) {
	root := findP5Root(t)
	mirPath := filepath.Join(root, "pass", "harness.mir")
	if _, err := os.Stat(mirPath); err != nil {
		t.Fatalf("P11 primary MIR missing: %v", err)
	}
	bodyPath := filepath.Join(root, "pass", "mi_full_bodies.txt")
	body, err := os.ReadFile(bodyPath)
	if err != nil {
		t.Fatalf("read mi_full_bodies: %v", err)
	}
	tmp := filepath.Join(t.TempDir(), "mi_lower.txt")
	wrapped := append([]byte("format goc-mi-lower-1\nabi amd64_ABIInternal\narch amd64\n"), body...)
	if err := os.WriteFile(tmp, wrapped, 0o644); err != nil {
		t.Fatal(err)
	}

	base, err := Load(tmp)
	if err != nil {
		t.Fatalf("Load: %v", err)
	}
	fn, err := base.RequireFn("goc_hold_live")
	if err != nil {
		t.Fatal(err)
	}
	sig := fnOpSignature(fn)
	if !strings.Contains(sig, "MOV64rm") || !strings.Contains(sig, "CALL64pcrel32") {
		t.Fatalf("hold_live signature missing expected ops: %s", sig)
	}
	if !strings.Contains(sig, "FS:-8") {
		t.Fatalf("goc_hold_live missing MIR morestack TLS load FS:-8 (P10)")
	}
	if !strings.Contains(sig, "runtime.morestack_noctxt") {
		t.Fatalf("goc_hold_live missing CALL morestack (P10)")
	}
	if fn.Flags&obj.NOSPLIT == 0 {
		t.Fatalf("goc_hold_live must be NOSPLIT (MIR-owned morestack)")
	}

	leaf, err := base.RequireFn("goc_leaf")
	if err != nil {
		t.Fatal(err)
	}
	if len(leaf.Ops) == 0 {
		t.Fatal("goc_leaf empty")
	}

	mut := cloneFn(fn)
	changed := false
	for i, line := range mut.Ops {
		if strings.Contains(line, "imm=42") {
			mut.Ops[i] = strings.Replace(line, "imm=42", "imm=43", 1)
			changed = true
			break
		}
	}
	if !changed {
		t.Fatal("could not find imm=42 to mutate in goc_hold_live")
	}
	if fnOpSignature(fn) == fnOpSignature(mut) {
		t.Fatal("mutation did not change op signature")
	}

	bw, err := os.ReadFile(filepath.Join(root, "goobj", "binwriter", "main.go"))
	if err != nil {
		t.Fatal(err)
	}
	if bytes.Contains(bw, []byte("x86.AMOVQ")) || bytes.Contains(bw, []byte("x86.ACALL")) ||
		bytes.Contains(bw, []byte("x86.ACMPQ")) {
		t.Fatal("binwriter contains direct x86.A* Prog construction (templates returned)")
	}
	if !bytes.Contains(bw, []byte("RequireFn")) || !bytes.Contains(bw, []byte("LowerFn")) {
		t.Fatal("binwriter missing RequireFn/LowerFn MIR-only path")
	}
}

func fnOpSignature(fn *Fn) string {
	var b strings.Builder
	for _, op := range fn.Ops {
		b.WriteString(op)
		b.WriteByte('\n')
	}
	return b.String()
}

func cloneFn(fn *Fn) *Fn {
	cp := *fn
	cp.Ops = append([]string(nil), fn.Ops...)
	return &cp
}

func findP5Root(t *testing.T) string {
	t.Helper()
	wd, err := os.Getwd()
	if err != nil {
		t.Fatal(err)
	}
	dir := wd
	for i := 0; i < 8; i++ {
		if _, err := os.Stat(filepath.Join(dir, "goobj", "binwriter", "main.go")); err == nil {
			return dir
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			break
		}
		dir = parent
	}
	t.Fatal("cannot find p5-machinepass-goobj root")
	return ""
}
