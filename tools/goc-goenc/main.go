// Command goc-goenc copies the Go object writer (cmd/internal/obj and what it
// needs) out of a GOROOT into a standalone module, so gocobj2goobj can be
// built against the exact object format, relocation numbering and funcdata
// indices of the Go release that will link the result.
//
//	goc-goenc -goroot $(go env GOROOT) -out build/goenc/go1.27.2
//
// The package set is the import closure of cmd/internal/obj/x86 restricted
// to cmd/internal/* and internal/*, as the GOROOT's own `go list` reports
// it. Paths map cmd/internal/X -> cmd/objlib/X and internal/X ->
// stdinternal/X under the module github.com/LoyieKing/goc/goobj/enc. Only
// import specs are rewritten (go/ast), never string literals. Two goc files
// are added: internal/abi's FuncPC* stubs (compiler intrinsics outside
// GOROOT) and obj.InlTree.SetParentPC.
package main

import (
	"bytes"
	"encoding/json"
	"flag"
	"fmt"
	"go/ast"
	"go/format"
	"go/parser"
	"go/token"
	"log"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"

	"github.com/LoyieKing/goc/gocobj"
)

const module = "github.com/LoyieKing/goc/goobj/enc"

// roots are the packages gocobj2goobj imports.
var roots = []string{"cmd/internal/obj/x86", "cmd/internal/obj", "cmd/internal/goobj",
	"cmd/internal/objabi", "cmd/internal/bio", "cmd/internal/src", "cmd/internal/sys",
	"internal/abi"}

type pkg struct {
	ImportPath string
	Dir        string
	Imports    []string
}

func mapped(ip string) (string, bool) {
	if r, ok := strings.CutPrefix(ip, "cmd/internal/"); ok {
		return "cmd/objlib/" + r, true
	}
	if r, ok := strings.CutPrefix(ip, "internal/"); ok {
		return "stdinternal/" + r, true
	}
	return "", false
}

func main() {
	goroot := flag.String("goroot", "", "GOROOT to copy from (default: go env GOROOT)")
	out := flag.String("out", "", "output module directory (replaced)")
	lang := flag.String("lang", "", "go directive of the copy's go.mod (default: the release's major.minor)")
	flag.Parse()
	if *out == "" {
		log.Fatal("goc-goenc: -out is required")
	}
	gobin := "go"
	if *goroot != "" {
		gobin = filepath.Join(*goroot, "bin", "go")
	}
	// GOTOOLCHAIN=local, outside any module: the go command must describe
	// itself, not switch to the toolchain a go.mod asks for.
	ver := strings.TrimSpace(run(gobin, "env", "GOVERSION"))
	gr := strings.TrimSpace(run(gobin, "env", "GOROOT"))

	// The closure of internal/cmd-internal packages, via the GOROOT's go list.
	args := append([]string{"list", "-deps", "-json=ImportPath,Dir,Imports"}, roots...)
	cmd := exec.Command(gobin, args...)
	cmd.Dir = filepath.Join(gr, "src", "cmd")
	cmd.Env = append(os.Environ(), "GOOS=linux", "GOARCH=amd64", "GOFLAGS=", "GOWORK=off", "GOTOOLCHAIN=local")
	raw, err := cmd.Output()
	if err != nil {
		log.Fatalf("goc-goenc: go list: %v", err)
	}
	all := map[string]*pkg{}
	dec := json.NewDecoder(bytes.NewReader(raw))
	for dec.More() {
		p := &pkg{}
		if err := dec.Decode(p); err != nil {
			log.Fatal(err)
		}
		all[p.ImportPath] = p
	}
	keep := map[string]bool{}
	var walk func(string)
	walk = func(ip string) {
		if keep[ip] {
			return
		}
		if _, ok := mapped(ip); !ok {
			return
		}
		p := all[ip]
		if p == nil {
			log.Fatalf("goc-goenc: %s not in go list output", ip)
		}
		keep[ip] = true
		for _, d := range p.Imports {
			walk(d)
		}
	}
	for _, r := range roots {
		walk(r)
	}

	if err := os.RemoveAll(*out); err != nil {
		log.Fatal(err)
	}
	var names []string
	for ip := range keep {
		names = append(names, ip)
	}
	sort.Strings(names)
	for _, ip := range names {
		copyPkg(all[ip], keep, *out)
	}
	write(filepath.Join(*out, "stdinternal/abi/funcpc_stub.go"), funcpcStub)
	write(filepath.Join(*out, "cmd/objlib/obj/goc_inl.go"), gocInl)
	// LSym.AddRel exists from go1.24 (it checks FIPS relocations); before,
	// relocations were appended to LSym.R directly.
	addRel := "s.R = append(s.R, r)"
	if has(all["cmd/internal/obj"].Dir, "func (s *LSym) AddRel(") {
		addRel = "s.AddRel(ctxt, r)"
	}
	// LSym.Align (go1.27+): content-addressable data must set it.
	setAlign := ""
	if m := regexp.MustCompile(`(?s)type LSym struct \{.*?\n\tAlign\s+(\w+)`).FindStringSubmatch(
		readFile(filepath.Join(all["cmd/internal/obj"].Dir, "link.go"))); m != nil && !strings.Contains(m[0], "\n}") {
		setAlign = "s.Align = " + m[1] + "(a)"
	}
	compat, err := format.Source([]byte(fmt.Sprintf(gocCompat, addRel, setAlign)))
	if err != nil {
		log.Fatal(err)
	}
	write(filepath.Join(*out, "cmd/objlib/obj/goc_compat.go"), string(compat))
	write(filepath.Join(*out, "cmd/objlib/objabi/goc_profile.go"), profileFile(gr, ver))
	write(filepath.Join(*out, "go.mod"), fmt.Sprintf("module %s\n\ngo %s\n", module, pick(*lang, langVersion(ver))))
	write(filepath.Join(*out, "GOVERSION"), ver+"\n")
	fmt.Printf("goc-goenc: %s -> %s (%d packages)\n", ver, *out, len(names))
}

