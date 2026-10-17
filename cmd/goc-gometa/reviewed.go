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
// (internal/goobj) reproduces, with the fingerprint of every version of them it
// was checked against (byte-identical output on goc's test corpus, see
// docs/multi-go-version.md). A fingerprint is the sha256 of the
// gofmt-printed function without comments. A function not listed under
// its fingerprint stops the resolver: read the Go change, update internal/goobj
// if needed, and add the fingerprint here.
var reviewed = map[string][]string{
	"cmd/internal/dwarf/dwarf.go:PutDefaultFunc": {
		"9ff25b2a3745c4aa", // go1.21.13
		"8357a4dfd11ce537", // go1.18.10, 1.19.13, 1.20.14
		"e92c18299980ba8a", // go1.22.12, 1.23.12, 1.24.4
		"8f2538a74f69b790", // go1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/dwarf/dwarf.go:emitHiLoPc": {
		"absent",           // go1.18.10, 1.19.13, 1.20.14, 1.21.13, 1.22.12, 1.23.12, 1.24.4
		"2b8707a04c7ad463", // go1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/dwarf/dwarf.go:putattr": {
		"7aebd17dac6af6f1", // go1.18.10, 1.19.13, 1.20.14, 1.21.13, 1.22.12, 1.23.12, 1.24.4
		"8b3b103fcbc2f44f", // go1.25.14
		"fd85ba53eedd6c48", // go1.26.9, 1.27.2
	},
	"cmd/internal/goobj/builtin.go:BuiltinIdx": {
		"58d4b0a2f56e0cc0", // go1.18.10, 1.19.13, 1.20.14, 1.21.13, 1.22.12, 1.23.12, 1.24.4, 1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/goobj/funcinfo.go:(*FuncInfo).Write": {
		"6f4cc98e3b8db160", // go1.18.10, 1.19.13
		"b440762e7e205eb6", // go1.20.14, 1.21.13, 1.22.12, 1.23.12, 1.24.4, 1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/goobj/funcinfo.go:(*InlTreeNode).Write": {
		"68cac23fa1213a9c", // go1.18.10, 1.19.13, 1.20.14, 1.21.13, 1.22.12, 1.23.12, 1.24.4, 1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/goobj/objfile.go:(*Header).Write": {
		"d1fd9249b3ca8436", // go1.18.10, 1.19.13, 1.20.14, 1.21.13, 1.22.12, 1.23.12, 1.24.4, 1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/obj/data.go:(*LSym).WriteInt": {
		"a166d4a526b8722d", // go1.18.10, 1.19.13, 1.20.14, 1.21.13, 1.22.12, 1.23.12, 1.24.4, 1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/obj/data.go:(*LSym).prepwrite": {
		"f069367d9dd8cee5", // go1.18.10, 1.19.13, 1.20.14, 1.21.13, 1.22.12, 1.23.12
		"cbab201976abe907", // go1.24.4, 1.25.14, 1.26.9
		"01b851182c5f422d", // go1.27.2
	},
	"cmd/internal/obj/dwarf.go:(*Link).dwarfSym": {
		"ae6078385ae76e81", // go1.18.10, 1.19.13, 1.20.14, 1.21.13, 1.22.12, 1.23.12
		"cc2d59bd03e46801", // go1.24.4, 1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/obj/dwarf.go:(*Link).generateDebugLinesSymbol": {
		"89f75e125125132d", // go1.20.14, 1.21.13
		"0140a9a79782b3d4", // go1.18.10, 1.19.13
		"1027815d7e4a5156", // go1.22.12, 1.23.12, 1.24.4, 1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/obj/dwarf.go:(*Link).populateDWARF": {
		"50b9f94701be28e1", // go1.21.13
		"00ffceb183ee3ebd", // go1.18.10, 1.19.13, 1.20.14
		"dd38fdc0e130b727", // go1.22.12, 1.23.12
		"1655a9b69213177f", // go1.24.4
		"522c2231d62f52a6", // go1.25.14, 1.26.9
		"0c426da117d63b05", // go1.27.2
	},
	"cmd/internal/obj/dwarf.go:putpclcdelta": {
		"bc9134d68c1e9734", // go1.18.10, 1.19.13, 1.20.14, 1.21.13, 1.22.12, 1.23.12, 1.24.4, 1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/obj/inl.go:(*InlTree).Add": {
		"584d3bc4994eddf1", // go1.18.10, 1.19.13, 1.20.14
		"596caed14cadf743", // go1.21.13, 1.22.12, 1.23.12, 1.24.4, 1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/obj/objfile.go:(*writer).Aux": {
		"8bf1f3b15259d6e2", // go1.18.10, 1.19.13, 1.20.14
		"b63079e50c40225c", // go1.21.13, 1.22.12, 1.23.12
		"c9fb792ceda90c6b", // go1.24.4, 1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/obj/objfile.go:(*writer).Reloc": {
		"9a1e15c25b8d1a62", // go1.18.10, 1.19.13
		"c53d94bfc6e01f53", // go1.20.14, 1.21.13, 1.22.12, 1.23.12, 1.24.4, 1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/obj/objfile.go:(*writer).StringTable": {
		"38e0544ac0fcf86b", // go1.19.13, 1.20.14, 1.21.13
		"51b1020068324e75", // go1.18.10
		"94b544d6a7fac60e", // go1.22.12, 1.23.12, 1.24.4, 1.25.14, 1.26.9
		"d2697b267a4df73f", // go1.27.2
	},
	"cmd/internal/obj/objfile.go:(*writer).Sym": {
		"9ab6c4a1abf002bb", // go1.21.13, 1.22.12
		"44a0227202718bde", // go1.20.14
		"296233e3acb87fa8", // go1.18.10, 1.19.13
		"49065a0b45e90bb5", // go1.23.12
		"3c0496b34d236787", // go1.24.4, 1.25.14, 1.26.9
		"db5015071822e2e7", // go1.27.2
	},
	"cmd/internal/obj/objfile.go:(*writer).contentHash": {
		"e26ccf381e5cd8a2", // go1.18.10
		"fd81c0f514e31682", // go1.19.13, 1.20.14, 1.21.13, 1.22.12, 1.23.12
		"99419aed470e0e1b", // go1.24.4
		"24f78a60fc734388", // go1.25.14, 1.26.9
		"98e74650b3e78700", // go1.27.2
	},
	"cmd/internal/obj/objfile.go:(*writer).refFlags": {
		"2c0cae2640bbd14f", // go1.18.10, 1.19.13
		"f8e03e99b7f0ba01", // go1.20.14, 1.21.13, 1.22.12, 1.23.12, 1.24.4, 1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/obj/objfile.go:(*writer).refNames": {
		"d73101f217b9f104", // go1.19.13
		"09dd48bb1938962e", // go1.18.10
		"c767b9c8d3bde09b", // go1.20.14, 1.21.13, 1.22.12, 1.23.12, 1.24.4, 1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/obj/objfile.go:WriteObjFile": {
		"54c7395c51128105", // go1.19.13, 1.20.14, 1.21.13, 1.22.12
		"684754b65b90f7c5", // go1.18.10
		"13bc9a607624f819", // go1.23.12
		"bbc2da1e4e7e749a", // go1.24.4, 1.25.14
		"5a4ce6cd8f80b7e1", // go1.26.9, 1.27.2
	},
	"cmd/internal/obj/objfile.go:contentHashSection": {
		"4413bab01de94781", // go1.18.10, 1.19.13
		"50db27a180361e96", // go1.20.14, 1.21.13, 1.22.12, 1.23.12, 1.24.4, 1.25.14, 1.26.9
		"c36f4d8611fe056e", // go1.27.2
	},
	"cmd/internal/obj/objfile.go:genFuncInfoSyms": {
		"52723b682a80df7c", // go1.21.13, 1.22.12
		"64f10ee2e809781a", // go1.20.14
		"7dba2023ef14811b", // go1.19.13
		"cc603850991efefa", // go1.18.10
		"2089b8013f303044", // go1.23.12
		"f1f5e061fa264035", // go1.24.4, 1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/obj/objfile.go:nAuxSym": {
		"790ca377e0438e09", // go1.18.10, 1.19.13, 1.20.14
		"fd8ff5ceb1f0b9eb", // go1.21.13, 1.22.12, 1.23.12
		"5a09acca9e99c240", // go1.24.4, 1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/obj/pcln.go:funcpctab": {
		"2cb19a654344fdf3", // go1.18.10, 1.19.13, 1.20.14, 1.21.13, 1.22.12, 1.23.12, 1.24.4, 1.25.14
		"44eaacb7b2ec28c0", // go1.26.9
		"7b33fb063eb374ed", // go1.27.2
	},
	"cmd/internal/obj/pcln.go:linkpcln": {
		"01edf60bf2167b03", // go1.18.10, 1.19.13
		"1af38de964ab37d8", // go1.20.14, 1.21.13, 1.22.12, 1.23.12, 1.24.4, 1.25.14
		"26089e13706b4c61", // go1.26.9
		"83c1745f5482f3b2", // go1.27.2
	},
	"cmd/internal/obj/pcln.go:pctofileline": {
		"42afe61e68f53892", // go1.18.10, 1.19.13
		"0ad132823dc7171e", // go1.20.14, 1.21.13, 1.22.12, 1.23.12, 1.24.4, 1.25.14
		"ce872ea72d8d2ce9", // go1.26.9, 1.27.2
	},
	"cmd/internal/obj/plist.go:(*Link).GloblPos": {
		"absent",           // go1.18.10, 1.19.13
		"dcbeba8e74523901", // go1.20.14, 1.21.13, 1.22.12, 1.23.12
		"8a92eb12c3c49dca", // go1.24.4, 1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/obj/plist.go:(*Link).InitTextSym": {
		"6c9fb7c952c170f8", // go1.21.13
		"d813c642bc45272c", // go1.20.14
		"e8309c6b15432a29", // go1.18.10, 1.19.13
		"d70d2ef62454026c", // go1.22.12, 1.23.12
		"e42f950bf3c51bb5", // go1.24.4, 1.25.14, 1.26.9
		"a10f18d36bf78c24", // go1.27.2
	},
	"cmd/internal/obj/plist.go:(*Link).toFuncFlag": {
		"9ea901a7b070464c", // go1.18.10, 1.19.13, 1.20.14
		"95e8b9f874894a04", // go1.21.13, 1.22.12, 1.23.12, 1.24.4, 1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/obj/plist.go:Flushplist": {
		"6f701be2eb042886", // go1.21.13
		"69933b6a38c2ac3e", // go1.20.14
		"11c86e3fd347a15e", // go1.18.10, 1.19.13
		"fdbebcba861c04c3", // go1.22.12, 1.23.12, 1.24.4, 1.25.14
		"0afd37d1ca83d6ce", // go1.26.9, 1.27.2
	},
	"cmd/internal/obj/sym.go:(*Link).NumberSyms": {
		"6ef2596a0d149354", // go1.22.12
		"9b1a997dead5a8b8", // go1.21.13
		"cc6a01f648213526", // go1.18.10, 1.19.13, 1.20.14
		"ce7f68bf48070eb3", // go1.23.12
		"510be6cae9bbe2af", // go1.24.4, 1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/obj/sym.go:(*Link).traverseFuncAux": {
		"1eebfe3b41d09318", // go1.21.13
		"fd91a8fdafdf60a7", // go1.20.14
		"9a01f82f4120ff6e", // go1.18.10, 1.19.13
		"4da0c381e7aaf833", // go1.22.12, 1.23.12
		"4268ea82be0908fb", // go1.24.4, 1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/obj/sym.go:(*Link).traverseSyms": {
		"0c4a225e7568eb81", // go1.20.14
		"e52b30a5a844fb31", // go1.18.10, 1.19.13
		"24d7f3a7217299cc", // go1.21.13, 1.22.12, 1.23.12
		"2908242a0b2a107c", // go1.24.4, 1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/obj/sym.go:isNonPkgSym": {
		"74ac0062a51f02b1", // go1.18.10, 1.19.13, 1.20.14, 1.21.13, 1.22.12, 1.23.12, 1.24.4, 1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/obj/x86/obj6.go:preprocess": {
		"dfc57152b2dd8bf2", // go1.18.10, 1.19.13, 1.20.14
		"0ddbd315961cda4a", // go1.21.13, 1.22.12, 1.23.12, 1.24.4, 1.25.14
		"65dadba8e6fc31f7", // go1.26.9
		"724edc73a89883c6", // go1.27.2
	},
	"cmd/internal/objabi/funcid.go:GetFuncID": {
		"839d36747662ae89", // go1.18.10, 1.19.13, 1.20.14
		"9fbcdbc347379cdf", // go1.21.13, 1.22.12, 1.23.12, 1.24.4, 1.25.14, 1.26.9, 1.27.2
	},
	"cmd/internal/objabi/path.go:PathToPrefix": {
		"6e878e8cfc9fa518", // go1.18.10, 1.19.13, 1.20.14, 1.21.13, 1.22.12, 1.23.12, 1.24.4, 1.25.14, 1.26.9, 1.27.2
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
			r.errorf("%s changed (fingerprint %s): review it against internal/goobj and "+
				"add the fingerprint to cmd/goc-gometa/reviewed.go", k, fp)
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
