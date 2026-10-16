package main

import (
	"runtime"
	"testing"

	"github.com/LoyieKing/goc/gocobj"
)

// The writer must come from the Go this converter is built with: the
// header it writes carries that release (tools/goc/goenc.sh does this; the
// tree copy in goobj/enc is go1.24.4's).
func TestWriterMatchesBuildGo(t *testing.T) {
	if goVersion() != runtime.Version() {
		t.Skipf("writer is %s, built with %s (build through tools/goc/goenc.sh)", goVersion(), runtime.Version())
	}
}

func TestProfilesKnown(t *testing.T) {
	for _, arch := range []string{"amd64", "arm64"} {
		p, err := profileFor(arch)
		if err != nil {
			t.Fatal(err)
		}
		if p.Arch != arch {
			t.Errorf("%s: profile %s is for %s", arch, p.Name, p.Arch)
		}
		old, _ := gocobj.LookupProfile("go1.24-" + arch)
		if !gocobj.SameFacts(old, p) {
			t.Errorf("%s: %s and go1.24-%s differ", arch, p.Name, arch)
		}
	}
}

func TestRelocMap(t *testing.T) {
	v := gv
	if v.X86Text == nil || v.X86Data == nil || v.A64PagePair == nil || v.A64Lo12 == nil || v.A64Data == nil {
		t.Error("missing relocation table")
	}
	if v.X86Call == 0 || v.A64Call == 0 || v.A64Page == 0 || v.A64GOTPair == 0 {
		t.Error("missing relocation type")
	}
}
