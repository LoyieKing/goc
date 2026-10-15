package main

import (
	"debug/elf"
	"encoding/binary"
	"fmt"
	"os"
	"path"
	"regexp"
	"sort"
	"strings"

	"github.com/LoyieKing/goc/gocobj"
	"github.com/LoyieKing/goc/goobj/enc/cmd/objlib/obj"
	"github.com/LoyieKing/goc/goobj/enc/cmd/objlib/objabi"
	"github.com/LoyieKing/goc/goobj/enc/cmd/objlib/src"
)

// rela is one ELF RELA entry.
type rela struct {
	Off  uint64
	Type uint32
	Sym  uint32
	Add  int64
}

// fn is one function of the object: its table record, where its bytes are
// in the ELF and its Go symbol.
type fn struct {
	tab  *gocobj.Func
	sec  int    // ELF section index
	off  uint64 // offset in the section
	name string // ELF name
	sym  *obj.LSym
	abi  obj.ABI
}

type converter struct {
	path, pkg, tu string
	arm64         bool
	f             *gocobj.File
	ef            *elf.File
	syms          []elf.Symbol
	ctxt          *obj.Link
	fns           []*fn
	byName        map[string]*fn
	bySec         map[int][]*fn // sorted by offset
	abiByGoSym    map[string]obj.ABI
	goName        map[string]string // ELF name -> Go name
	dataSym       map[int]*obj.LSym // ELF symbol index -> per-name data symbol
	secSym        map[int]*obj.LSym // ELF section index -> section copy
	gotSyms       map[string]*obj.LSym
}

var (
	tuCharRE   = regexp.MustCompile(`[^A-Za-z0-9_]`)
	tuSuffixRE = regexp.MustCompile(`\.(c|goc|ll|bc)$`)
)

// tuTag is the translation-unit tag that qualifies internal C names and the
// section copies: the source basename with every character outside
// [A-Za-z0-9_] replaced by '_'.
func tuTag(f *gocobj.File) string {
	if len(f.Files) == 0 || f.Files[0] == "" {
		return "tu"
	}
	return tuSuffixRE.ReplaceAllString(tuCharRE.ReplaceAllString(path.Base(f.Files[0]), "_"), "")
}

// libcallImpl: LLVM may synthesize a libcall after the IR-level Go ABI
// rename, so that reference is bare even if the defining TU emitted
// <name>.impl. The QJS build supplies the eligible names from its shim
// object; without that explicit contract keep the core integer libcalls.
var libcallImpl = func() map[string]bool {
	if names := os.Getenv("GOC_LIBCALL_IMPL"); names != "" {
		out := map[string]bool{}
		for _, name := range strings.Fields(names) {
			out[name] = true
		}
		return out
	}
	return map[string]bool{
		"memcpy": true, "memmove": true, "memset": true, "memcmp": true,
		"bcmp": true, "strlen": true, "strcpy": true, "strcmp": true,
		"vsnprintf": true, "snprintf": true,
	}
}()

func newConverter(p string, f *gocobj.File, pkg, goarch string) (*converter, error) {
	want := map[string]elf.Machine{"amd64": elf.EM_X86_64, "arm64": elf.EM_AARCH64}[goarch]
	if want == 0 {
		return nil, fmt.Errorf("GOARCH=%s: only amd64 and arm64 objects are converted", goarch)
	}
	ef, err := elf.Open(p)
	if err != nil {
		return nil, err
	}
	if ef.Machine != want {
		return nil, fmt.Errorf("%s: ELF machine %v, want %v for GOARCH=%s", p, ef.Machine, want, goarch)
	}
	syms, err := ef.Symbols()
	if err != nil {
		return nil, err
	}
	c := &converter{path: p, arm64: goarch == "arm64", pkg: pkg, tu: tuTag(f), f: f, ef: ef, syms: syms,
		byName: map[string]*fn{}, bySec: map[int][]*fn{}, abiByGoSym: map[string]obj.ABI{},
		goName: map[string]string{}, dataSym: map[int]*obj.LSym{}, secSym: map[int]*obj.LSym{},
		gotSyms: map[string]*obj.LSym{}}
	elfFn := map[string]elf.Symbol{}
	for _, s := range syms {
		if elf.ST_TYPE(s.Info) == elf.STT_FUNC && s.Section != elf.SHN_UNDEF && int(s.Section) < len(ef.Sections) {
			elfFn[s.Name] = s
		}
	}
	for i := range f.Funcs {
		t := &f.Funcs[i]
		s, ok := elfFn[t.Name]
		if !ok {
			return nil, fmt.Errorf("%s: no ELF function symbol", t.Name)
		}
		if t.TextSize > s.Size {
			return nil, fmt.Errorf("%s: table size %d exceeds ELF size %d", t.Name, t.TextSize, s.Size)
		}
		x := &fn{tab: t, sec: int(s.Section), off: s.Value, name: t.Name, abi: obj.ABIInternal}
		gn := pkg + "." + t.Name
		if t.Kind == 0 {
			if t.Flags&gocobj.FuncInternal != 0 {
				gn = pkg + "." + c.tu + "." + t.Name
			}
			if t.ABI == 0 {
				x.abi = obj.ABI0
			}
		}
		c.goName[t.Name] = gn
		c.abiByGoSym[gn] = x.abi
		c.fns = append(c.fns, x)
		c.byName[t.Name] = x
		c.bySec[x.sec] = append(c.bySec[x.sec], x)
	}
	for _, l := range c.bySec {
		sort.Slice(l, func(i, j int) bool { return l[i].off < l[j].off })
	}
	return c, nil
}