func pick(a, b string) string {
	if a != "" {
		return a
	}
	return b
}

// langVersion is the go directive for the copied module: the release's
// major.minor, so the copy may use what that release's language allows.
func langVersion(v string) string {
	v = strings.TrimPrefix(v, "go")
	parts := strings.SplitN(v, ".", 3)
	if len(parts) < 2 {
		return v
	}
	n := 0
	for n < len(parts[1]) && parts[1][n] >= '0' && parts[1][n] <= '9' {
		n++
	}
	return parts[0] + "." + parts[1][:n]
}

func copyPkg(p *pkg, keep map[string]bool, out string) {
	rel, _ := mapped(p.ImportPath)
	dst := filepath.Join(out, rel)
	ents, err := os.ReadDir(p.Dir)
	if err != nil {
		log.Fatal(err)
	}
	for _, e := range ents {
		n := e.Name()
		if e.IsDir() || strings.HasSuffix(n, "_test.go") {
			continue
		}
		// Assembly (cmd/internal/notsha256 before go1.24) is copied as is.
		// internal/abi's assembly only defines test helpers (FuncPCTestFn),
		// which would clash with the real internal/abi at link time.
		if (strings.HasSuffix(n, ".s") || strings.HasSuffix(n, ".h")) && p.ImportPath == "internal/abi" {
			continue
		}
		if strings.HasSuffix(n, ".s") || strings.HasSuffix(n, ".h") {
			src, err := os.ReadFile(filepath.Join(p.Dir, n))
			if err != nil {
				log.Fatal(err)
			}
			write(filepath.Join(dst, n), string(src))
			continue
		}
		if !strings.HasSuffix(n, ".go") {
			continue
		}
		// internal/abi's FuncPCABI0/FuncPCABIInternal are compiler
		// intrinsics, declared without a body; funcpc_stub.go replaces them.
		if p.ImportPath == "internal/abi" && n == "funcpc.go" {
			continue
		}
		src, err := os.ReadFile(filepath.Join(p.Dir, n))
		if err != nil {
			log.Fatal(err)
		}
		write(filepath.Join(dst, n), rewrite(filepath.Join(p.Dir, n), src, keep))
	}
}

func rewrite(name string, src []byte, keep map[string]bool) string {
	fset := token.NewFileSet()
	f, err := parser.ParseFile(fset, name, src, parser.ParseComments)
	if err != nil {
		log.Fatal(err)
	}
	changed := false
	for _, im := range f.Imports {
		ip, _ := strconv.Unquote(im.Path.Value)
		if !keep[ip] {
			if _, ok := mapped(ip); ok {
				log.Fatalf("goc-goenc: %s imports %s, which is outside the copied set", name, ip)
			}
			continue
		}
		rel, _ := mapped(ip)
		im.Path.Value = strconv.Quote(module + "/" + rel)
		changed = true
	}
	if !changed {
		return string(src)
	}
	var b bytes.Buffer
	if err := format.Node(&b, fset, f); err != nil {
		log.Fatal(err)
	}
	_ = ast.File{}
	return b.String()
}

// profileFile probes the ABI facts goc depends on from the GOROOT's own
// sources and names the gocobj profile that states them, per GOARCH.
func profileFile(goroot, ver string) string {
	var b strings.Builder
	fmt.Fprintf(&b, "// Code generated by tools/goc-goenc from %s; DO NOT EDIT.\n\npackage objabi\n\n", ver)
	b.WriteString("// GocProfiles is the goc ABI profile (gocobj/abi.go) whose facts this Go\n// release has, per GOARCH.\nvar GocProfiles = map[string]string{\n")
	for _, arch := range []string{"amd64", "arm64"} {
		got := probe(goroot, arch)
		name := ""
		for _, p := range gocobj.Profiles() {
			if gocobj.SameFacts(p, got) && (name == "" || strings.HasPrefix(p.Name, "goabi")) {
				name = p.Name
			}
		}
		if name == "" {
			log.Fatalf("goc-goenc: %s %s has ABI facts no goc profile states: %+v\n"+
				"add a profile to llvm/include/Goc/AbiProfiles.def and gocobj/abi.go (docs/multi-go-version.md)", ver, arch, got)
		}
		fmt.Fprintf(&b, "\t%q: %q,\n", arch, name)
	}
	b.WriteString("}\n")
	return b.String()
}

