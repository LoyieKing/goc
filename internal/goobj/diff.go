// Diff compares two Go diffObject files semantically: symbols and their
// attributes, relocations (offset, size, type, target, addend), decoded
// pc-value tables (pcsp, pcfile, pcline, pcinline, pcdata), funcdata
// contents and FuncInfo. It is the migration check of plan row 5: the output
// of two converter builds must have no semantic difference.
package goobj

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"fmt"
	"os"
	"sort"
	"strings"
)

type diffReloc struct {
	Off    int32
	Siz    uint8
	Type   string // objabi name: the numbers differ between releases
	Target string
	Add    int64
}

type diffSym struct {
	Name        string
	ABI         uint16
	Type        string // symbol kind name
	Flag, Flag2 string // flag names
	Siz, Align  uint32
	Data        []byte
	Relocs      []diffReloc
	Aux         map[string]string // aux kind -> rendered content
}

type diffObject struct {
	Header string
	Syms   map[string]*diffSym
}

func diffLoad(path string) (*diffObject, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	r, err := NewReader(b, nil)
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
	resolve := func(ref SymRef) string {
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
	o := &diffObject{Header: hdr, Syms: map[string]*diffSym{}}
	for i := uint32(0); i < ndef; i++ {
		s := r.Sym(i)
		y := &diffSym{Name: nameOf(i), ABI: s.ABI, Type: diffNameFor(r.Meta.Kinds, int(s.Type)),
			Flag: diffFlagNames(r.Meta.Object.SymFlags, s.Flag), Flag2: diffFlagNames(r.Meta.Object.SymFlags2, s.Flag2),
			Siz: s.Siz, Align: s.Align, Data: r.Data(i), Aux: map[string]string{}}
		for _, rl := range r.Relocs(i) {
			y.Relocs = append(y.Relocs, diffReloc{rl.Off, rl.Siz, diffNameFor(r.Meta.Relocs, int(rl.Type)), resolve(rl.Sym), rl.Add})
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
				y.Aux["pcsp"] = diffPCValue(data, quantum)
			case at["Pcfile"]:
				y.Aux["pcfile"] = diffPCValue(data, quantum)
			case at["Pcline"]:
				y.Aux["pcline"] = diffPCValue(data, quantum)
			case at["Pcinline"]:
				y.Aux["pcinline"] = diffPCValue(data, quantum)
			case at["Pcdata"]:
				y.Aux[fmt.Sprintf("pcdata%d", npd)] = diffPCValue(data, quantum)
				npd++
			case at["Funcdata"]:
				y.Aux[fmt.Sprintf("funcdata%d", nfd)] = fmt.Sprintf("%x", data)
				nfd++
			case at["FuncInfo"]:
				y.Aux["funcinfo"] = diffFuncInfo(r, data)
			case at["DwarfInfo"]:
				y.Aux["dwarfinfo"] = fmt.Sprintf("%x", data)
			case at["DwarfLines"]:
				y.Aux["dwarflines"] = fmt.Sprintf("%x", data)
			case at["Gotype"]:
				y.Aux["gotype"] = resolve(ref)
			}
		}
		o.Syms[fmt.Sprintf("%s<%d>", y.Name, y.ABI)] = y
	}
	return o, nil
}

// pcvalue decodes Go's pc-value encoding into "[start,end)=value" spans.
func diffPCValue(b []byte, quantum int) string {
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

func diffFuncInfo(r *Reader, b []byte) string {
	fi, err := r.DecodeFuncInfo(b)
	if err != nil {
		return fmt.Sprintf("%x", b)
	}
	return fmt.Sprintf("args=%d locals=%d funcid=%d flag=%d startline=%d files=%v inl=%v",
		fi.Args, fi.Locals, fi.FuncID, fi.FuncFlag, fi.StartLine, fi.File, fi.InlTree)
}

// Diff compares the Go diffObject files at pathA and pathB semantically. It
// returns the number of symbols in A and one line per difference.
func Diff(pathA, pathB string) (int, []string, error) {
	a, err := diffLoad(pathA)
	if err != nil {
		return 0, nil, err
	}
	b, err := diffLoad(pathB)
	if err != nil {
		return 0, nil, err
	}
	return len(a.Syms), diffCompare(a, b), nil
}

func diffCompare(a, b *diffObject) []string {
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
			d = append(d, fmt.Sprintf("%s: attrs type=%s/%s flag=[%s]/[%s] flag2=[%s]/[%s] size=%d/%d align=%d/%d",
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
			d = append(d, fmt.Sprintf("%s: relocs differ: %s", k, diffFirstReloc(x.Relocs, y.Relocs)))
		}
		for _, ak := range diffUnionKeys(x.Aux, y.Aux) {
			if x.Aux[ak] != y.Aux[ak] {
				d = append(d, fmt.Sprintf("%s: %s differs:\n  A %s\n  B %s", k, ak, diffClip(x.Aux[ak]), diffClip(y.Aux[ak])))
			}
		}
	}
	return d
}

// nameFor returns the name a release gives a number (the first, sorted,
// when several share it).
func diffNameFor(m map[string]int, v int) string {
	var names []string
	for n, x := range m {
		if x == v {
			names = append(names, n)
		}
	}
	if len(names) == 0 {
		return fmt.Sprintf("#%d", v)
	}
	sort.Strings(names)
	return names[0]
}

func diffFlagNames(m map[string]int, v uint8) string {
	var names []string
	rest := int(v)
	for n, bit := range m {
		if bit != 0 && int(v)&bit == bit {
			names = append(names, n)
			rest &^= bit
		}
	}
	sort.Strings(names)
	if rest != 0 {
		names = append(names, fmt.Sprintf("%#x", rest))
	}
	return strings.Join(names, ",")
}

func diffFirstReloc(a, b []diffReloc) string {
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

func diffUnionKeys(a, b map[string]string) []string {
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

func diffClip(s string) string {
	if len(s) > 200 {
		return s[:200] + "..."
	}
	return s
}
