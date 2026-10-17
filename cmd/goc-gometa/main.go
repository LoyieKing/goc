// goc-gometa reads a Go release's sources and writes the
// facts gocobj2goobj needs to produce object files for that release, as
// JSON (schema: docs/go-metadata-schema.md):
//
//	goc-gometa [-goroot DIR] [-o internal/gometa/data/go1.N.json]
//
// Every fact comes from a file under GOROOT/src: constants are evaluated
// from their declarations (iota included), tables from their composite
// literals. Writer behaviours (which hash, which alignment rule, which
// DWARF layout) are read from the functions that implement them, and only
// if those functions match a version this tool's writer was checked
// against (reviewed.go); a changed function is an error, never a guess.
package main

import (
	"flag"
	"fmt"
	"go/ast"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"

	"github.com/LoyieKing/goc/internal/gometa"
)

type resolver struct {
	goroot string
	files  map[string]*srcFile
	used   map[string]bool
	errs   []string
}

func main() {
	goroot := flag.String("goroot", "", "GOROOT to read (default: go env GOROOT)")
	out := flag.String("o", "", "output file (default stdout)")
	fingerprints := flag.Bool("fingerprints", false, "print the reviewed functions' fingerprints and exit")
	printFunc := flag.String("print", "", "print a reviewed function (file:func), comments stripped, and exit (for reviewing a change)")
	keepGoing := flag.Bool("keep-going", false, "report errors but still write what was resolved (surveying a new release; the result is not for internal/gometa/data)")
	flag.Parse()
	if *goroot == "" {
		b, err := exec.Command("go", "env", "GOROOT").Output()
		if err != nil {
			fmt.Fprintln(os.Stderr, "goc-gometa: go env GOROOT:", err)
			os.Exit(1)
		}
		*goroot = strings.TrimSpace(string(b))
	}
	r := &resolver{goroot: *goroot, files: map[string]*srcFile{}, used: map[string]bool{}}
	if *printFunc != "" {
		fmt.Println(r.normalized(*printFunc))
		return
	}
	if *fingerprints {
		for _, k := range reviewedKeys() {
			fmt.Printf("%s\t%s\n", k, r.fingerprint(k))
		}
		return
	}
	m := r.resolve()
	if len(r.errs) > 0 {
		for _, e := range r.errs {
			fmt.Fprintln(os.Stderr, "goc-gometa:", e)
		}
		if !*keepGoing {
			os.Exit(1)
		}
	}
	b, err := m.Encode()
	if err == nil && !*keepGoing {
		_, err = gometa.Parse(b) // the converter must accept what we write
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, "goc-gometa:", err)
		os.Exit(1)
	}
	if *out == "" {
		os.Stdout.Write(b)
		return
	}
	if err := os.WriteFile(*out, b, 0o644); err != nil {
		fmt.Fprintln(os.Stderr, "goc-gometa:", err)
		os.Exit(1)
	}
}

func (r *resolver) fatalf(format string, args ...any) {
	fmt.Fprintf(os.Stderr, "goc-gometa: "+format+"\n", args...)
	os.Exit(1)
}

func (r *resolver) errorf(format string, args ...any) {
	r.errs = append(r.errs, fmt.Sprintf(format, args...))
}

func (r *resolver) version() string {
	b, err := os.ReadFile(filepath.Join(r.goroot, "VERSION"))
	if err != nil {
		r.fatalf("%v", err)
	}
	return strings.TrimSpace(strings.SplitN(string(b), "\n", 2)[0])
}

