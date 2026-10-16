// Checks that pcs inside goc-compiled C report their C file and line, and
// that an inlined C call shows up as its own frame (gocobj 1.4 FILE, LINE,
// INL_INDEX and the inline tree).
package main

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strings"
)

//go:noescape
func lines_direct(line *int64) int64

//go:noescape
func lines_inlined(x int64, inlLine, callLine *int64) int64

var failed bool

func check(what string, got, want any) {
	if fmt.Sprint(got) != fmt.Sprint(want) {
		fmt.Printf("FAIL %s: got %v, want %v\n", what, got, want)
		failed = true
	}
}

func main() {
	var line int64
	pc := uintptr(lines_direct(&line))
	f := runtime.FuncForPC(pc - 1)
	if f == nil {
		fmt.Println("FAIL no func for pc")
		os.Exit(1)
	}
	file, l := f.FileLine(pc - 1)
	check("direct file", filepath.Base(file), "lines.c")
	check("direct line", l, line)
	check("direct func", strings.TrimSuffix(f.Name(), ".impl"), "main.lines_direct")
	fmt.Printf("direct: %s %s:%d\n", f.Name(), filepath.Base(file), l)

	var inl, call int64
	pc = uintptr(lines_inlined(42, &inl, &call))
	// As in a stack trace: the C pc, then its Go caller. The runtime only
	// expands inlined frames when another pc follows.
	var up [1]uintptr
	runtime.Callers(1, up[:])
	fr := runtime.CallersFrames([]uintptr{pc, up[0]})
	a, more := fr.Next()
	// A static function's Go name carries its translation unit.
	check("inlined frame func", a.Function, "main.lines_c.inner")
	check("inlined frame file", filepath.Base(a.File), "lines.c")
	check("inlined frame line", a.Line, inl)
	check("more frames", more, true)
	if !expandsPCList() {
		// Before go1.23, CallersFrames gives one frame per pc unless the
		// list came from runtime.Callers (which adds the virtual pcs of
		// outer frames itself), so the outer frame cannot be checked here.
		fmt.Printf("inlined: %s %s:%d (%s: outer frame not expanded)\n", a.Function, filepath.Base(a.File), a.Line, runtime.Version())
		if failed {
			os.Exit(1)
		}
		fmt.Println("PASS lines")
		return
	}
	b, more := fr.Next()
	check("more frames after caller", more, true)
	check("caller func", strings.TrimSuffix(b.Function, ".impl"), "main.lines_inlined")
	check("caller line", b.Line, call)
	m, _ := fr.Next()
	check("go caller", m.Function, "main.main")
	fmt.Printf("inlined: %s %s:%d <- %s %s:%d <- %s\n", a.Function, filepath.Base(a.File), a.Line,
		b.Function, filepath.Base(b.File), b.Line, m.Function)
	if failed {
		os.Exit(1)
	}
	fmt.Println("PASS lines")
}

// expandsPCList reports whether runtime.CallersFrames inserts the virtual
// pcs of the frames an inlined pc expands to (go1.23+).
func expandsPCList() bool {
	var major, minor int
	if n, _ := fmt.Sscanf(runtime.Version(), "go%d.%d", &major, &minor); n < 2 {
		return true // devel
	}
	return major > 1 || minor >= 23
}