func (c *converter) symabis() string {
	var b strings.Builder
	for _, x := range c.fns {
		a := "ABIInternal"
		if x.abi == obj.ABI0 {
			a = "ABI0"
		}
		fmt.Fprintf(&b, "def %s %s\n", c.goName[x.name], a)
	}
	return b.String()
}

func (c *converter) relas(sec int) ([]rela, error) {
	rs := c.ef.Section(".rela" + c.ef.Sections[sec].Name)
	if rs == nil {
		return nil, nil
	}
	d, err := rs.Data()
	if err != nil {
		return nil, err
	}
	out := make([]rela, 0, len(d)/24)
	for i := 0; i+24 <= len(d); i += 24 {
		info := binary.LittleEndian.Uint64(d[i+8:])
		out = append(out, rela{Off: binary.LittleEndian.Uint64(d[i:]), Type: uint32(info), Sym: uint32(info >> 32),
			Add: int64(binary.LittleEndian.Uint64(d[i+16:]))})
	}
	return out, nil
}

func (c *converter) isText(sec int) bool {
	return sec > 0 && sec < len(c.ef.Sections) && c.ef.Sections[sec].Flags&elf.SHF_EXECINSTR != 0
}

// resolveText maps an address in a text section to the enclosing function
// and the offset in it. An address in the padding after a function resolves
// to the next function with a negative offset.
func (c *converter) resolveText(sec int, addr uint64) (*obj.LSym, int64) {
	l := c.bySec[sec]
	for _, x := range l {
		if addr >= x.off && addr < x.off+x.tab.TextSize {
			if addr == x.off && strings.HasPrefix(x.sym.Name, c.pkg+".") {
				base := strings.TrimPrefix(x.sym.Name, c.pkg+".")
				if libcallImpl[base] {
					impl := c.pkg + "." + base + ".impl"
					if ab, ok := c.abiByGoSym[impl]; ok && ab == obj.ABI0 {
						// A same-TU ELF call or function pointer resolves by
						// address to the Go thunk. C uses the SysV body.
						return c.ctxt.LookupABI(impl, obj.ABI0), 0
					}
				}
			}
			return x.sym, int64(addr - x.off)
		}
	}
	for _, x := range l {
		if x.off > addr {
			return x.sym, -int64(x.off - addr)
		}
	}
	return nil, 0
}

