package llvmmc_test

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

func findRoot(t *testing.T) string {
	t.Helper()
	wd, _ := os.Getwd()
	dir := wd
	for i := 0; i < 8; i++ {
		if _, err := os.Stat(filepath.Join(dir, "pass", "harness.mir")); err == nil {
			return dir
		}
		dir = filepath.Dir(dir)
	}
	t.Fatal("p5 root not found")
	return ""
}

func TestMirguardIdentityNoTransforms(t *testing.T) {
	root := findRoot(t)
	for _, name := range []string{"mircanon.py", "mirguard.py"} {
		src, err := os.ReadFile(filepath.Join(root, "goobj", "llvmmc", name))
		if err != nil {
			t.Fatal(err)
		}
		s := string(src)
		for _, bad := range []string{
			"\ndef split_morestack_cfg",
			"\ndef inject_frame",
			"\ndef rewrite_mem_globals",
			"\ndef normalize_opcodes",
			"\ndef rewrite_body",
			"\ndef extract_calls_and_strip",
		} {
			if strings.Contains(s, bad) {
				t.Fatalf("%s must not define %s (P14 identity)", name, strings.TrimSpace(bad))
			}
		}
	}
	g, _ := os.ReadFile(filepath.Join(root, "goobj", "llvmmc", "mirguard.py"))
	if !strings.Contains(string(g), "identity") {
		t.Fatal("mirguard missing identity marker")
	}
}

func TestHarnessDirectLlcAndIdentityCopy(t *testing.T) {
	root := findRoot(t)
	tmp := t.TempDir()
	mir := filepath.Join(tmp, "canon.mir")
	meta := filepath.Join(tmp, "meta.json")
	inMir := filepath.Join(root, "pass", "harness.mir")
	inMeta := filepath.Join(root, "pass", "harness.meta.json")
	cmd := exec.Command("python3", filepath.Join(root, "goobj", "llvmmc", "mirguard.py"),
		"-in", inMir, "-meta-in", inMeta, "-out-mir", mir, "-out-meta", meta)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("mirguard: %v\n%s", err, out)
	}
	a, _ := os.ReadFile(inMir)
	b, _ := os.ReadFile(mir)
	if string(a) != string(b) {
		t.Fatal("mirguard rewrote MIR (P14 requires identity)")
	}
	if !strings.Contains(string(b), "no_callee_saved_registers") {
		t.Fatal("harness MIR missing no_callee_saved_registers")
	}
	if strings.Contains(string(b), "goc.flags") || strings.Contains(string(b), "GOC_PCDATA1") {
		// allow only in comments — strip comments
		body := ""
		for _, ln := range strings.Split(string(b), "\n") {
			if strings.TrimSpace(ln) == "" || strings.HasPrefix(strings.TrimSpace(ln), "#") {
				continue
			}
			body += ln + "\n"
		}
		if strings.Contains(body, "goc.flags") || strings.Contains(body, "GOC_PCDATA1") {
			t.Fatal("harness MIR body still has goc dialect keys")
		}
	}
	if !strings.Contains(string(b), "PUSH64r") {
		t.Fatal("harness MIR missing explicit PUSH64r frame")
	}
	if !strings.Contains(string(b), "ADDSDrr") {
		t.Fatal("harness MIR missing goc_fadd64 ADDSDrr")
	}
	// llc DIRECT on input (not rewrite)
	obj := filepath.Join(tmp, "out.o")
	cmd = exec.Command("llc-19", "-O0", "-relocation-model=pic", "-march=x86-64", "-filetype=obj", "-o", obj, inMir)
	out, err = cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("llc-19 direct: %v\n%s", err, out)
	}
	st, err := os.Stat(obj)
	if err != nil || st.Size() < 100 {
		t.Fatalf("llc ELF missing/small: %v", err)
	}
	nm, err := exec.Command("llvm-nm-19", obj).CombinedOutput()
	if err != nil {
		nm, err = exec.Command("nm", obj).CombinedOutput()
	}
	if err != nil {
		t.Fatalf("nm: %v", err)
	}
	s := string(nm)
	for _, want := range []string{"goc_checked_add", "goc_hold_live", "goc_leaf", "goc_store_gptr", "goc_fadd64", "goc_fadd32"} {
		if !strings.Contains(s, want) {
			t.Fatalf("ELF missing %s\n%s", want, s)
		}
	}
}

func TestReadmeDocumentsLLVMAPIs(t *testing.T) {
	root := findRoot(t)
	b, err := os.ReadFile(filepath.Join(root, "goobj", "llvmmc", "README.md"))
	if err != nil {
		t.Fatal(err)
	}
	s := string(b)
	for _, want := range []string{"llc-19", "MCCodeEmitter", "AsmPrinter", "elfpack", "identity", "P14", "X0"} {
		if !strings.Contains(s, want) {
			t.Fatalf("README missing %s", want)
		}
	}
}
