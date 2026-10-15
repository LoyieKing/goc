package main

import (
	"bufio"
	"debug/elf"
	"encoding/binary"
	"fmt"
	"math"
	"os"
	"path"
	"regexp"
	"sort"
	"strings"

	"github.com/LoyieKing/goc/gocobj"
	"github.com/LoyieKing/goc/goobj/enc/cmd/objlib/bio"
	"github.com/LoyieKing/goc/goobj/enc/cmd/objlib/goobj"
	"github.com/LoyieKing/goc/goobj/enc/cmd/objlib/obj"
	"github.com/LoyieKing/goc/goobj/enc/cmd/objlib/obj/x86"
	"github.com/LoyieKing/goc/goobj/enc/cmd/objlib/objabi"
	"github.com/LoyieKing/goc/goobj/enc/cmd/objlib/src"
	"github.com/LoyieKing/goc/goobj/enc/stdinternal/abi"
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

func isDataSection(name string) bool {
	for _, p := range []string{".rodata", ".data", ".bss", ".sdata", ".sbss", ".tdata", ".tbss"} {
		if strings.HasPrefix(name, p) {
			return true
		}
	}
	return false
}

// emitData copies the program data: one Go symbol per named global (other
// objects reference it by name) and one per allocated data section for the
// rest. C data holds C pointers, not Go pointers, so all of it is NOPTR.
func (c *converter) emitData() error {
	ctxt := c.ctxt
	secs := c.ef.Sections
	type nameRange struct {
		start, end uint64
		sym        *obj.LSym
	}
	ranges := map[int][]nameRange{}
	for i, es := range c.syms {
		sec := int(es.Section)
		if es.Section == elf.SHN_UNDEF || es.Section >= elf.SHN_LORESERVE || sec >= len(secs) || es.Size == 0 || es.Name == "" || c.isText(sec) {
			continue
		}
		if ty := elf.ST_TYPE(es.Info); ty != elf.STT_OBJECT && ty != elf.STT_NOTYPE {
			continue
		}
		if b := elf.ST_BIND(es.Info); b != elf.STB_GLOBAL && b != elf.STB_WEAK {
			continue
		}
		sc := secs[sec]
		if sc.Flags&elf.SHF_ALLOC == 0 {
			continue
		}
		ls := ctxt.Lookup(c.pkg + "." + es.Name)
		if sc.Type == elf.SHT_NOBITS {
			ctxt.GloblPos(ls, int64(es.Size), obj.NOPTR, src.NoXPos)
		} else {
			b, err := sc.Data()
			if err != nil {
				return err
			}
			if es.Value+es.Size > uint64(len(b)) {
				continue
			}
			ls.Type = objabi.SDATA
			ctxt.GloblPos(ls, int64(es.Size), obj.NOPTR, src.NoXPos)
			writeBytes(ctxt, ls, b[es.Value:es.Value+es.Size])
		}
		c.dataSym[i] = ls
		ranges[sec] = append(ranges[sec], nameRange{es.Value, es.Value + es.Size, ls})
	}
	for i, sc := range secs {
		if sc.Flags&elf.SHF_ALLOC == 0 || c.isText(i) || sc.Size == 0 ||
			(sc.Type != elf.SHT_PROGBITS && sc.Type != elf.SHT_NOBITS) || !isDataSection(sc.Name) {
			continue
		}
		ls := ctxt.Lookup("goc.data." + c.tu + "." + sanitize(sc.Name))
		if sc.Type == elf.SHT_NOBITS {
			ctxt.GloblPos(ls, int64(sc.Size), obj.NOPTR, src.NoXPos)
		} else {
			b, err := sc.Data()
			if err != nil {
				return err
			}
			fl := int(obj.RODATA | obj.DUPOK)
			if sc.Flags&elf.SHF_WRITE != 0 {
				ls.Type = objabi.SDATA
				fl = obj.NOPTR
			}
			ctxt.GloblPos(ls, int64(len(b)), fl, src.NoXPos)
			writeBytes(ctxt, ls, b)
		}
		c.secSym[i] = ls
	}

	// Jump tables: LLVM emits `.long case - table` as R_X86_64_PC32 against
	// the text section with addend case + (field - table). Each table base
	// is the target of a PC-relative code reference; recover the case
	// address from it before resolving the function.
	type tableBase struct {
		sec   int
		off   uint64
		owner *obj.LSym
	}
	var codeRels []struct {
		sec int
		r   rela
	}
	for sec := range c.bySec {
		rs, err := c.relas(sec)
		if err != nil {
			return err
		}
		for _, r := range rs {
			codeRels = append(codeRels, struct {
				sec int
				r   rela
			}{sec, r})
		}
	}
	for i, ls := range c.secSym {
		rs, err := c.relas(i)
		if err != nil {
			return err
		}
		if len(rs) == 0 {
			continue
		}
		for int64(len(ls.P)) < ls.Size {
			ls.WriteInt(ctxt, int64(len(ls.P)), 1, 0)
		}
		pcTextAt := map[uint64]bool{}
		for _, r := range rs {
			if !c.arm64 && elf.R_X86_64(r.Type) == elf.R_X86_64_PC32 && r.Sym > 0 && int(r.Sym-1) < len(c.syms) && c.isText(int(c.syms[r.Sym-1].Section)) {
				pcTextAt[r.Off] = true
			}
		}
		var tables []tableBase
		for _, cr := range codeRels {
			r := cr.r
			if c.arm64 || elf.R_X86_64(r.Type) != elf.R_X86_64_PC32 || r.Sym == 0 || int(r.Sym-1) >= len(c.syms) {
				continue
			}
			es := c.syms[r.Sym-1]
			if int(es.Section) != i {
				continue
			}
			base := int64(es.Value) + r.Add + 4
			if base < 0 || !pcTextAt[uint64(base)] {
				continue
			}
			owner, _ := c.resolveText(cr.sec, r.Off)
			if owner == nil {
				return fmt.Errorf("jump table %s+%#x has no owning function", secs[i].Name, base)
			}
			tables = append(tables, tableBase{cr.sec, uint64(base), owner})
		}
		sort.Slice(tables, func(a, b int) bool { return tables[a].off < tables[b].off })
		for j := 1; j < len(tables); j++ {
			if tables[j-1].off == tables[j].off && tables[j-1].owner != tables[j].owner {
				return fmt.Errorf("jump table %s+%#x has two owners", secs[i].Name, tables[j].off)
			}
		}
		for _, r := range rs {
			var target *obj.LSym
			var add int64
			var ename string
			switch {
			case pcTextAt[r.Off]:
				j := sort.Search(len(tables), func(j int) bool { return tables[j].off > r.Off }) - 1
				if j < 0 {
					return fmt.Errorf("no base for jump table entry %s+%#x", secs[i].Name, r.Off)
				}
				idx := int64(r.Off - tables[j].off)
				es := c.syms[r.Sym-1]
				actual := int64(es.Value) + r.Add - idx
				if actual < 0 {
					return fmt.Errorf("negative jump table target %s+%#x", secs[i].Name, r.Off)
				}
				var delta int64
				target, delta = c.resolveText(int(es.Section), uint64(actual))
				if target != tables[j].owner {
					return fmt.Errorf("jump table %s+%#x targets another function than its owner", secs[i].Name, r.Off)
				}
				// Go's R_PCREL subtracts the field address+4; keep the
				// word equal to case - table.
				add, ename = delta+idx+4, target.Name
			case r.Sym == 0:
				target, add, ename = ls, r.Add, secs[i].Name
			default:
				target, add, ename = c.resolveTarget(r)
			}
			if target == nil {
				return fmt.Errorf("data reloc at %s+%#x: unresolved %q", secs[i].Name, r.Off, ename)
			}
			host, off := ls, int64(r.Off)
			for _, nr := range ranges[i] {
				if r.Off >= nr.start && r.Off < nr.end {
					host, off = nr.sym, int64(r.Off-nr.start)
					break
				}
			}
			var typ objabi.RelocType
			siz := uint8(4)
			if c.arm64 {
				var ok bool
				if typ, siz, ok = arm64DataReloc(r.Type); !ok {
					return fmt.Errorf("unsupported arm64 data reloc type %d in %s at %#x", r.Type, secs[i].Name, r.Off)
				}
				host.AddRel(ctxt, obj.Reloc{Off: int32(off), Siz: siz, Type: typ, Add: add, Sym: target})
				continue
			}
			d, ok := gv.X86Data[elf.R_X86_64(r.Type)]
			if !ok {
				return fmt.Errorf("unsupported data reloc type %d in %s at %#x", r.Type, secs[i].Name, r.Off)
			}
			typ, siz = d.Type, d.Siz
			host.AddRel(ctxt, obj.Reloc{Off: int32(off), Siz: siz, Type: typ, Add: add, Sym: target})
		}
	}
	return nil
}

// pctab encodes plain (pc, value) entries as a Go pc-value table. Values
// hold from their pc to the next entry; the runtime starts at -1.
func pctab(ctxt *obj.Link, size int64, ents []gocobj.PCEntry) *obj.LSym {
	sym := &obj.LSym{Type: objabi.SRODATA, Attribute: obj.AttrContentAddressable | obj.AttrPcdata}
	if size <= 0 {
		return sym
	}
	type seg struct {
		val   int64
		start int64
	}
	segs := []seg{{-1, 0}}
	for _, e := range ents {
		pc := int64(e.PC)
		if pc < 0 || pc >= size {
			continue
		}
		last := &segs[len(segs)-1]
		if pc == last.start {
			last.val = e.Value
			continue
		}
		if e.Value == last.val {
			continue
		}
		segs = append(segs, seg{e.Value, pc})
	}
	minLC := int64(ctxt.Arch.MinLC)
	var dst []byte
	prev := int64(-1)
	for i, s := range segs {
		end := size
		if i+1 < len(segs) {
			end = segs[i+1].start
		}
		if end <= s.start {
			continue
		}
		dst = binary.AppendVarint(dst, s.val-prev)
		dst = binary.AppendUvarint(dst, uint64((end-s.start)/minLC))
		prev = s.val
	}
	dst = append(dst, 0)
	sym.P = dst
	sym.Size = int64(len(dst))
	return sym
}

// inlSym names an inlined C function in the inline tree: the Go symbol of
// its body in this object if it has one, else (a static function inlined
// everywhere) the name a body of it would have, which gets a stub.
func (c *converter) inlSym(name string) *obj.LSym {
	if gn, ok := c.goName[name]; ok {
		return c.ctxt.LookupABI(gn, c.abiByGoSym[gn])
	}
	return c.ctxt.Lookup(c.pkg + "." + c.tu + "." + name)
}

func pcTable(t *gocobj.Func, k gocobj.PCKind) []gocobj.PCEntry {
	for _, p := range t.PCValue {
		if p.Kind == k {
			return p.Entries
		}
	}
	return nil
}

// stackmaps turns a .goc.gcmaps record into the args and locals stack map
// blobs (n uint32, nbit uint32, n bitmaps) and the StackMapIndex entries:
// each safepoint's index holds from the byte before its return address.
func stackmaps(g *gocobj.GCMap) (args, locals []byte, idx []gocobj.PCEntry) {
	type pair struct{ a, l string }
	seen := map[pair]int{}
	var as, ls [][]byte
	for _, sp := range g.Safepoints {
		k := pair{string(sp.Args), string(sp.Locals)}
		i, ok := seen[k]
		if !ok {
			i = len(as)
			seen[k] = i
			as = append(as, sp.Args)
			ls = append(ls, sp.Locals)
		}
		idx = append(idx, gocobj.PCEntry{PC: sp.PC - 1, Value: int64(i)})
	}
	blob := func(nbit uint64, maps [][]byte) []byte {
		stride := int((nbit + 7) / 8)
		b := make([]byte, 8, 8+len(maps)*stride)
		n := len(maps)
		if n == 0 {
			n = 1 // the runtime wants n > 0 even when nothing is scanned
		}
		binary.LittleEndian.PutUint32(b[0:], uint32(n))
		binary.LittleEndian.PutUint32(b[4:], uint32(nbit))
		for _, m := range maps {
			b = append(b, m...)
		}
		if len(maps) == 0 {
			b = append(b, make([]byte, stride)...)
		}
		return b
	}
	return blob(g.NArgs, as), blob(g.NLocals, ls), idx
}

func (c *converter) write(out string) error {
	arch := &x86.Linkamd64
	if c.arm64 {
		arch = &linkARM64
	}
	ctxt := obj.Linknew(arch)
	c.ctxt = ctxt
	ctxt.IsAsm = true
	ctxt.Pkgpath = c.pkg
	ctxt.DiagFunc = func(format string, args ...any) {
		fatalf("diag: "+format, args...)
	}
	ctxt.Bso = bufio.NewWriter(os.Stdout)
	defer ctxt.Bso.Flush()
	if ctxt.Arch.Init != nil {
		ctxt.Arch.Init(ctxt)
	}
	file := "goc"
	if len(c.f.Files) > 0 && c.f.Files[0] != "" {
		file = c.f.Files[0]
	}
	fileBase := src.NewFileBase(file, file)
	pos := ctxt.PosTable.XPos(src.MakePos(fileBase, 1, 0))
	// 1.4: every .goc.files entry gets a PosBase, so FILE values and
	// inline-tree call sites map to the object's file table.
	fileBases := []*src.PosBase{fileBase}
	for _, f := range c.f.Files[min(1, len(c.f.Files)):] {
		b := src.NewFileBase(f, f)
		ctxt.PosTable.XPos(src.MakePos(b, 1, 0))
		fileBases = append(fileBases, b)
	}
	goFile := func(i int64) int64 {
		if i < 0 || i >= int64(len(fileBases)) {
			return 0
		}
		return int64(fileBases[i].FileIndex())
	}

	for _, x := range c.fns {
		x.sym = ctxt.LookupABI(c.goName[x.name], x.abi)
	}
	if err := c.emitData(); err != nil {
		return err
	}

	// TEXT, FUNCDATA and PCDATA through the assembler's plist, which sets
	// up each symbol's FuncInfo; the bytes and tables are filled in after.
	var head, tail *obj.Prog
	add := func(p *obj.Prog) {
		p.Pos = pos
		if head == nil {
			head = p
		} else {
			tail.Link = p
		}
		tail = p
	}
	stackIdx := map[*fn][]gocobj.PCEntry{}
	for _, x := range c.fns {
		t := x.tab
		flags := 0
		if t.Flags&gocobj.FuncNosplit != 0 {
			flags |= obj.NOSPLIT
		}
		ctxt.InitTextSym(x.sym, flags, pos)
		tp := ctxt.NewProg()
		tp.As = obj.ATEXT
		tp.Ctxt = ctxt
		tp.From = obj.Addr{Type: obj.TYPE_MEM, Name: obj.NAME_EXTERN, Sym: x.sym}
		tp.To = obj.Addr{Type: obj.TYPE_TEXTSIZE, Offset: 0}
		tp.To.Val = int32(0)
		x.sym.Func().Text = tp
		add(tp)
		if t.GC != nil {
			args, locals, idx := stackmaps(t.GC)
			stackIdx[x] = idx
			for _, m := range []struct {
				kind int64
				name string
				blob []byte
			}{
				{gv.FuncdataArgs, "gclocals.goc." + x.sym.Name + ".args", args},
				{gv.FuncdataLocals, "gclocals.goc." + x.sym.Name + ".locals", locals},
			} {
				ms := ctxt.Lookup(m.name)
				ctxt.GloblPos(ms, int64(len(m.blob)), obj.RODATA|obj.DUPOK, src.NoXPos)
				writeBytes(ctxt, ms, m.blob)
				p := ctxt.NewProg()
				p.As = obj.AFUNCDATA
				p.From = obj.Addr{Type: obj.TYPE_CONST, Offset: m.kind}
				p.To = obj.Addr{Type: obj.TYPE_MEM, Name: obj.NAME_EXTERN, Sym: ms}
				add(p)
			}
		}
		p := ctxt.NewProg()
		p.As = obj.APCDATA
		p.From = obj.Addr{Type: obj.TYPE_CONST, Offset: gv.PcdataStackMapIndex}
		p.To = obj.Addr{Type: obj.TYPE_CONST, Offset: -1}
		add(p)
	}
	// 1.4: Go's linker reads the FuncID and start line of every inlined
	// function from its FuncInfo. A static C function inlined everywhere
	// has no body here, so it gets a one-byte stub (never referenced, so
	// the linker leaves it out of the binary).
	type stub struct {
		sym  *obj.LSym
		line uint64
	}
	var stubs []stub
	for _, x := range c.fns {
		for _, n := range x.tab.Inl {
			if _, ok := c.goName[n.Func]; ok {
				continue
			}
			ls := c.inlSym(n.Func)
			if ls.Func() != nil && ls.Func().Text != nil {
				continue
			}
			ctxt.InitTextSym(ls, obj.NOSPLIT|obj.DUPOK, pos)
			tp := ctxt.NewProg()
			tp.As = obj.ATEXT
			tp.Ctxt = ctxt
			tp.From = obj.Addr{Type: obj.TYPE_MEM, Name: obj.NAME_EXTERN, Sym: ls}
			tp.To = obj.Addr{Type: obj.TYPE_TEXTSIZE, Offset: 0}
			tp.To.Val = int32(0)
			ls.Func().Text = tp
			add(tp)
			stubs = append(stubs, stub{ls, n.StartLine})
		}
	}
	obj.Flushplist(ctxt, &obj.Plist{Firstpc: head}, nil)
	for _, st := range stubs {
		st.sym.P = []byte{0xcc} // int3
		if c.arm64 {
			st.sym.P = []byte{0, 0, 0x20, 0xd4} // brk #0
		}
		st.sym.Size = int64(len(st.sym.P))
		st.sym.Set(obj.AttrNoFrame, true)
		fi := st.sym.Func()
		fi.StartLine = int32(st.line)
		fi.Pcln.Pcdata = make([]*obj.LSym, gv.PcdataStackMapIndex+1)
		for i := range fi.Pcln.Pcdata {
			fi.Pcln.Pcdata[i] = pctab(ctxt, 0, nil)
		}
		fi.Pcln.Pcsp = pctab(ctxt, st.sym.Size, []gocobj.PCEntry{{PC: 0, Value: 0}})
		fi.Pcln.Pcfile = pctab(ctxt, st.sym.Size, []gocobj.PCEntry{{PC: 0, Value: 0}})
		fi.Pcln.Pcline = pctab(ctxt, st.sym.Size, []gocobj.PCEntry{{PC: 0, Value: int64(st.line)}})
		fi.Pcln.UsedFiles = map[goobj.CUFileIndex]struct{}{0: {}}
	}

	// Calls, by function and offset of their relocated field: the tables
	// list every call, so R_CALL vs R_PCREL is read from them.
	callAt := map[string]map[uint64]bool{}
	for _, r := range c.f.Relocs {
		if r.Kind == gocobj.RCall {
			if callAt[r.Func] == nil {
				callAt[r.Func] = map[uint64]bool{}
			}
			callAt[r.Func][r.Offset] = true
		}
	}
	for sec, l := range c.bySec {
		rs, err := c.relas(sec)
		if err != nil {
			return err
		}
		d, err := c.ef.Sections[sec].Data()
		if err != nil {
			return err
		}
		for _, x := range l {
			t := x.tab
			s := x.sym
			s.P = append([]byte(nil), d[x.off:x.off+t.TextSize]...)
			s.Size = int64(len(s.P))
			s.R = nil
			if c.arm64 {
				if err := c.arm64TextRelocs(x, rs); err != nil {
					return err
				}
			}
			for _, r := range rs {
				if c.arm64 {
					break
				}
				if r.Off < x.off || r.Off >= x.off+t.TextSize {
					continue
				}
				off := r.Off - x.off
				target, goAdd, ename := c.resolveTarget(r)
				if target == nil {
					return fmt.Errorf("reloc at %s+%#x: unresolved symbol %q", s.Name, off, ename)
				}
				ty := elf.R_X86_64(r.Type)
				if ty == elf.R_X86_64_GOTPCREL || ty == elf.R_X86_64_GOTPCRELX || ty == elf.R_X86_64_REX_GOTPCRELX {
					target, goAdd, ty = c.gotSlot(target, goAdd), 0, elf.R_X86_64_PC32
				}
				typ, ok := gv.X86Text[ty]
				if !ok {
					return fmt.Errorf("unsupported text reloc type %d in %s at %#x", r.Type, s.Name, off)
				}
				if (ty == elf.R_X86_64_PLT32 || ty == elf.R_X86_64_PC32) && callAt[x.name][off] {
					typ = gv.X86Call
					if goAdd != 0 {
						return fmt.Errorf("call at %s+%#x has addend %d", s.Name, off, goAdd)
					}
				}
				if r.Add < math.MinInt32 || r.Add > math.MaxInt32 {
					return fmt.Errorf("reloc at %s+%#x: addend %d out of range", s.Name, off, r.Add)
				}
				s.AddRel(ctxt, obj.Reloc{Off: int32(off), Siz: 4, Type: typ, Add: goAdd, Sym: target})
			}

			fi := s.Func()
			fi.Locals = int32(t.FrameSize)
			fi.Args = int32(t.ArgSize)
			s.Set(obj.AttrNoFrame, t.FrameSize == 0)
			s.Set(obj.AttrNoSplit, t.Flags&gocobj.FuncNosplit != 0)
			need := abi.PCDATA_StackMapIndex + 1
			if len(fi.Pcln.Pcdata) < need {
				n := make([]*obj.LSym, need)
				copy(n, fi.Pcln.Pcdata)
				fi.Pcln.Pcdata = n
			}
			for i := range fi.Pcln.Pcdata {
				if fi.Pcln.Pcdata[i] == nil {
					fi.Pcln.Pcdata[i] = pctab(ctxt, 0, nil)
				}
			}
			if sp := pcTable(t, gocobj.PCSPDelta); sp != nil {
				fi.Pcln.Pcsp = pctab(ctxt, s.Size, sp)
			} else {
				fi.Pcln.Pcsp = pctab(ctxt, s.Size, []gocobj.PCEntry{{PC: 0, Value: int64(t.FrameSize)}})
			}
			if up := pcTable(t, gocobj.PCUnsafePoint); up != nil {
				fi.Pcln.Pcdata[abi.PCDATA_UnsafePoint] = pctab(ctxt, s.Size, up)
			}
			fi.Pcln.Pcdata[abi.PCDATA_StackMapIndex] = pctab(ctxt, s.Size, stackIdx[x])
			fi.Pcln.Pcfile = pctab(ctxt, s.Size, []gocobj.PCEntry{{PC: 0, Value: 1}})
			fi.Pcln.Pcline = pctab(ctxt, s.Size, []gocobj.PCEntry{{PC: 0, Value: 1}})
			if t.StartLine != 0 {
				fi.StartLine = int32(t.StartLine)
			}
			if ft, lt := pcTable(t, gocobj.PCFile), pcTable(t, gocobj.PCLine); ft != nil && lt != nil {
				// 1.4: real C files and lines from the debug locations.
				fi.Pcln.UsedFiles = map[goobj.CUFileIndex]struct{}{}
				gf := make([]gocobj.PCEntry, len(ft))
				for i, e := range ft {
					gf[i] = gocobj.PCEntry{PC: e.PC, Value: goFile(e.Value)}
					fi.Pcln.UsedFiles[goobj.CUFileIndex(gf[i].Value)] = struct{}{}
				}
				fi.Pcln.Pcfile = pctab(ctxt, s.Size, gf)
				fi.Pcln.Pcline = pctab(ctxt, s.Size, lt)
				fi.Pcln.InlTree = obj.InlTree{}
				for _, n := range t.Inl {
					p := ctxt.PosTable.XPos(src.MakePos(fileBases[min(int(n.File), len(fileBases)-1)], uint(n.Line), 0))
					k := fi.Pcln.InlTree.Add(int(n.Parent), p, c.inlSym(n.Func), n.Func)
					fi.Pcln.InlTree.SetParentPC(k, int32(n.ParentPC))
				}
				if it := pcTable(t, gocobj.PCInlIndex); it != nil && len(t.Inl) > 0 {
					fi.Pcln.Pcinline = pctab(ctxt, s.Size, it)
				}
			}
		}
	}

	ctxt.NumberSyms()
	buf, err := bio.Create(out)
	if err != nil {
		return err
	}
	buf.WriteString(objabi.HeaderString())
	fmt.Fprintf(buf, "!\n")
	obj.WriteObjFile(ctxt, buf)
	return buf.Close()
}
