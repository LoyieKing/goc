package convert

import (
	"testing"

	"github.com/LoyieKing/goc/internal/gocobj"
	"github.com/LoyieKing/goc/internal/gometa"
)

// Every supported release resolves to a goc ABI profile for each arch it
// supports (arm64 from go1.21, amd64 always), and every relocation name the
// converter emits for those arches is known to that release.
func TestProfilesAndRelocsKnown(t *testing.T) {
	for _, v := range gometa.Releases() {
		m, err := gometa.ForVersion(v)
		if err != nil {
			t.Fatal(err)
		}
		arm64 := m.Arch["arm64"].StackCopyAdjustsFP
		if g, _ := gometa.Minor(m.Go); arm64 != (g >= "go1.21") {
			t.Errorf("%s: arm64 support %v", v, arm64)
		}
		if !m.Arch["amd64"].StackCopyAdjustsFP {
			t.Errorf("%s: amd64 unsupported", v)
		}
		for _, arch := range []string{"amd64", "arm64"} {
			if arch == "arm64" && !arm64 {
				if _, err := profileFor(m, arch); err == nil {
					t.Errorf("%s: arm64 profile despite no frame-pointer adjustment", v)
				}
				continue
			}
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
		names := []string{gv.X86Call}
		for _, n := range gv.X86Text {
			names = append(names, n)
		}
		for _, r := range gv.X86Data {
			names = append(names, r.Type)
		}
		if arm64 {
			names = append(names, gv.A64Call, gv.A64Page, gv.A64GOTPair)
			for _, n := range gv.A64PagePair {
				names = append(names, n)
			}
			for _, n := range gv.A64Lo12 {
				names = append(names, n)
			}
			for _, r := range gv.A64Data {
				names = append(names, r.Type)
			}
		}
		for _, n := range names {
			if _, ok := m.Relocs[n]; !ok {
				t.Errorf("%s: relocation %s unknown", v, n)
			}
		}
	}
}
