package main

import (
	"runtime"
	"strings"
	"testing"

	"github.com/LoyieKing/goc/gocobj"
)

func TestLookupGoVersion(t *testing.T) {
	for _, s := range []string{"go1.24", "go1.24.4", "go1.24rc2"} {
		if v, err := lookupGoVersion(s); err != nil || v.Name != "go1.24" {
			t.Errorf("%s: got %v, %v", s, v, err)
		}
	}
	for _, s := range []string{"go1.2", "go1.240", "go1.25.0", "devel"} {
		if _, err := lookupGoVersion(s); err == nil {
			t.Errorf("%s: accepted", s)
		}
	}
	_, err := lookupGoVersion("go1.99.1")
	if err == nil || !strings.Contains(err.Error(), "gover_go1_99.go") {
		t.Errorf("go1.99.1: %v", err)
	}
}

// The Go this is built with must have a mapping: it is the one the driver
// links with. A CI job on a newer Go fails here first.
func TestBuildGoHasMapping(t *testing.T) {
	if _, err := lookupGoVersion(runtime.Version()); err != nil {
		t.Fatal(err)
	}
}

func TestGoVersionTables(t *testing.T) {
	for _, v := range goVersions {
		for arch, name := range v.Profiles {
			p, ok := gocobj.LookupProfile(name)
			if !ok {
				t.Errorf("%s: %s profile %q is not in gocobj/abi.go", v.Name, arch, name)
				continue
			}
			if !strings.HasPrefix(name, v.Name+"-") {
				t.Errorf("%s: %s profile %q belongs to another Go", v.Name, arch, p.Name)
			}
		}
		if v.X86Text == nil || v.X86Data == nil || v.A64PagePair == nil || v.A64Lo12 == nil || v.A64Data == nil {
			t.Errorf("%s: missing relocation table", v.Name)
		}
		if v.X86Call == 0 || v.A64Call == 0 || v.A64Page == 0 || v.A64GOTPair == 0 {
			t.Errorf("%s: missing relocation type", v.Name)
		}
	}
}