// resolveCName maps a C symbol name (an undefined ELF symbol) to the Go
// symbol that defines it. The goabi rename appends ".impl" to C-side
// declarations: a C function with an ABI0 body resolves to
// "<pkg>.<base>.impl", anything else to the package-qualified name.
func (c *converter) resolveCName(name string) *obj.LSym {
	ctxt := c.ctxt
	name = strings.TrimSuffix(name, "@plt")
	if strings.HasSuffix(name, ".goabi") {
		// Inline C wrappers may call a Go function explicitly after restoring
		// g in R14. The suffix names the relocation ABI, not the Go symbol.
		return ctxt.LookupABI(strings.TrimSuffix(name, ".goabi"), obj.ABIInternal)
	}
	if libcallImpl[name] {
		return ctxt.LookupABI(c.pkg+"."+name+".impl", obj.ABI0)
	}
	if g, ok := c.goName[name]; ok {
		name = g
	}
	if ab, ok := c.abiByGoSym[name]; ok {
		return ctxt.LookupABI(name, ab)
	}
	if name == "llvm.trap" || name == "llvm.trap.impl" {
		return ctxt.Lookup("runtime.abort")
	}
	if name == "" || strings.HasPrefix(name, c.pkg+".") {
		return ctxt.Lookup(name)
	}
	base, suffix := name, ""
	if strings.HasSuffix(base, ".impl") {
		base, suffix = strings.TrimSuffix(base, ".impl"), ".impl"
	}
	if strings.Contains(base, ".") {
		return ctxt.Lookup(name) // Go/runtime symbol: verbatim
	}
	if suffix == ".impl" || libcallImpl[base] {
		return ctxt.LookupABI(c.pkg+"."+base+".impl", obj.ABI0)
	}
	return ctxt.Lookup(c.pkg + "." + base + suffix)
}

func pcRelELF(t uint32) bool {
	switch elf.R_X86_64(t) {
	case elf.R_X86_64_PC32, elf.R_X86_64_PLT32, elf.R_X86_64_GOTPCREL, elf.R_X86_64_GOTPCRELX, elf.R_X86_64_REX_GOTPCRELX:
		return true
	}
	return false
}

// resolveTarget maps an ELF relocation to a Go symbol and the offset of the
// referenced address from that symbol's start (PC-relative forms lose their
// -4: Go's R_PCREL/R_CALL subtract the field width themselves).
func (c *converter) resolveTarget(r rela) (*obj.LSym, int64, string) {
	abs := r.Add
	if !c.arm64 && pcRelELF(r.Type) {
		abs += 4
	}
	if r.Sym == 0 || int(r.Sym-1) >= len(c.syms) {
		return nil, 0, ""
	}
	es := c.syms[r.Sym-1]
	sec := int(es.Section)
	switch {
	case es.Section == elf.SHN_UNDEF:
		return c.resolveCName(es.Name), abs, es.Name
	case c.isText(sec):
		// Section symbol (value 0) or a named label: fold into the function.
		addr := int64(es.Value) + abs
		if addr < 0 {
			return nil, 0, es.Name
		}
		ts, delta := c.resolveText(sec, uint64(addr))
		return ts, delta, es.Name
	default:
		if ls, ok := c.dataSym[int(r.Sym-1)]; ok {
			return ls, abs, es.Name
		}
		if ls, ok := c.secSym[sec]; ok {
			return ls, int64(es.Value) + abs, es.Name
		}
		return nil, 0, es.Name
	}
}

// gotSlot materializes a Go data word holding the target address for a
// GOTPCREL reference, which is then made PC-relative to that word.
func (c *converter) gotSlot(target *obj.LSym, add int64) *obj.LSym {
	key := fmt.Sprintf("%s+%d", target.Name, add)
	if ls, ok := c.gotSyms[key]; ok {
		return ls
	}
	ls := c.ctxt.Lookup("goc.got." + c.tu + "." + sanitize(target.Name))
	ls.Type = objabi.SDATA
	c.ctxt.GloblPos(ls, 8, obj.NOPTR, src.NoXPos)
	ls.WriteInt(c.ctxt, 0, 8, 0) // the reloc needs bytes to land in
	ls.AddRel(c.ctxt, obj.Reloc{Off: 0, Siz: 8, Type: objabi.R_ADDR, Add: add, Sym: target})
	c.gotSyms[key] = ls
	return ls
}

func sanitize(name string) string {
	out := []byte(name)
	for i, ch := range out {
		if !(ch == '_' || ch == '.' || ch == '$' || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) {
			out[i] = '_'
		}
	}
	return string(out)
}

func writeBytes(ctxt *obj.Link, ls *obj.LSym, b []byte) {
	off := 0
	for ; off+4 <= len(b); off += 4 {
		ls.WriteInt(ctxt, int64(off), 4, int64(binary.LittleEndian.Uint32(b[off:])))
	}
	for ; off < len(b); off++ {
		ls.WriteInt(ctxt, int64(off), 1, int64(b[off]))
	}
}
