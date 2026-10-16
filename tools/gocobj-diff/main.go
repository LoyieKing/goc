// gocobj-diff compares two Go object files semantically: symbols and their
// attributes, relocations (offset, size, type, target, addend), decoded
// pc-value tables (pcsp, pcfile, pcline, pcinline, pcdata), funcdata
// contents and FuncInfo. It is the migration check of plan row 5: the output
// of two converter builds must have no semantic difference.
//
//	gocobj-diff [-v] A.goobj B.goobj
//
// Exit status 0 means no difference, 1 a difference, 2 an error.
package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"flag"
	"fmt"
	"os"
	"sort"
	"strings"

	"github.com/LoyieKing/goc/goobjw"
)

var verbose = flag.Bool("v", false, "print every difference, not just the first 20")

type reloc struct {
	Off    int32
	Siz    uint8
	Type   uint16
	Target string
	Add    int64
}

type sym struct {
	Name        string
	ABI         uint16
	Type        uint8
	Flag, Flag2 uint8
	Siz, Align  uint32
	Data        []byte
	Relocs      []reloc
	Aux         map[string]string // aux kind -> rendered content
}

type object struct {
	Header string
	Syms   map[string]*sym
}

func load(path string) (*object, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	r, err := goobjw.NewReader(b, nil)
	if err != nil {
		return nil, fmt.Errorf("%s: %v", path, err)
	}
	hdr := r.Header
	pk := r.Meta.Object.PkgIdx
	at := r.Meta.Object.AuxTypes
	pkNone, pkSelf, pkH64, pkH, pkB := uint32(pk["None"]), uint32(pk["Self"]), uint32(pk["Hashed64"]), uint32(pk["Hashed"]), uint32(pk["Builtin"])
	quantum := 1
	if strings.Contains(hdr, "arm64") {
		quantum = 4
	}
	ndef := uint32(r.NDef())
	nall := ndef + uint32(r.NNonpkgref())
	// PkgIdxNone indexes the non-package symbols, defined ones first: the
	// same base the Go linker's loader uses (cmd/link/internal/loader).
	nonpkg := uint32(r.NSym() + r.NHashed64def() + r.NHasheddef())
	nameOf := func(i uint32) string {
		s := r.Sym(i)
		n := s.Name
		if n == "" && i < ndef { // content-addressed: name by content
			h := sha256.Sum256(r.Data(i))
			n = fmt.Sprintf("<hashed type=%d %x>", s.Type, h[:6])
		}
		return n
	}
	resolve := func(ref goobjw.SymRef) string {
		switch ref.PkgIdx {
		case pkNone:
			if k := nonpkg + ref.SymIdx; k < nall {
				return nameOf(k)
			}
		case pkSelf:
			if ref.SymIdx < ndef {
				return nameOf(ref.SymIdx)
			}
		case pkH64:
			return nameOf(uint32(r.NSym()) + ref.SymIdx)
		case pkH:
			return nameOf(uint32(r.NSym()+r.NHashed64def()) + ref.SymIdx)
		case pkB:
			n, abi := r.BuiltinName(int(ref.SymIdx))
			return fmt.Sprintf("%s<%d>", n, abi)
		}
		if ref.PkgIdx > 0 && int(ref.PkgIdx) < r.NPkg() {
			return fmt.Sprintf("%s#%d", r.Pkg(int(ref.PkgIdx)), ref.SymIdx)
		}
		return fmt.Sprintf("<ref %d:%d>", ref.PkgIdx, ref.SymIdx)
	}
	o := &object{Header: hdr, Syms: map[string]*sym{}}
	for i := uint32(0); i < ndef; i++ {
		s := r.Sym(i)
		y := &sym{Name: nameOf(i), ABI: s.ABI, Type: s.Type, Flag: s.Flag, Flag2: s.Flag2,
			Siz: s.Siz, Align: s.Align, Data: r.Data(i), Aux: map[string]string{}}
		for _, rl := range r.Relocs(i) {
			y.Relocs = append(y.Relocs, reloc{rl.Off, rl.Siz, rl.Type, resolve(rl.Sym), rl.Add})
		}
		sort.Slice(y.Relocs, func(a, b int) bool { return y.Relocs[a].Off < y.Relocs[b].Off })
		nfd, npd := 0, 0
		for _, a := range r.Auxs(i) {
			ref := a.Sym
			var data []byte
			if ref.PkgIdx == pkSelf && ref.SymIdx < ndef {
				data = r.Data(ref.SymIdx)
			} else if ref.PkgIdx == pkNone && nonpkg+ref.SymIdx < ndef {
				data = r.Data(nonpkg + ref.SymIdx)
			} else if ref.PkgIdx == pkH64 {
				data = r.Data(uint32(r.NSym()) + ref.SymIdx)
			} else if ref.PkgIdx == pkH {
				data = r.Data(uint32(r.NSym()+r.NHashed64def()) + ref.SymIdx)
			}
			switch int(a.Type) {
			case at["Pcsp"]:
				y.Aux["pcsp"] = pcvalue(data, quantum)
			case at["Pcfile"]:
				y.Aux["pcfile"] = pcvalue(data, quantum)
			case at["Pcline"]:
				y.Aux["pcline"] = pcvalue(data, quantum)
			case at["Pcinline"]:
				y.Aux["pcinline"] = pcvalue(data, quantum)
			case at["Pcdata"]:
				y.Aux[fmt.Sprintf("pcdata%d", npd)] = pcvalue(data, quantum)
				npd++
			case at["Funcdata"]:
				y.Aux[fmt.Sprintf("funcdata%d", nfd)] = fmt.Sprintf("%x", data)
				nfd++
			case at["FuncInfo"]:
				y.Aux["funcinfo"] = funcinfo(data)
			case at["Gotype"]:
				y.Aux["gotype"] = resolve(ref)
			}
		}
		o.Syms[fmt.Sprintf("%s<%d>", y.Name, y.ABI)] = y
	}
	return o, nil
}

