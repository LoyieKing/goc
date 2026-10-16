package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"go/ast"
	"go/printer"
	"go/token"
	"sort"
	"strings"
)

// reviewed lists the Go functions whose behaviour gometa's writer
// (goobjw) reproduces, with the fingerprint of every version of them it
// was checked against (byte-identical output on goc's test corpus, see
// docs/multi-go-version.md). A fingerprint is the sha256 of the
// gofmt-printed function without comments. A function not listed under
// its fingerprint stops the resolver: read the Go change, update goobjw
// if needed, and add the fingerprint here.
var reviewed = map[string][]string{
	"cmd/internal/dwarf/dwarf.go:PutDefaultFunc": {
		"e92c18299980ba8a", // go1.23.12, 1.24.4
		"8f2538a74f69b790", // go1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/dwarf/dwarf.go:emitHiLoPc": {
		"absent",           // go1.23.12, 1.24.4
		"2b8707a04c7ad463", // go1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/dwarf/dwarf.go:putattr": {
		"7aebd17dac6af6f1", // go1.23.12, 1.24.4
		"8b3b103fcbc2f44f", // go1.25.14
		"fd85ba53eedd6c48", // go1.26.9, 1.27.2
	},
	"cmd/internal/goobj/builtin.go:BuiltinIdx": {
		"58d4b0a2f56e0cc0", // go1.23.12, 1.24.4, 1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/goobj/funcinfo.go:(*FuncInfo).Write": {
		"b440762e7e205eb6", // go1.23.12, 1.24.4, 1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/goobj/funcinfo.go:(*InlTreeNode).Write": {
		"68cac23fa1213a9c", // go1.23.12, 1.24.4, 1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/goobj/objfile.go:(*Header).Write": {
		"d1fd9249b3ca8436", // go1.23.12, 1.24.4, 1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/obj/data.go:(*LSym).WriteInt": {
		"a166d4a526b8722d", // go1.23.12, 1.24.4, 1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/obj/data.go:(*LSym).prepwrite": {
		"f069367d9dd8cee5", // go1.23.12
		"cbab201976abe907", // go1.24.4, 1.25.14, 1.26.9
		"01b851182c5f422d", // go1.27.2
	},
	"cmd/internal/obj/dwarf.go:(*Link).dwarfSym": {
		"ae6078385ae76e81", // go1.23.12
		"cc2d59bd03e46801", // go1.24.4, 1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/obj/dwarf.go:(*Link).generateDebugLinesSymbol": {
		"1027815d7e4a5156", // go1.23.12, 1.24.4, 1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/obj/dwarf.go:(*Link).populateDWARF": {
		"dd38fdc0e130b727", // go1.23.12
		"1655a9b69213177f", // go1.24.4
		"522c2231d62f52a6", // go1.25.14, 1.26.9
		"0c426da117d63b05", // go1.27.2
	},
	"cmd/internal/obj/dwarf.go:putpclcdelta": {
		"bc9134d68c1e9734", // go1.23.12, 1.24.4, 1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/obj/inl.go:(*InlTree).Add": {
		"596caed14cadf743", // go1.23.12, 1.24.4, 1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/obj/objfile.go:(*writer).Aux": {
		"b63079e50c40225c", // go1.23.12
		"c9fb792ceda90c6b", // go1.24.4, 1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/obj/objfile.go:(*writer).Reloc": {
		"c53d94bfc6e01f53", // go1.23.12, 1.24.4, 1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/obj/objfile.go:(*writer).StringTable": {
		"94b544d6a7fac60e", // go1.23.12, 1.24.4, 1.25.14, 1.26.9
		"d2697b267a4df73f", // go1.27.2
	},
	"cmd/internal/obj/objfile.go:(*writer).Sym": {
		"49065a0b45e90bb5", // go1.23.12
		"3c0496b34d236787", // go1.24.4, 1.25.14, 1.26.9
		"db5015071822e2e7", // go1.27.2
	},
	"cmd/internal/obj/objfile.go:(*writer).contentHash": {
		"fd81c0f514e31682", // go1.23.12
		"99419aed470e0e1b", // go1.24.4
		"24f78a60fc734388", // go1.25.14, 1.26.9
		"98e74650b3e78700", // go1.27.2
	},
	"cmd/internal/obj/objfile.go:(*writer).refFlags": {
		"f8e03e99b7f0ba01", // go1.23.12, 1.24.4, 1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/obj/objfile.go:(*writer).refNames": {
		"c767b9c8d3bde09b", // go1.23.12, 1.24.4, 1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/obj/objfile.go:WriteObjFile": {
		"13bc9a607624f819", // go1.23.12
		"bbc2da1e4e7e749a", // go1.24.4, 1.25.14
		"5a4ce6cd8f80b7e1", // go1.26.9, 1.27.2
	},
	"cmd/internal/obj/objfile.go:contentHashSection": {
		"50db27a180361e96", // go1.23.12, 1.24.4, 1.25.14, 1.26.9
		"c36f4d8611fe056e", // go1.27.2
	},
	"cmd/internal/obj/objfile.go:genFuncInfoSyms": {
		"2089b8013f303044", // go1.23.12
		"f1f5e061fa264035", // go1.24.4, 1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/obj/objfile.go:nAuxSym": {
		"fd8ff5ceb1f0b9eb", // go1.23.12
		"5a09acca9e99c240", // go1.24.4, 1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/obj/pcln.go:funcpctab": {
		"2cb19a654344fdf3", // go1.23.12, 1.24.4, 1.25.14
		"44eaacb7b2ec28c0", // go1.26.9
		"7b33fb063eb374ed", // go1.27.2
	},
	"cmd/internal/obj/pcln.go:linkpcln": {
		"1af38de964ab37d8", // go1.23.12, 1.24.4, 1.25.14
		"26089e13706b4c61", // go1.26.9
		"83c1745f5482f3b2", // go1.27.2
	},
	"cmd/internal/obj/pcln.go:pctofileline": {
		"0ad132823dc7171e", // go1.23.12, 1.24.4, 1.25.14
		"ce872ea72d8d2ce9", // go1.26.9, 1.27.2
	},
	"cmd/internal/obj/plist.go:(*Link).GloblPos": {
		"dcbeba8e74523901", // go1.23.12
		"8a92eb12c3c49dca", // go1.24.4, 1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/obj/plist.go:(*Link).InitTextSym": {
		"d70d2ef62454026c", // go1.23.12
		"e42f950bf3c51bb5", // go1.24.4, 1.25.14, 1.26.9
		"a10f18d36bf78c24", // go1.27.2
	},
	"cmd/internal/obj/plist.go:(*Link).toFuncFlag": {
		"95e8b9f874894a04", // go1.23.12, 1.24.4, 1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/obj/plist.go:Flushplist": {
		"fdbebcba861c04c3", // go1.23.12, 1.24.4, 1.25.14
		"0afd37d1ca83d6ce", // go1.26.9, 1.27.2
	},
	"cmd/internal/obj/sym.go:(*Link).NumberSyms": {
		"ce7f68bf48070eb3", // go1.23.12
		"510be6cae9bbe2af", // go1.24.4, 1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/obj/sym.go:(*Link).traverseFuncAux": {
		"4da0c381e7aaf833", // go1.23.12
		"4268ea82be0908fb", // go1.24.4, 1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/obj/sym.go:(*Link).traverseSyms": {
		"24d7f3a7217299cc", // go1.23.12
		"2908242a0b2a107c", // go1.24.4, 1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/obj/sym.go:isNonPkgSym": {
		"74ac0062a51f02b1", // go1.23.12, 1.24.4, 1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/obj/x86/obj6.go:preprocess": {
		"0ddbd315961cda4a", // go1.23.12, 1.24.4, 1.25.14
		"65dadba8e6fc31f7", // go1.26.9
		"724edc73a89883c6", // go1.27.2
	},
	"cmd/internal/objabi/funcid.go:GetFuncID": {
		"9fbcdbc347379cdf", // go1.23.12, 1.24.4, 1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/objabi/path.go:PathToPrefix": {
		"6e878e8cfc9fa518", // go1.23.12, 1.24.4, 1.25.14, 1.26.9, 1.27.2
	},
}