func (r *resolver) resolve() *gometa.Meta {
	ver := r.version()
	minor, ok := gometa.Minor(ver)
	if !ok {
		r.fatalf("cannot parse GOROOT/VERSION %q", ver)
	}
	m := &gometa.Meta{
		Schema:        gometa.Schema,
		Go:            minor,
		SourceVersion: ver,
		Resolver:      "cmd/goc-gometa",
		Sources:       map[string]string{},
	}
	r.checkReviewed()
	src := func(group string, files ...string) {
		m.Sources[group] = strings.Join(files, " ")
	}

	// Object container format.
	gf := r.parse("cmd/internal/goobj/objfile.go")
	gb := r.consts(gf, nil)
	o := &m.Object
	for _, b := range gb {
		if s, ok := b.strs["Magic"]; ok {
			o.Magic = s
		}
	}
	o.HeaderFormat = r.headerFormat()
	if blk, ok := blockWith(gb, "BlkAutolib"); ok {
		for _, n := range blk.names {
			if n != "NBlk" {
				o.Blocks = append(o.Blocks, strings.TrimPrefix(n, "Blk"))
			}
		}
	}
	o.ObjFlags = pickTrim(gb, "ObjFlag")
	if blk, ok := blockWith(gb, "SymFlagDupok"); ok {
		o.SymFlags = trimmed(blk, "SymFlag")
	}
	if blk, ok := blockWith(gb, "SymFlagUsedInIface"); ok {
		o.SymFlags2 = trimmed(blk, "SymFlag")
	}
	o.AuxTypes = pickTrim(gb, "Aux")
	o.PkgIdx = pickTrim(gb, "PkgIdx")
	delete(o.PkgIdx, "Special")
	delete(o.PkgIdx, "Invalid")
	all := pick(gb, "")
	o.SymABIStatic = all["SymABIstatic"]
	o.HashSize = all["HashSize"]
	if o.HashSize == 0 && regexp.MustCompile(`const HashSize = sha1\.Size\b`).Match(gf.text) {
		o.HashSize = 20 // go1.18
	}
	o.Hash64Size = all["Hash64Size"]
	if bl := compositeVar(r.parse("cmd/internal/goobj/builtinlist.go"), "builtins"); bl != nil {
		for _, e := range bl.Elts {
			cl, ok := e.(*ast.CompositeLit)
			if !ok || len(cl.Elts) != 2 {
				r.errorf("goobj builtins: unexpected entry")
				continue
			}
			name, ok1 := litString(cl.Elts[0])
			abi, ok2 := litInt(cl.Elts[1])
			if !ok1 || !ok2 {
				r.errorf("goobj builtins: unexpected entry")
				continue
			}
			o.Builtins = append(o.Builtins, gometa.Builtin{Name: name, ABI: abi})
		}
	} else {
		r.errorf("cmd/internal/goobj/builtinlist.go: no builtins table")
	}
	src("object", "cmd/internal/goobj/objfile.go", "cmd/internal/goobj/builtinlist.go", "cmd/internal/objabi/util.go")

	// Relocation types and symbol kinds.
	m.Relocs = pick(r.consts(r.parse("cmd/internal/objabi/reloctype.go"), nil), "R_")
	m.Kinds = map[string]int{}
	if blk, ok := blockWith(r.consts(r.parse("cmd/internal/objabi/symkind.go"), nil), "Sxxx"); ok {
		for _, n := range blk.names {
			m.Kinds[n] = int(blk.values[n])
		}
	}
	src("relocs", "cmd/internal/objabi/reloctype.go")
	src("sym_kinds", "cmd/internal/objabi/symkind.go")

	// internal/abi (go1.21+); cmd/internal/objabi before that.
	var sym []constBlock
	abiSrc := []string{"internal/abi/symtab.go", "internal/abi/stack.go"}
	if r.exists("internal/abi/symtab.go") {
		sym = r.consts(r.parse("internal/abi/symtab.go"), nil)
		m.ABI.Funcdata = pickTrim(sym, "FUNCDATA_")
		m.ABI.Pcdata = pickTrim(sym, "PCDATA_")
		m.Writer.FuncFlags = pickTrim(sym, "FuncFlag")
		m.Writer.FuncIDs = map[string]int{}
		for n, v := range pick(sym, "FuncID") {
			m.Writer.FuncIDs[strings.TrimPrefix(strings.TrimPrefix(n, "FuncID"), "_")] = v
		}
		stack := pick(r.consts(r.parse("internal/abi/stack.go"), nil), "Stack")
		m.ABI.StackSmall = stack["StackSmall"]
		m.ABI.StackBig = stack["StackBig"]
		m.ABI.StackNosplitBase = stack["StackNosplitBase"]
	} else {
		abiSrc = []string{"cmd/internal/objabi/funcdata.go", "cmd/internal/objabi/funcid.go", "cmd/internal/objabi/stack.go"}
		fd := r.consts(r.parse("cmd/internal/objabi/funcdata.go"), nil)
		m.ABI.Funcdata = pickTrim(fd, "FUNCDATA_")
		m.ABI.Pcdata = pickTrim(fd, "PCDATA_")
		sym = r.consts(r.parse("cmd/internal/objabi/funcid.go"), nil)
		// Same values under internal/abi's later names.
		flagNames := map[string]string{"TOPFRAME": "TopFrame", "SPWRITE": "SPWrite", "ASM": "Asm"}
		m.Writer.FuncFlags = map[string]int{}
		for n, v := range pickTrim(sym, "FuncFlag_") {
			if nn, ok := flagNames[n]; ok {
				m.Writer.FuncFlags[nn] = v
			} else {
				r.errorf("cmd/internal/objabi/funcid.go: unknown FuncFlag_%s", n)
			}
		}
		m.Writer.FuncIDs = map[string]int{}
		for n, v := range pickTrim(sym, "FuncID_") {
			switch n {
			case "normal":
				n = "Normal"
			case "wrapper":
				n = "Wrapper"
			}
			m.Writer.FuncIDs[n] = v
		}
		st := r.parse("cmd/internal/objabi/stack.go")
		stack := pick(r.consts(st, nil), "Stack")
		m.ABI.StackSmall = stack["StackSmall"]
		m.ABI.StackBig = stack["StackBig"]
		// StackLimit = stackGuard - StackSystem - StackSmall: the nosplit
		// budget that go1.21 names StackNosplitBase.
		mm := regexp.MustCompile(`stackGuard := (\d+)\*stackGuardMultiplier\(race\) \+ StackSystem\s+stackLimit := stackGuard - StackSystem - StackSmall`).FindStringSubmatch(string(st.text))
		if mm == nil { // go1.18, 1.19
			mm = regexp.MustCompile(`var StackGuard = (\d+)\*stackGuardMultiplier\(\) \+ StackSystem\s+var StackLimit = StackGuard - StackSystem - StackSmall`).FindStringSubmatch(string(st.text))
		}
		if mm == nil {
			r.errorf("cmd/internal/objabi/stack.go: StackLimit has an unknown layout")
		} else {
			g, _ := strconv.Atoi(mm[1])
			m.ABI.StackNosplitBase = g - m.ABI.StackSmall
		}
	}
	m.ABI.GOffsets = r.gOffsets()
	src("abi", append(abiSrc, "runtime/runtime2.go")...)

	// objabi.funcIDs: runtime.<name> -> FuncID.
	m.Writer.RuntimeFuncIDs = map[string]int{}
	abiNames := map[string]int64{}
	for n, v := range pick(sym, "FuncID") {
		abiNames["abi."+n] = int64(v)
		abiNames[n] = int64(v)
	}
	if cl := compositeVar(r.parse("cmd/internal/objabi/funcid.go"), "funcIDs"); cl != nil {
		for _, e := range cl.Elts {
			kv, ok := e.(*ast.KeyValueExpr)
			if !ok {
				continue
			}
			name, ok := litString(kv.Key)
			if !ok {
				continue
			}
			v, err := evalConst(kv.Value, abiNames, 0, 0)
			if err != nil {
				r.errorf("cmd/internal/objabi/funcid.go: funcIDs[%q]: %v", name, err)
				continue
			}
			m.Writer.RuntimeFuncIDs[name] = int(v)
		}
	} else {
		r.errorf("cmd/internal/objabi/funcid.go: no funcIDs table")
	}

	// Architectures.
	m.Arch = map[string]gometa.Arch{}
	archf := r.parse("cmd/internal/sys/arch.go")
	for _, a := range []string{"amd64", "arm64"} {
		ar := gometa.Arch{}
		cl := compositeVar(archf, "Arch"+strings.ToUpper(a))
		if cl == nil {
			r.errorf("cmd/internal/sys/arch.go: no Arch%s", strings.ToUpper(a))
			continue
		}
		for _, e := range cl.Elts {
			kv, ok := e.(*ast.KeyValueExpr)
			if !ok {
				continue
			}
			k, _ := kv.Key.(*ast.Ident)
			if k == nil {
				continue
			}
			switch k.Name {
			case "PtrSize":
				ar.PtrSize, _ = litInt(kv.Value)
			case "MinLC":
				ar.MinLC, _ = litInt(kv.Value)
			}
		}
		r.regs(a, &ar)
		ar.StackCopyAdjustsFP = r.stackCopyAdjustsFP(a)
		m.Arch[a] = ar
	}
	src("arch", "cmd/internal/sys/arch.go", r.ssaGen()+"/AMD64Ops.go",
		r.ssaGen()+"/ARM64Ops.go", "runtime/asm_amd64.s", "runtime/asm_arm64.s", "runtime/stack.go")

	hashSrc := r.writerFacts(m)
	r.dwarfFacts(m)
	src("writer", "cmd/internal/obj/objfile.go", "cmd/internal/obj/plist.go", hashSrc)
	src("dwarf", "cmd/internal/dwarf/dwarf.go", "cmd/internal/obj/dwarf.go", "cmd/internal/objabi/reloctype.go")

	_, err := os.Stat(filepath.Join(r.goroot, "pkg", "tool", "linux_amd64", "pack"))
	m.Tools.PackPrebuilt = err == nil
	src("toolexec", "pkg/tool/linux_amd64 (directory listing)")
	return m
}