// probe reads one GOARCH's facts from the Go sources that define them
// (docs/go-version-diffs.md sections 4 and 5).
func probe(goroot, arch string) gocobj.Profile {
	src := func(rel string) string {
		b, err := os.ReadFile(filepath.Join(goroot, "src", rel))
		if err != nil {
			log.Fatalf("goc-goenc: probe: %v", err)
		}
		return string(b)
	}
	find := func(text, re, what string) string {
		m := regexp.MustCompile(re).FindStringSubmatch(text)
		if m == nil {
			log.Fatalf("goc-goenc: probe %s: %s not found", arch, what)
		}
		return m[1]
	}
	atoi := func(s string) int { n, _ := strconv.Atoi(s); return n }
	p := gocobj.Profile{Arch: arch, PtrSize: 8}
	stack := src("internal/abi/stack.go")
	p.StackSmall = atoi(find(stack, `StackSmall\s*=\s*(\d+)`, "StackSmall"))
	p.StackBig = atoi(find(stack, `StackBig\s*=\s*(\d+)`, "StackBig"))
	// g starts with stack (lo, hi uintptr) and then stackguard0.
	rt := src("runtime/runtime2.go")
	if regexp.MustCompile(`type stack struct \{\s*lo\s+uintptr\s*hi\s+uintptr\s*\}`).MatchString(rt) &&
		regexp.MustCompile(`type g struct \{(?:\s*//[^\n]*)*\s*stack\s+stack[^\n]*\n\s*stackguard0\s+uintptr`).MatchString(rt) {
		p.StackGuard0Off = 2 * p.PtrSize
	}
	ops := src("cmd/compile/internal/ssa/_gen/" + strings.ToUpper(arch) + "Ops.go")
	p.GReg = find(ops, `"g",\s*//\s*a\.?k\.?a\.?\s*(R\d+)`, "g register")
	ints := strings.Fields(find(ops, `ParamIntRegNames:\s*"([^"]*)"`, "ParamIntRegNames"))
	if arch == "amd64" {
		for i, r := range ints {
			if !strings.HasPrefix(r, "R") {
				ints[i] = "R" + r // AX -> RAX, as the profile spells them
			}
		}
	}
	p.IntRegs = ints
	p.FPRegs = strings.Fields(find(ops, `ParamFloatRegNames:\s*"([^"]*)"`, "ParamFloatRegNames"))
	if strings.Contains(src("runtime/asm_"+arch+".s"), "TEXT runtime·morestack_noctxt(") {
		p.Morestack = "runtime.morestack_noctxt"
	}
	return p
}

func readFile(path string) string {
	b, err := os.ReadFile(path)
	if err != nil {
		log.Fatal(err)
	}
	return string(b)
}

// has reports whether any .go file of dir contains needle.
func has(dir, needle string) bool {
	m, _ := filepath.Glob(filepath.Join(dir, "*.go"))
	for _, f := range m {
		b, err := os.ReadFile(f)
		if err == nil && bytes.Contains(b, []byte(needle)) {
			return true
		}
	}
	return false
}

func write(path, s string) {
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		log.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(s), 0o644); err != nil {
		log.Fatal(err)
	}
}

func run(name string, args ...string) string {
	cmd := exec.Command(name, args...)
	cmd.Dir = os.TempDir()
	cmd.Env = append(os.Environ(), "GOTOOLCHAIN=local", "GOFLAGS=", "GOWORK=off")
	b, err := cmd.Output()
	if err != nil {
		log.Fatalf("goc-goenc: %s %v: %v", name, args, err)
	}
	return string(b)
}

const funcpcStub = `// goc addition, not part of the Go tree (tools/goc-goenc).

package abi

// FuncPCABI0 and FuncPCABIInternal are compiler intrinsics inside GOROOT.
// The object writer never calls them; these stubs only let the copy build.

func FuncPCABI0(f interface{}) uintptr { return 0 }

func FuncPCABIInternal(f interface{}) uintptr { return 0 }
`

const gocInl = `// goc addition, not part of the Go tree (tools/goc-goenc):
// gocobj2goobj builds a function's inline tree from the .goc.inltree table
// instead of from Progs, so it sets ParentPC directly.

package obj

// SetParentPC sets the ParentPC of node i of a per-function inline tree.
func (tree *InlTree) SetParentPC(i int, pc int32) { tree.setParentPC(i, pc) }
`

const gocCompat = `// goc addition, not part of the Go tree (tools/goc-goenc): the obj API
// gocobj2goobj uses that differs between Go releases.

package obj

// GocAddRel appends relocation r to s.
func (s *LSym) GocAddRel(ctxt *Link, r Reloc) { %s }

// GocSetAlign sets a data symbol's alignment where LSym has one.
func (s *LSym) GocSetAlign(a int) { %s }
`