func reviewedKeys() []string {
	ks := make([]string, 0, len(reviewed))
	for k := range reviewed {
		ks = append(ks, k)
	}
	sort.Strings(ks)
	return ks
}

func (r *resolver) checkReviewed() {
	for _, k := range reviewedKeys() {
		fp := r.fingerprint(k)
		ok := false
		for _, want := range reviewed[k] {
			if strings.HasPrefix(want, fp) {
				ok = true
			}
		}
		if !ok {
			r.errorf("%s changed (fingerprint %s): review it against tools/gocobj2goobj/goobjw and "+
				"add the fingerprint to tools/golangc-metadata-resolver/reviewed.go", k, fp)
		}
	}
}

// fingerprint returns the fingerprint of "file:Func", or "absent".
// normalized is the gofmt-printed function without comments, the text
// a fingerprint is taken of ("absent" if there is no such function).
func (r *resolver) normalized(key string) string {
	file, fn, _ := strings.Cut(key, ":")
	sf := r.parse(file)
	d := findFunc(sf, fn)
	if d == nil {
		return "absent"
	}
	var b bytes.Buffer
	cp := *d
	cp.Doc = nil
	printer.Fprint(&b, token.NewFileSet(), stripComments(&cp))
	return b.String()
}

func (r *resolver) fingerprint(key string) string {
	n := r.normalized(key)
	if n == "absent" {
		return n
	}
	sum := sha256.Sum256([]byte(n))
	return hex.EncodeToString(sum[:])[:16]
}

func stripComments(d *ast.FuncDecl) *ast.FuncDecl { return d }

func findFunc(sf *srcFile, name string) *ast.FuncDecl {
	for _, d := range sf.file.Decls {
		fd, ok := d.(*ast.FuncDecl)
		if !ok {
			continue
		}
		if funcName(fd) == name {
			return fd
		}
	}
	return nil
}

func funcName(fd *ast.FuncDecl) string {
	if fd.Recv == nil || len(fd.Recv.List) == 0 {
		return fd.Name.Name
	}
	var recv string
	switch t := fd.Recv.List[0].Type.(type) {
	case *ast.StarExpr:
		if id, ok := t.X.(*ast.Ident); ok {
			recv = "(*" + id.Name + ")"
		}
	case *ast.Ident:
		recv = "(" + t.Name + ")"
	}
	return recv + "." + fd.Name.Name
}

// funcSource returns the printed source of a function, which must exist.
func (r *resolver) funcSource(file, name string) string {
	s := r.funcSourceOpt(r.parse(file), name)
	if s == "" {
		r.errorf("%s: no function %s", file, name)
	}
	return s
}

func (r *resolver) funcSourceOpt(sf *srcFile, name string) string {
	d := findFunc(sf, name)
	if d == nil {
		return ""
	}
	return string(sf.text[sf.fset.Position(d.Pos()).Offset:sf.fset.Position(d.End()).Offset])
}
