package convert

import (
	"bufio"
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/LoyieKing/goc/internal/gocobj"
	"github.com/LoyieKing/goc/internal/gometa"
)

// testdata/golden holds .gocobj files and the objects the old converter
// (built against each release's cmd/internal/obj writer, go1.23-1.27) or,
// for go1.18-1.22, this converter when those releases were added (checked
// with goc tool objdump diff against go1.23 and by linking: docs/design/go-versions.md)
// made of them. The converter must reproduce them byte for byte. arm64 has
// goldens from go1.21, the first release goc supports it on.
func TestGolden(t *testing.T) {
	ins, _ := filepath.Glob("testdata/golden/*.gocobj")
	vers, _ := filepath.Glob("testdata/golden/go1.*")
	if len(ins) == 0 || len(vers) == 0 {
		t.Fatal("no golden inputs")
	}
	for _, vdir := range vers {
		ver := filepath.Base(vdir)
		meta, err := gometa.ForVersion(ver)
		if err != nil {
			t.Fatal(err)
		}
		for _, in := range ins {
			name := strings.TrimSuffix(filepath.Base(in), ".gocobj")
			args, err := os.ReadFile(strings.TrimSuffix(in, ".gocobj") + ".args")
			if err != nil {
				t.Fatal(err)
			}
			f := strings.Fields(string(args))
			pkg, goarch := f[0], f[1]
			want, err := os.ReadFile(filepath.Join(vdir, name+".goobj"))
			if os.IsNotExist(err) && goarch == "arm64" && !meta.Arch["arm64"].StackCopyAdjustsFP {
				continue
			}
			if err != nil {
				t.Fatal(err)
			}
			hdr, _ := bufio.NewReader(bytes.NewReader(want)).ReadString('\n')
			obj, err := gocobj.Open(in)
			if err != nil {
				t.Fatal(err)
			}
			c, err := newConverter(in, obj, pkg, goarch, "")
			if err != nil {
				t.Fatal(err)
			}
			out := filepath.Join(t.TempDir(), name+".goobj")
			if err := c.write(out, meta, hdr); err != nil {
				t.Fatalf("%s %s: %v", ver, name, err)
			}
			got, _ := os.ReadFile(out)
			if !bytes.Equal(got, want) {
				t.Errorf("%s %s: object differs from golden (inspect with goc tool objdump diff)", ver, name)
			}
			wantSA, _ := os.ReadFile(filepath.Join(vdir, name+".symabis"))
			if c.symabis() != string(wantSA) {
				t.Errorf("%s %s: symabis differ", ver, name)
			}
		}
	}
}
