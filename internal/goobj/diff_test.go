package goobj

import (
	"path/filepath"
	"testing"
)

// An object has no semantic difference from itself; two different objects
// do (and the first lines name what differs).
func TestDiffGolden(t *testing.T) {
	files, _ := filepath.Glob("../convert/testdata/golden/go1.*/*.goobj")
	if len(files) < 2 {
		t.Fatal("need two golden objects")
	}
	a, b := files[0], files[len(files)-1]
	n, d, err := Diff(a, a)
	if err != nil {
		t.Fatal(err)
	}
	if n == 0 || len(d) != 0 {
		t.Fatalf("%s vs itself: %d symbols, differences %q", a, n, d)
	}
	if _, d, err = Diff(a, b); err != nil {
		t.Fatal(err)
	}
	if len(d) == 0 {
		t.Fatalf("%s and %s compared equal", a, b)
	}
}