func trimmed(b constBlock, prefix string) map[string]int {
	out := map[string]int{}
	for _, n := range b.names {
		out[strings.TrimPrefix(n, prefix)] = int(b.values[n])
	}
	return out
}

// headerFormat checks objabi.HeaderString is the layout the converter's
// header probe expects and returns it.
func (r *resolver) headerFormat() string {
	body := r.funcSource("cmd/internal/objabi/util.go", "HeaderString")
	if strings.Contains(body, `"go object %s %s %s X:%s\n"`) { // go1.18, 1.19
		return "go object {GOOS} {GOARCH} {GOVERSION} X:{experiments}\\n"
	}
	if !strings.Contains(body, `"go object %s %s %s%s X:%s\n"`) {
		r.errorf("cmd/internal/objabi/util.go: HeaderString has an unknown layout")
	}
	return "go object {GOOS} {GOARCH} {GOVERSION}[ {GOARCHVAR}={value}] X:{experiments}\\n"
}

// gOffsets reads the start of runtime.g: stack (lo, hi uintptr), then
// stackguard0 uintptr.
func (r *resolver) gOffsets() map[string]int {
	rt := string(r.parse("runtime/runtime2.go").text)
	if !regexp.MustCompile(`type stack struct \{\s*lo\s+uintptr\s*hi\s+uintptr\s*\}`).MatchString(rt) ||
		!regexp.MustCompile(`type g struct \{(?:\s*//[^\n]*)*\s*stack\s+stack[^\n]*\n\s*stackguard0\s+uintptr`).MatchString(rt) {
		r.errorf("runtime/runtime2.go: g does not start with stack{lo,hi} and stackguard0")
		return nil
	}
	return map[string]int{"stack_lo": 0, "stack_hi": 8, "stackguard0": 16}
}