// pcvalue decodes Go's pc-value encoding into "[start,end)=value" spans.
func pcvalue(b []byte, quantum int) string {
	if b == nil {
		return "<none>"
	}
	var out []string
	val, pc := int64(-1), uint64(0)
	first := true
	for len(b) > 0 {
		uv, n := binary.Uvarint(b)
		if n <= 0 {
			return "<bad>"
		}
		b = b[n:]
		if uv == 0 && !first {
			break
		}
		first = false
		val += int64(uv>>1) ^ -int64(uv&1)
		d, n := binary.Uvarint(b)
		if n <= 0 {
			return "<bad>"
		}
		b = b[n:]
		end := pc + d*uint64(quantum)
		out = append(out, fmt.Sprintf("[%d,%d)=%d", pc, end, val))
		pc = end
	}
	return strings.Join(out, " ")
}

func funcinfo(b []byte) string {
	fi, err := goobjw.DecodeFuncInfo(b)
	if err != nil {
		return fmt.Sprintf("%x", b)
	}
	return fmt.Sprintf("args=%d locals=%d funcid=%d flag=%d startline=%d files=%v inl=%v",
		fi.Args, fi.Locals, fi.FuncID, fi.FuncFlag, fi.StartLine, fi.File, fi.InlTree)
}

func main() {
	flag.Parse()
	if flag.NArg() != 2 {
		fmt.Fprintln(os.Stderr, "usage: gocobj-diff [-v] A.goobj B.goobj")
		os.Exit(2)
	}
	a, err := load(flag.Arg(0))
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}
	b, err := load(flag.Arg(1))
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}
	diffs := compare(a, b)
	limit := 20
	for i, d := range diffs {
		if i == limit && !*verbose {
			fmt.Printf("... %d more\n", len(diffs)-limit)
			break
		}
		fmt.Println(d)
	}
	if len(diffs) > 0 {
		fmt.Printf("gocobj-diff: %d differences\n", len(diffs))
		os.Exit(1)
	}
	fmt.Printf("gocobj-diff: no semantic difference (%d symbols)\n", len(a.Syms))
}

func compare(a, b *object) []string {
	var d []string
	if a.Header != b.Header {
		d = append(d, fmt.Sprintf("header: %q vs %q", a.Header, b.Header))
	}
	keys := map[string]bool{}
	for k := range a.Syms {
		keys[k] = true
	}
	for k := range b.Syms {
		keys[k] = true
	}
	sorted := make([]string, 0, len(keys))
	for k := range keys {
		sorted = append(sorted, k)
	}
	sort.Strings(sorted)
	for _, k := range sorted {
		x, y := a.Syms[k], b.Syms[k]
		switch {
		case x == nil:
			d = append(d, "only in B: "+k)
			continue
		case y == nil:
			d = append(d, "only in A: "+k)
			continue
		}
		if x.Type != y.Type || x.Flag != y.Flag || x.Flag2 != y.Flag2 || x.Siz != y.Siz || x.Align != y.Align {
			d = append(d, fmt.Sprintf("%s: attrs type=%d/%d flag=%#x/%#x flag2=%#x/%#x size=%d/%d align=%d/%d",
				k, x.Type, y.Type, x.Flag, y.Flag, x.Flag2, y.Flag2, x.Siz, y.Siz, x.Align, y.Align))
		}
		if !bytes.Equal(x.Data, y.Data) {
			n := 0
			for i := 0; i < len(x.Data) && i < len(y.Data); i++ {
				if x.Data[i] != y.Data[i] {
					n++
				}
			}
			d = append(d, fmt.Sprintf("%s: data differs (%d bytes, len %d/%d)", k, n, len(x.Data), len(y.Data)))
		}
		if fmt.Sprint(x.Relocs) != fmt.Sprint(y.Relocs) {
			d = append(d, fmt.Sprintf("%s: relocs differ: %s", k, firstRelocDiff(x.Relocs, y.Relocs)))
		}
		for _, ak := range unionKeys(x.Aux, y.Aux) {
			if x.Aux[ak] != y.Aux[ak] {
				d = append(d, fmt.Sprintf("%s: %s differs:\n  A %s\n  B %s", k, ak, clip(x.Aux[ak]), clip(y.Aux[ak])))
			}
		}
	}
	return d
}

func firstRelocDiff(a, b []reloc) string {
	for i := 0; i < len(a) || i < len(b); i++ {
		var x, y any = "-", "-"
		if i < len(a) {
			x = a[i]
		}
		if i < len(b) {
			y = b[i]
		}
		if fmt.Sprint(x) != fmt.Sprint(y) {
			return fmt.Sprintf("#%d %v vs %v (counts %d/%d)", i, x, y, len(a), len(b))
		}
	}
	return ""
}

func unionKeys(a, b map[string]string) []string {
	m := map[string]bool{}
	for k := range a {
		m[k] = true
	}
	for k := range b {
		m[k] = true
	}
	out := make([]string, 0, len(m))
	for k := range m {
		out = append(out, k)
	}
	sort.Strings(out)
	return out
}

func clip(s string) string {
	if len(s) > 200 {
		return s[:200] + "..."
	}
	return s
}
