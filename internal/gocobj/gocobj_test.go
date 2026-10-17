package gocobj

import (
	"bytes"
	"debug/elf"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"testing"
)

const golden = "../../test/gocobj/v1.0"

var wantErr = map[string]string{
	"bad-profile":       `unknown ABI profile "go1.99-amd64"`,
	"bad-text-size":     "text_size 24, ELF symbol size 23",
	"bad-reloc-range":   "runs past function end",
	"bad-reloc-target":  `no ELF symbol "runtime.nosuch"`,
	"bad-no-gc-record":  "safepoint call without gc record at pc 17",
	"bad-pc-order":      "SP_DELTA table not increasing",
	"bad-locals-bits":   "locals bits do not fit frame",
	"bad-no-stackcheck": "tables show no go1.24-amd64 entry check",
}

func TestGolden(t *testing.T) {
	files, _ := filepath.Glob(filepath.Join(golden, "*.gocobj"))
	if len(files) != len(wantErr)+1 {
		t.Fatalf("found %d golden files, want %d", len(files), len(wantErr)+1)
	}
	for _, path := range files {
		name := strings.TrimSuffix(filepath.Base(path), ".gocobj")
		f, err := Open(path)
		if err != nil {
			t.Errorf("%s: %v", name, err)
			continue
		}
		errs := Verify(f)
		if name == "valid" {
			for _, e := range errs {
				t.Errorf("valid: %v", e)
			}
			continue
		}
		want, ok := wantErr[name]
		if !ok {
			t.Errorf("%s: no expected error listed", name)
			continue
		}
		var got []string
		for _, e := range errs {
			got = append(got, e.Error())
		}
		if !strings.Contains(strings.Join(got, "\n"), want) {
			t.Errorf("%s: want error %q, got %q", name, want, got)
		}
	}
}

// Re-encoding what Parse read must give the bytes that are in the file.
func TestRoundTrip(t *testing.T) {
	path := filepath.Join(golden, "valid.gocobj")
	f, err := Open(path)
	if err != nil {
		t.Fatal(err)
	}
	b := &Builder{Header: f.Header, Files: f.Files, Funcs: f.Funcs, Relocs: f.Relocs, Data: f.Data}
	ef, err := elf.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer ef.Close()
	for name, want := range b.Sections() {
		s := ef.Section(name)
		if s == nil {
			t.Errorf("%s missing from file", name)
			continue
		}
		got, _ := s.Data()
		if !bytes.Equal(got, want) {
			t.Errorf("%s: re-encoded bytes differ", name)
		}
	}
}

func patchHeader(t *testing.T, off int, v byte) string {
	t.Helper()
	path := filepath.Join(golden, "valid.gocobj")
	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	ef, err := elf.NewFile(bytes.NewReader(raw))
	if err != nil {
		t.Fatal(err)
	}
	raw[int(ef.Section(SecHeader).Offset)+off] = v
	out := filepath.Join(t.TempDir(), "p.gocobj")
	if err := os.WriteFile(out, raw, 0o644); err != nil {
		t.Fatal(err)
	}
	return out
}

func TestVersions(t *testing.T) {
	if _, err := Open(patchHeader(t, 8, 2)); err == nil || !strings.Contains(err.Error(), "major 2") {
		t.Errorf("major 2: got %v, want rejection", err)
	}
	f, err := Open(patchHeader(t, 10, 7))
	if err != nil || f.Header.Minor != 7 {
		t.Errorf("minor 7: got %v, %v; want accepted", f, err)
	}
	if _, err := Open(patchHeader(t, 0, 'X')); err == nil {
		t.Error("bad magic accepted")
	}
}

// A newer minor may append fields to a record; the reader must skip them.
func TestRecordSkipsUnknownTail(t *testing.T) {
	var sec enc
	var r enc
	r.uleb(5)
	r.raw([]byte{0xaa, 0xbb, 0xcc}) // fields from a later minor
	sec.record(r.b)
	sec.record([]byte{6})
	var got []uint64
	err := records("t", sec.b, func(_ uint64, b *buf) error {
		got = append(got, b.uleb())
		return nil
	})
	if err != nil || len(got) != 2 || got[0] != 5 || got[1] != 6 {
		t.Fatalf("got %v %v", got, err)
	}
}

func TestLEB(t *testing.T) {
	for _, v := range []int64{0, 1, -1, 63, 64, -64, -65, 1 << 40, -(1 << 40), 1<<63 - 1, -1 << 63} {
		var e enc
		e.sleb(v)
		r := &buf{b: e.b}
		if got := r.sleb(); got != v || r.err != nil || len(r.b) != 0 {
			t.Errorf("sleb %d: got %d %v", v, got, r.err)
		}
	}
}

// gocobj/abi.go must match llvm/include/Goc/AbiProfiles.def.
func TestProfilesMatchDef(t *testing.T) {
	src, err := os.ReadFile("../../llvm/include/Goc/AbiProfiles.def")
	if err != nil {
		t.Fatal(err)
	}
	re := regexp.MustCompile(`(?m)^GOC_ABI_PROFILE\(([^)]*)\)`)
	tok := regexp.MustCompile(`"[^"]*"|-?\d+`)
	ms := re.FindAllSubmatch(src, -1)
	if len(ms) != len(profiles) {
		t.Fatalf(".def has %d profiles, abi.go %d", len(ms), len(profiles))
	}
	for i, m := range ms {
		a := tok.FindAllString(string(m[1]), -1)
		if len(a) != 10 {
			t.Fatalf("profile %d: %d fields", i, len(a))
		}
		s := func(j int) string { return strings.Trim(a[j], `"`) }
		n := func(j int) int { v, _ := strconv.Atoi(a[j]); return v }
		p := profiles[i]
		if p.Name != s(0) || p.Arch != s(1) || p.PtrSize != n(2) || p.GReg != s(3) ||
			p.StackGuard0Off != n(4) || p.StackSmall != n(5) || p.StackBig != n(6) ||
			p.Morestack != s(7) || strings.Join(p.IntRegs, ",") != s(8) || strings.Join(p.FPRegs, ",") != s(9) {
			t.Errorf("profile %s differs from AbiProfiles.def", p.Name)
		}
	}
}