func (r *resolver) regs(arch string, ar *gometa.Arch) {
	ops := string(r.parse(r.ssaGen() + "/" + strings.ToUpper(arch) + "Ops.go").text)
	find := func(re, what string) string {
		m := regexp.MustCompile(re).FindStringSubmatch(ops)
		if m == nil {
			r.errorf("%sOps.go: %s not found", strings.ToUpper(arch), what)
			return ""
		}
		return m[1]
	}
	ar.GReg = find(`"g",\s*//\s*a\.?k\.?a\.?\s*(R\d+)`, "g register")
	ar.IntArgs = strings.Fields(find(`ParamIntRegNames:\s*"([^"]*)"`, "ParamIntRegNames"))
	ar.FloatArgs = strings.Fields(find(`ParamFloatRegNames:\s*"([^"]*)"`, "ParamFloatRegNames"))
	asm, err := os.ReadFile(filepath.Join(r.goroot, "src", "runtime", "asm_"+arch+".s"))
	if err != nil {
		r.errorf("%v", err)
		return
	}
	if strings.Contains(string(asm), "TEXT runtime·morestack_noctxt(") {
		ar.Morestack = "runtime.morestack_noctxt"
	} else {
		r.errorf("runtime/asm_%s.s: no runtime·morestack_noctxt", arch)
	}
}

