package lower

import (
	"encoding/json"
	"flag"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

var update = flag.Bool("update", false, "rewrite testdata/goabi/* from the current output")

// TestGoABIGolden compares the Go ABI thunk generator with the outputs
// recorded in testdata/goabi (the IR, the thunk assembly, the goabi JSON and
// the log). They were first taken from the Python generator this package
// replaced; go test -run TestGoABIGolden -update rewrites them.
func TestGoABIGolden(t *testing.T) {
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
			gotLL, gotS, gotJ, gotLog, err := RunGoABI(tc.ir, tc.arch)
			if err != nil {
				t.Fatal(err)
			}
			for _, f := range []struct{ ext, got string }{
				{"ll", gotLL}, {"s", gotS}, {"json", string(gotJ)}, {"log", gotLog},
			} {
				path := filepath.Join("testdata", "goabi", tc.name+"."+f.ext)
				if *update {
					if err := os.WriteFile(path, []byte(f.got), 0o644); err != nil {
						t.Fatal(err)
					}
					continue
				}
				want, err := os.ReadFile(path)
				if err != nil {
					t.Fatal(err)
				}
				if f.ext == "json" {
					if !jsonEqual(t, want, gotJ) {
						t.Errorf("%s mismatch\n--- want ---\n%s\n--- got ---\n%s", path, want, gotJ)
					}
					continue
				}
				if f.got != string(want) {
					t.Errorf("%s mismatch\n--- want ---\n%s\n--- got ---\n%s", path, want, f.got)
				}
			}
		})
	}
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

// jsonEqual compares two JSON documents by value: the golden files keep the
// Python generator's formatting.
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
