package goobjw

import (
	"bytes"
	"os"
	"path/filepath"
	"testing"
)

// The reader decodes the converter's golden objects of every release, and
// every defined symbol's relocations and aux symbols point into the file.
func TestReadGolden(t *testing.T) {
	files, _ := filepath.Glob("../tools/gocobj2goobj/testdata/golden/go1.*/*.goobj")
	if len(files) == 0 {
		t.Skip("no golden objects")
	}
	for _, p := range files {
		b, err := os.ReadFile(p)
		if err != nil {
			t.Fatal(err)
		}
		r, err := NewReader(b, nil)
		if err != nil {
			t.Fatalf("%s: %v", p, err)
		}
		if r.NDef() == 0 || r.NPkg() < 1 || len(r.Files()) == 0 {
			t.Errorf("%s: empty object", p)
		}
		for i := uint32(0); i < uint32(r.NDef()); i++ {
			s := r.Sym(i)
			if int(s.Siz) < len(r.Data(i)) {
				t.Errorf("%s: %s: %d data bytes > size %d", p, s.Name, len(r.Data(i)), s.Siz)
			}
			for _, a := range r.Auxs(i) {
				if a.Type == 0 {
					t.Errorf("%s: %s: aux type 0", p, s.Name)
				}
			}
			for _, rl := range r.Relocs(i) {
				if rl.Off < 0 || uint32(rl.Off)+uint32(rl.Siz) > s.Siz {
					t.Errorf("%s: %s: reloc at %d+%d outside %d", p, s.Name, rl.Off, rl.Siz, s.Siz)
				}
			}
		}
		if !bytes.Contains(b, []byte(r.Meta.Object.Magic)) {
			t.Errorf("%s: no magic", p)
		}
	}
}