func (r *resolver) writerFacts(m *gometa.Meta) (hashSrc string) {
	w := &m.Writer
	ch := r.funcSource("cmd/internal/obj/objfile.go", "(*writer).contentHash")
	switch {
	case strings.Contains(ch, "notsha256.New()"):
		w.ContentHash = "notsha256"
		hashSrc = "cmd/internal/notsha256/sha256.go"
		ns := r.funcSource("cmd/internal/notsha256/sha256.go", "(*digest).checkSum")
		if !strings.Contains(ns, "d.h[0]^0xFFFFFFFF") {
			r.errorf("cmd/internal/notsha256: checkSum no longer complements the sha256 words")
		}
	case strings.Contains(ch, "hash.New20()"), strings.Contains(ch, "hash.New32()"):
		w.ContentHash = "sha256-prefix01"
		hashSrc = "cmd/internal/hash/hash.go"
		n32 := r.funcSource("cmd/internal/hash/hash.go", "New32")
		if !strings.Contains(n32, "sha256.New()") || !strings.Contains(n32, "h.Write([]byte{1})") {
			r.errorf("cmd/internal/hash: New32 is no longer sha256 over 0x01 || data")
		}
		if m.Object.HashSize > 20 {
			r.errorf("goobj.HashSize %d exceeds the 20 bytes hash.New20 yields", m.Object.HashSize)
		}
	case strings.Contains(ch, "sha1.New()"): // go1.18
		w.ContentHash = "sha1"
		hashSrc = "crypto/sha1"
		if m.Object.HashSize != 20 {
			r.errorf("goobj.HashSize %d is not sha1's 20 bytes", m.Object.HashSize)
		}
	default:
		r.errorf("cmd/internal/obj/objfile.go: contentHash uses an unknown hash")
	}
	sym := r.funcSource("cmd/internal/obj/objfile.go", "(*writer).Sym")
	w.AsmNamedSymsLinkname = strings.Contains(sym, `w.ctxt.IsAsm && name != ""`)
	switch {
	case strings.Contains(sym, "align := uint32(s.Align)"):
		w.Align = "explicit"
	case strings.Contains(sym, `strings.HasPrefix(name, "gclocals·")`) && strings.Contains(sym, "align = uint32(fn.Align)"):
		w.Align = "heuristic"
	default:
		r.errorf("cmd/internal/obj/objfile.go: (*writer).Sym aligns symbols in an unknown way")
	}
	w.TrimInlineHash = strings.Contains(sym, "TrimInlineHash(")
	switch {
	case strings.Contains(sym, `strings.HasPrefix(s.Name, "type:")`) && strings.Contains(sym, `strings.HasPrefix(s.Name, "go:itab.")`):
		w.TypePrefix, w.ItabPrefix = "type:", "go:itab."
	case strings.Contains(sym, `strings.HasPrefix(s.Name, "type.")`) && strings.Contains(sym, `strings.HasPrefix(s.Name, "go.itab.")`): // before go1.20
		w.TypePrefix, w.ItabPrefix = "type.", "go.itab."
	default:
		r.errorf("cmd/internal/obj/objfile.go: (*writer).Sym marks type and itab symbols in an unknown way")
	}
	wof := r.funcSource("cmd/internal/obj/objfile.go", "WriteObjFile")
	w.SortRelocs = strings.Contains(wof, "sort.Sort(relocByOff(s.R))") || strings.Contains(wof, "slices.SortFunc(s.R, relocByOffCmp)")
	tfa := r.funcSource("cmd/internal/obj/sym.go", "(*Link).traverseFuncAux")
	w.InlFileSymRefs = regexp.MustCompile(`f, _ := (ctxt\.getFileSymbolAndLine\(|linkgetlineFromPos\(ctxt, )call\.Pos\)\s+if filesym := ctxt\.Lookup\(f\); filesym != nil \{\s+fn\(fsym, filesym\)`).MatchString(tfa)
	fiw := r.funcSource("cmd/internal/goobj/funcinfo.go", "(*FuncInfo).Write")
	w.FuncInfoStartLine = strings.Contains(fiw, "writeUint32(uint32(a.StartLine))")
	fp := r.funcSource("cmd/internal/obj/plist.go", "Flushplist")
	// Before go1.22 only functions written package-relative ("".f, ·f in
	// the source) got them; the converter's names are package-qualified,
	// as main·f would be.
	w.AsmArgsStackmap = strings.Contains(fp, `".args_stackmap"`) && strings.Contains(fp, `"%s.arginfo%d"`) &&
		!strings.Contains(fp, `if !strings.HasPrefix(s.Name, "\"\".") {`)
	return hashSrc
}

