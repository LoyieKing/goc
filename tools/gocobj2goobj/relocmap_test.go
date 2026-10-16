package main

import (
	"testing"

	"github.com/LoyieKing/goc/gocobj"
	"github.com/LoyieKing/goc/gometa"
)

// Every supported release resolves to a goc ABI profile for each arch, and
// every relocation name the converter emits is known to that release.
func TestProfilesAndRelocsKnown(t *testing.T) {
	for _, v := range gometa.Releases() {
		m, err := gometa.ForVersion(v)
		if err != nil {
			t.Fatal(err)
		}
		for _, arch := range []string{"amd64", "arm64"} {
			p, err := profileFor(m, arch)
			if err != nil {
				t.Fatal(err)
			}
			if p.Arch != arch {
				t.Errorf("%s %s: profile %s is for %s", v, arch, p.Name, p.Arch)
			}
			old, _ := gocobj.LookupProfile("go1.24-" + arch)
			if !gocobj.SameFacts(old, p) {
				t.Errorf("%s %s: %s and go1.24-%s differ", v, arch, p.Name, arch)
			}
		}
		names := []string{gv.X86Call, gv.A64Call, gv.A64Page, gv.A64GOTPair}
		for _, n := range gv.X86Text {
			names = append(names, n)
		}
		for _, n := range gv.A64PagePair {
			// The ADRP+load pairs are go1.20+; before, the halves are
			// written on their own (arm64PagePair).
			if _, ok := m.Relocs[n]; ok || n == "R_ADDRARM64" {
				names = append(names, n)
			} else if g, _ := gometa.Minor(m.Go); g >= "go1.20" {
				t.Errorf("%s: relocation %s unknown", v, n)
			}
		}
		for _, n := range gv.A64Lo12 {
			names = append(names, n)
		}
		for _, r := range gv.X86Data {
			names = append(names, r.Type)
		}
		for _, r := range gv.A64Data {
			names = append(names, r.Type)
		}
		for _, n := range names {
			if _, ok := m.Relocs[n]; !ok {
				t.Errorf("%s: relocation %s unknown", v, n)
			}
		}
	}
}
