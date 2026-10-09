package lower

import (
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

func TestGoABIMatchesPython(t *testing.T) {
	script := filepath.Join("..", "..", "realbody", "goc_goabi.py")
	if _, err := os.Stat(script); err != nil {
		t.Fatalf("python oracle missing: %v", err)
	}
	cases := []struct {
		name string
		arch string
		ir   string
	}{
		{"add", "amd64", `
define i32 @add(i32 %a, i32 %b) {
  %c = add i32 %a, %b
  ret i32 %c
}
`},
		{"seven", "amd64", `
declare i32 @leaf(i32, i32, i32, i32, i32, i32, i32)
define i32 @sum7(i32 %a, i32 %b, i32 %c, i32 %d, i32 %e, i32 %f, i32 %g) {
  %r = call i32 @leaf(i32 %a, i32 %b, i32 %c, i32 %d, i32 %e, i32 %f, i32 %g)
  ret i32 %r
}
`},
		{"agg", "amd64", `
define { i64, i64 } @pair(i64 %a, { i64, i64 } %s) {
  ret { i64, i64 } %s
}
`},
		{"skip", "amd64", `
define internal i32 @hid(i32 %a) {
  ret i32 %a
}
define i32 @dots(i32 %a, ...) {
  ret i32 %a
}
define float @fadd(float %a, double %b) {
  ret float %a
}
`},
		{"arm", "arm64", `
define i64 @sum9(i64, i64, i64, i64, i64, i64, i64, i64, i64) {
  ret i64 0
}
define ptr @box(ptr %p) {
  ret ptr %p
}
`},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			dir := t.TempDir()
			in := filepath.Join(dir, "in.ll")
			if err := os.WriteFile(in, []byte(tc.ir), 0o644); err != nil {
				t.Fatal(err)
			}
			pyLL := filepath.Join(dir, "py.ll")
			pyS := filepath.Join(dir, "py.s")
			pyJ := filepath.Join(dir, "py.json")
			cmd := exec.Command("python3", script, in, pyLL, pyS, pyJ)
			cmd.Env = append(os.Environ(), "GOC_ARCH="+tc.arch)
			out, err := cmd.CombinedOutput()
			if err != nil {
				t.Fatalf("python: %v\n%s", err, out)
			}
			gotLL, gotS, gotJ, gotLog, err := RunGoABI(tc.ir, tc.arch)
			if err != nil {
				t.Fatal(err)
			}
			wantLL, _ := os.ReadFile(pyLL)
			wantS, _ := os.ReadFile(pyS)
			wantJ, _ := os.ReadFile(pyJ)
			if gotLL != string(wantLL) {
				t.Errorf("IR mismatch\n--- py ---\n%s\n--- go ---\n%s", wantLL, gotLL)
			}
			if gotS != string(wantS) {
				t.Errorf("asm mismatch\n--- py ---\n%s\n--- go ---\n%s", wantS, gotS)
			}
			if !jsonEqual(t, wantJ, gotJ) {
				t.Errorf("json mismatch\n--- py ---\n%s\n--- go ---\n%s", wantJ, gotJ)
			}
			if gotLog != string(out) {
				t.Errorf("log mismatch\n--- py ---\n%s\n--- go ---\n%s", out, gotLog)
			}
		})
	}
}

func jsonEqual(t *testing.T, a, b []byte) bool {
	t.Helper()
	var ja, jb any
	if err := json.Unmarshal(a, &ja); err != nil {
		t.Fatal(err)
	}
	if err := json.Unmarshal(b, &jb); err != nil {
		t.Fatal(err)
	}
	ab, _ := json.Marshal(ja)
	bb, _ := json.Marshal(jb)
	return string(ab) == string(bb)
}

func TestFixIRMatchesShape(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "a.ll")
	src := "define i32 @add(i32 %a) {\n  ret i32 %a\n}\n"
	if err := os.WriteFile(path, []byte(src), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := FixIR(path, "amd64"); err != nil {
		t.Fatal(err)
	}
	got, _ := os.ReadFile(path)
	if !strings.Contains(string(got), `override-stack-alignment`) || !strings.Contains(string(got), `"no-realign-stack"`) {
		t.Fatalf("fix-ir:\n%s", got)
	}
}