func (r *resolver) dwarfFacts(m *gometa.Meta) {
	d := &m.DWARF
	blocks := r.consts(r.parse("cmd/internal/dwarf/dwarf.go"), nil)
	if blk, ok := blockWith(blocks, "DW_ABRV_FUNCTION"); ok {
		d.AbbrevFunction = int(blk.values["DW_ABRV_FUNCTION"])
	} else {
		r.errorf("cmd/internal/dwarf/dwarf.go: no DW_ABRV_FUNCTION")
	}
	pdf := r.funcSource("cmd/internal/dwarf/dwarf.go", "PutDefaultFunc")
	if strings.Contains(pdf, "emitHiLoPc(") {
		d.FuncDIE = "v2"
		eh := r.funcSource("cmd/internal/dwarf/dwarf.go", "emitHiLoPc")
		if !strings.Contains(eh, "buildcfg.Experiment.Dwarf5") || !strings.Contains(eh, "DW_FORM_addrx") {
			r.errorf("cmd/internal/dwarf/dwarf.go: emitHiLoPc has an unknown layout")
		}
	} else {
		d.FuncDIE = "v1"
		if !strings.Contains(pdf, "int64(s.StartLine)") && !strings.Contains(pdf, "int64(s.StartPos.RelLine())") {
			d.FuncDIE = "v0" // before go1.21: no DW_AT_decl_line
		}
	}
	d.Lines = "v1" // generateDebugLinesSymbol, guarded by reviewed.go
	rt := r.parse("cmd/internal/objabi/reloctype.go")
	if body := r.funcSourceOpt(rt, "FuncCountToDwTxtAddrFlavor"); body != "" {
		re := regexp.MustCompile(`fncount <= (\d+):\s*return (R_DWTXTADDR_U\d), (\d+)`)
		for _, mm := range re.FindAllStringSubmatch(body, -1) {
			n, _ := strconv.Atoi(mm[1])
			sz, _ := strconv.Atoi(mm[3])
			d.TextAddrx = append(d.TextAddrx, gometa.AddrxFlavor{MaxFuncs: n, Reloc: mm[2], Size: sz})
		}
		if len(d.TextAddrx) == 0 {
			r.errorf("cmd/internal/objabi/reloctype.go: FuncCountToDwTxtAddrFlavor has an unknown layout")
		}
		sort.Slice(d.TextAddrx, func(i, j int) bool { return d.TextAddrx[i].MaxFuncs < d.TextAddrx[j].MaxFuncs })
	} else if d.FuncDIE == "v2" {
		r.errorf("emitHiLoPc without FuncCountToDwTxtAddrFlavor")
	}
}

func (r *resolver) exists(rel string) bool {
	_, err := os.Stat(filepath.Join(r.goroot, "src", filepath.FromSlash(rel)))
	return err == nil
}

// ssaGen is the SSA rule generator directory: ssa/_gen from go1.20,
// ssa/gen before.
func (r *resolver) ssaGen() string {
	if r.exists("cmd/compile/internal/ssa/_gen") {
		return "cmd/compile/internal/ssa/_gen"
	}
	return "cmd/compile/internal/ssa/gen"
}

// stackCopyAdjustsFP reads runtime.adjustframe: does it adjust the saved
// frame pointer on goarch?
func (r *resolver) stackCopyAdjustsFP(goarch string) bool {
	af := r.funcSource("runtime/stack.go", "adjustframe")
	m := regexp.MustCompile(`if (\(?goarch\.ArchFamily == goarch\.\w+(?: \|\| goarch\.ArchFamily == goarch\.\w+)*\)?) && frame\.argp-frame\.varp == 2\*goarch\.PtrSize \{`).FindStringSubmatch(af)
	if m == nil {
		r.errorf("runtime/stack.go: adjustframe adjusts frame pointers in an unknown way")
		return false
	}
	return strings.Contains(m[1], "goarch."+strings.ToUpper(goarch))
}
