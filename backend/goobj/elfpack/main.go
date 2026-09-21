// elfpack packs an LLVM llc-produced ELF .o into a Go goobj.
//
// Instruction bytes and ELF relocs come from LLVM MC (llc). This program only:
//   - renames MIR symbols to Go ABI names (meta.json)
//   - translates ELF relocs → goobj Reloc (R_CALL / R_PCREL) with addend check
//   - attaches FUNCDATA (Args/Locals pointer maps) via a tiny Prog plist
//   - emits dense PCDATA_StackMapIndex at each CALL safepoint (from meta.calls)
//   - rebuilds pcsp consistent with LLVM Go frame (PUSH BP + SUB $frame)
//
// Primary path for P12/P13. Does not use mirlower opcode switches for encoding.
package main

import (
	"bufio"
	"debug/elf"
	"encoding/binary"
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"goc.local/p5-machinepass-goobj/goobj/enc/cmd/objlib/bio"
	"goc.local/p5-machinepass-goobj/goobj/enc/cmd/objlib/obj"
	"goc.local/p5-machinepass-goobj/goobj/enc/cmd/objlib/obj/x86"
	"goc.local/p5-machinepass-goobj/goobj/enc/cmd/objlib/objabi"
	"goc.local/p5-machinepass-goobj/goobj/enc/cmd/objlib/src"
	"goc.local/p5-machinepass-goobj/goobj/enc/stdinternal/abi"
)

type metaFile struct {
	Functions []metaFn   `json:"functions"`
	Mircanon  *metaCanon `json:"mircanon"`
}

type metaCanon struct {
	Mode          string   `json:"mode"`
	Transforms    []string `json:"transforms"`
	CfgRewrite    bool     `json:"cfg_rewrite"`
	FrameInject   bool     `json:"frame_inject"`
	DialectStrip  bool     `json:"dialect_strip"`
}

type metaCall struct {
	Callee         string `json:"callee"`
	StackmapIndex  int    `json:"stackmap_index"`
}

type metaFn struct {
	MIRName    string     `json:"mir_name"`
	GoSym      string     `json:"go_sym"`
	Frame      int        `json:"frame"`
	Flags      string     `json:"flags"`
	Encoding   string     `json:"encoding"`
	Calls      []metaCall `json:"calls"`
	// P21 optional: color-vertical per-fn maps (additive; P16 ignores these).
	ColorFP    string `json:"color_fp,omitempty"`
	MapsSubdir string `json:"maps_subdir,omitempty"`
	ArgsMap    string `json:"args_map,omitempty"`
	LocalsMap  string `json:"locals_map,omitempty"`
}

type textReq struct {
	mirName   string
	goSym     string
	frame     int
	flags     int
	argsMap   string
	localsMap string
	abi       obj.ABI
	calls     []metaCall
}

// makeConstPcsp builds a pctab that maps the entire function to a constant SP delta.
func makeConstPcsp(ctxt *obj.Link, size int64, spdelta int32) *obj.LSym {
	sym := &obj.LSym{
		Type:      objabi.SRODATA,
		Attribute: obj.AttrContentAddressable | obj.AttrPcdata,
	}
	if size <= 0 {
		return sym
	}
	buf := make([]byte, binary.MaxVarintLen64)
	var dst []byte
	n := binary.PutVarint(buf, int64(spdelta-(-1)))
	dst = append(dst, buf[:n]...)
	n = binary.PutUvarint(buf, uint64(size))
	dst = append(dst, buf[:n]...)
	dst = append(dst, 0)
	sym.P = dst
	sym.Size = int64(len(dst))
	return sym
}

// pcValue is a (pc, value) transition for a pctab. Value applies from that PC onward.
type pcValue struct {
	PC    int64
	Value int32
}

// makePcdataFromTransitions builds a Go pctab from sorted PC→value transitions.
// Runtime step(): start val=-1; each (valΔ,pcΔ) applies val then advances pc;
// value holds for PCs in [prevpc, pc). See runtime.step.
func makePcdataFromTransitions(ctxt *obj.Link, size int64, trans []pcValue) *obj.LSym {
	sym := &obj.LSym{
		Type:      objabi.SRODATA,
		Attribute: obj.AttrContentAddressable | obj.AttrPcdata,
	}
	if size <= 0 {
		return sym
	}
	sort.Slice(trans, func(i, j int) bool { return trans[i].PC < trans[j].PC })
	type seg struct {
		val   int32
		start int64
	}
	segs := []seg{{val: -1, start: 0}}
	for _, tr := range trans {
		if tr.PC < 0 || tr.PC >= size {
			continue
		}
		last := &segs[len(segs)-1]
		if tr.PC == last.start {
			last.val = tr.Value
			continue
		}
		if tr.Value == last.val {
			continue
		}
		segs = append(segs, seg{val: tr.Value, start: tr.PC})
	}
	buf := make([]byte, binary.MaxVarintLen64)
	var dst []byte
	prevVal := int32(-1)
	for i, s := range segs {
		end := size
		if i+1 < len(segs) {
			end = segs[i+1].start
		}
		if end <= s.start {
			continue
		}
		n := binary.PutVarint(buf, int64(s.val)-int64(prevVal))
		dst = append(dst, buf[:n]...)
		n = binary.PutUvarint(buf, uint64(end-s.start))
		dst = append(dst, buf[:n]...)
		prevVal = s.val
	}
	dst = append(dst, 0)
	sym.P = dst
	sym.Size = int64(len(dst))
	return sym
}

func mustRead(path string) []byte {
	b, err := os.ReadFile(path)
	if err != nil {
		panic(err)
	}
	return b
}

func readOpt(path string) []byte {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil
	}
	return b
}

func emitStackmapROData(ctxt *obj.Link, name string, b []byte) {
	s := ctxt.Lookup(name)
	ctxt.GloblPos(s, int64(len(b)), obj.RODATA|obj.DUPOK, src.NoXPos)
	off := 0
	for off+4 <= len(b) {
		v := int64(binary.LittleEndian.Uint32(b[off : off+4]))
		s.WriteInt(ctxt, int64(off), 4, v)
		off += 4
	}
	for off < len(b) {
		s.WriteInt(ctxt, int64(off), 1, int64(b[off]))
		off++
	}
}

// findCallSites returns file-relative offsets of CALL rel32 (0xe8) in code,
// using ELF PLT32/PC32 relocs whose preceding byte is 0xe8.
func findCallSites(code []byte, baseOff uint64, rels []rela, syms []elf.Symbol) []callSite {
	var out []callSite
	for _, r := range rels {
		if r.Off < baseOff || r.Off >= baseOff+uint64(len(code)) {
			continue
		}
		off := int(r.Off - baseOff)
		if off < 1 || code[off-1] != 0xe8 {
			continue
		}
		if r.Type != uint32(elf.R_X86_64_PLT32) && r.Type != uint32(elf.R_X86_64_PC32) {
			continue
		}
		ename := ""
		if r.Sym > 0 && int(r.Sym-1) < len(syms) {
			ename = syms[r.Sym-1].Name
		}
		out = append(out, callSite{Off: off - 1, RelOff: off, Callee: ename, Add: r.Add, Type: r.Type})
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Off < out[j].Off })
	return out
}

type callSite struct {
	Off    int // offset of 0xe8
	RelOff int // offset of reloc field
	Callee string
	Add    int64
	Type   uint32
}

type rela struct {
	Off  uint64
	Type uint32
	Sym  uint32
	Add  int64
}

func main() {
	outO := flag.String("out-o", "", "output goobj path")
	elfPath := flag.String("elf", "", "llc-produced ELF .o")
	metaPath := flag.String("meta", "", "harness.meta.json (P14 sidecar; mirguard identity)")
	mapsDir := flag.String("maps", "", "pass-out dir with maps")
	pkg := flag.String("p", "main", "package path")
	checkOnly := flag.Bool("check-relocs", false, "validate ELF reloc addends then exit 0")
	flag.Parse()
	if *outO == "" || *elfPath == "" || *metaPath == "" || *mapsDir == "" {
		fmt.Fprintf(os.Stderr, "usage: elfpack -elf F.o -meta meta.json -maps DIR -out-o OUT.o\n")
		os.Exit(2)
	}

	var meta metaFile
	if err := json.Unmarshal(mustRead(*metaPath), &meta); err != nil {
		fmt.Fprintf(os.Stderr, "elfpack: meta: %v\n", err)
		os.Exit(1)
	}
	if meta.Mircanon != nil {
		if meta.Mircanon.CfgRewrite || meta.Mircanon.FrameInject || meta.Mircanon.DialectStrip {
			fmt.Fprintf(os.Stderr, "elfpack: FATAL meta says cfg_rewrite/frame_inject/dialect_strip still enabled (P14 requires identity)\n")
			os.Exit(1)
		}
		if len(meta.Mircanon.Transforms) != 0 {
			fmt.Fprintf(os.Stderr, "elfpack: FATAL meta.mircanon.transforms must be empty (P14 identity), got %v\n", meta.Mircanon.Transforms)
			os.Exit(1)
		}
	}

	ef, err := elf.Open(*elfPath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "elfpack: elf open: %v\n", err)
		os.Exit(1)
	}
	defer ef.Close()

	textSec := ef.Section(".text")
	if textSec == nil {
		fmt.Fprintf(os.Stderr, "elfpack: FATAL no .text in %s\n", *elfPath)
		os.Exit(1)
	}
	textData, err := textSec.Data()
	if err != nil {
		panic(err)
	}
	syms, err := ef.Symbols()
	if err != nil {
		panic(err)
	}
	var rels []rela
	if relaSec := ef.Section(".rela.text"); relaSec != nil {
		rd, err := relaSec.Data()
		if err != nil {
			panic(err)
		}
		for i := 0; i+24 <= len(rd); i += 24 {
			off := binary.LittleEndian.Uint64(rd[i : i+8])
			info := binary.LittleEndian.Uint64(rd[i+8 : i+16])
			add := int64(binary.LittleEndian.Uint64(rd[i+16 : i+24]))
			rels = append(rels, rela{
				Off:  off,
				Type: uint32(info & 0xffffffff),
				Sym:  uint32(info >> 32),
				Add:  add,
			})
		}
	} else {
		// No relocations (e.g. SSE leaf with only physregs) — allowed.
		fmt.Printf("elfpack: note: no .rela.text in %s (zero relocs)\n", *elfPath)
	}

	// P13 reloc hardening: PLT32 addend must be -4; PC32 typically -4 or -5 (with rex/modrm).
	relocOK := 0
	for _, r := range rels {
		switch r.Type {
		case uint32(elf.R_X86_64_PLT32):
			if r.Add != -4 {
				fmt.Fprintf(os.Stderr, "elfpack: FATAL PLT32 at 0x%x addend=%d want -4 (llc PIC call)\n", r.Off, r.Add)
				os.Exit(1)
			}
			relocOK++
		case uint32(elf.R_X86_64_PC32):
			// RIP-relative data: addend is usually -4; some encodings use -5.
			if r.Add != -4 && r.Add != -5 {
				fmt.Fprintf(os.Stderr, "elfpack: FATAL PC32 at 0x%x addend=%d want -4 or -5\n", r.Off, r.Add)
				os.Exit(1)
			}
			relocOK++
		}
	}
	fmt.Printf("elfpack: reloc check OK (%d PLT32/PC32 addends match llc PIC expectations)\n", relocOK)
	if *checkOnly {
		fmt.Println("elfpack: -check-relocs done")
		return
	}

	type elfFn struct {
		name string
		off  uint64
		size uint64
	}
	byMIR := map[string]elfFn{}
	for _, s := range syms {
		if elf.ST_TYPE(s.Info) != elf.STT_FUNC {
			continue
		}
		byMIR[s.Name] = elfFn{name: s.Name, off: s.Value, size: s.Size}
	}

	argsMap := mustRead(filepath.Join(*mapsDir, "args_map.bin"))
	localsMap := mustRead(filepath.Join(*mapsDir, "locals_map.bin"))
	holdTwoLocals := readOpt(filepath.Join(*mapsDir, "hold_two", "locals_map.bin"))
	holdTwoArgs := readOpt(filepath.Join(*mapsDir, "hold_two", "args_map.bin"))
	holdRegLocals := readOpt(filepath.Join(*mapsDir, "hold_regonly", "locals_map.bin"))
	holdRegArgs := readOpt(filepath.Join(*mapsDir, "hold_regonly", "args_map.bin"))

	// P21: optional per-fn maps from meta.maps_subdir / args_map / locals_map.
	type p21Maps struct {
		argsSym, localsSym string
		args, locals       []byte
	}
	p21ByMIR := map[string]p21Maps{}
	for _, fn := range meta.Functions {
		if fn.MapsSubdir == "" && fn.ArgsMap == "" && fn.LocalsMap == "" {
			continue
		}
		sub := fn.MapsSubdir
		var a, l []byte
		if sub != "" {
			a = readOpt(filepath.Join(*mapsDir, sub, "args_map.bin"))
			l = readOpt(filepath.Join(*mapsDir, sub, "locals_map.bin"))
		}
		if len(a) == 0 {
			a = argsMap
		}
		if len(l) == 0 {
			l = localsMap
		}
		as := fn.ArgsMap
		ls := fn.LocalsMap
		if as == "" {
			as = "gclocals." + fn.MIRName + "Args"
		}
		if ls == "" {
			ls = "gclocals." + fn.MIRName + "Locals"
		}
		p21ByMIR[fn.MIRName] = p21Maps{argsSym: as, localsSym: ls, args: a, locals: l}
	}

	emitStackmap := func(ctxt *obj.Link) {
		emitStackmapROData(ctxt, "gclocals.gocHoldArgs", argsMap)
		emitStackmapROData(ctxt, "gclocals.gocHoldLive", localsMap)
		if len(holdTwoLocals) > 0 {
			emitStackmapROData(ctxt, "gclocals.gocHoldTwo", holdTwoLocals)
		}
		if len(holdTwoArgs) > 0 {
			emitStackmapROData(ctxt, "gclocals.gocHoldTwoArgs", holdTwoArgs)
		}
		if len(holdRegLocals) > 0 {
			emitStackmapROData(ctxt, "gclocals.gocHoldRegOnly", holdRegLocals)
		}
		if len(holdRegArgs) > 0 {
			emitStackmapROData(ctxt, "gclocals.gocHoldRegOnlyArgs", holdRegArgs)
		} else if len(holdRegLocals) > 0 {
			emitStackmapROData(ctxt, "gclocals.gocHoldRegOnlyArgs", argsMap)
		}
		for _, pm := range p21ByMIR {
			if len(pm.args) > 0 {
				emitStackmapROData(ctxt, pm.argsSym, pm.args)
			}
			if len(pm.locals) > 0 {
				emitStackmapROData(ctxt, pm.localsSym, pm.locals)
			}
		}
	}

	reqs := make([]textReq, 0, len(meta.Functions))
	for _, fn := range meta.Functions {
		flags := 0
		if strings.Contains(fn.Flags, "nosplit") {
			flags |= obj.NOSPLIT
		}
		tr := textReq{
			mirName: fn.MIRName,
			goSym:   fn.GoSym,
			frame:   fn.Frame,
			flags:   flags,
			abi:     obj.ABIInternal,
			calls:   fn.Calls,
		}
		if fn.MIRName == "goc_leaf" {
			tr.abi = obj.ABI0
		}
			switch fn.MIRName {
		case "goc_hold_live", "goc_hold_arg":
			tr.argsMap = "gclocals.gocHoldArgs"
			tr.localsMap = "gclocals.gocHoldLive"
		case "goc_hold_two":
			tr.localsMap = "gclocals.gocHoldTwo"
			tr.argsMap = "gclocals.gocHoldArgs"
			if len(holdTwoArgs) > 0 {
				tr.argsMap = "gclocals.gocHoldTwoArgs"
			}
		case "goc_hold_regonly":
			tr.argsMap = "gclocals.gocHoldRegOnlyArgs"
			tr.localsMap = "gclocals.gocHoldRegOnly"
		default:
			// Fixture / unknown: attach root maps when calls carry stackmap indices.
			hasSM := false
			for _, c := range fn.Calls {
				if c.StackmapIndex >= 0 {
					hasSM = true
					break
				}
			}
			if hasSM && len(localsMap) > 0 {
				tr.localsMap = "gclocals.gocHoldLive"
				tr.argsMap = "gclocals.gocHoldArgs"
			}
		}
		// P21 color-vertical: explicit maps from meta override switch defaults.
		if pm, ok := p21ByMIR[fn.MIRName]; ok {
			if pm.argsSym != "" && len(pm.args) > 0 {
				tr.argsMap = pm.argsSym
			}
			if pm.localsSym != "" && len(pm.locals) > 0 {
				tr.localsMap = pm.localsSym
			}
		}

		reqs = append(reqs, tr)
	}

	ctxt := obj.Linknew(&x86.Linkamd64)
	ctxt.IsAsm = true
	ctxt.Pkgpath = *pkg
	ctxt.DiagFunc = func(format string, args ...interface{}) {
		fmt.Fprintf(os.Stderr, "elfpack diag: "+format+"\n", args...)
		os.Exit(1)
	}
	ctxt.Bso = bufio.NewWriter(os.Stdout)
	defer ctxt.Bso.Flush()
	if ctxt.Arch.Init != nil {
		ctxt.Arch.Init(ctxt)
	}
	fileBase := src.NewFileBase("goc_elfpack_llvmmc", "goc_elfpack_llvmmc")
	pos := ctxt.PosTable.XPos(src.MakePos(fileBase, 1, 0))
	emitStackmap(ctxt)

	var plist obj.Plist
	var head, tail *obj.Prog
	add := func(p *obj.Prog) {
		if p == nil {
			return
		}
		if head == nil {
			head = p
		} else {
			tail.Link = p
		}
		for p.Link != nil {
			p = p.Link
		}
		tail = p
	}

	type pending struct {
		sym     *obj.LSym
		code    []byte
		baseOff uint64
		frame   int
		calls   []metaCall
		req     textReq
	}
	var pend []pending

	for _, req := range reqs {
		efn, ok := byMIR[req.mirName]
		if !ok || efn.size == 0 {
			fmt.Fprintf(os.Stderr, "elfpack: FATAL missing LLVM-encoded TEXT for %s in ELF\n", req.mirName)
			os.Exit(1)
		}
		if efn.off+efn.size > uint64(len(textData)) {
			fmt.Fprintf(os.Stderr, "elfpack: FATAL %s out of .text range\n", req.mirName)
			os.Exit(1)
		}
		code := append([]byte(nil), textData[efn.off:efn.off+efn.size]...)

		s := ctxt.LookupABI(req.goSym, req.abi)
		// Frame already in LLVM bytes; tell Go frame=0 so assembler does not double-prologue.
		ctxt.InitTextSym(s, req.flags, pos)
		tp := ctxt.NewProg()
		tp.As = obj.ATEXT
		tp.Pos = pos
		tp.Ctxt = ctxt
		tp.From = obj.Addr{Type: obj.TYPE_MEM, Name: obj.NAME_EXTERN, Sym: s}
		tp.To = obj.Addr{Type: obj.TYPE_TEXTSIZE, Offset: 0}
		tp.To.Val = int32(0)
		s.Func().Text = tp
		s.Func().Locals = 0
		s.Func().Args = 0
		add(tp)

		if req.argsMap != "" {
			p := ctxt.NewProg()
			p.As = obj.AFUNCDATA
			p.Pos = pos
			p.From = obj.Addr{Type: obj.TYPE_CONST, Offset: int64(abi.FUNCDATA_ArgsPointerMaps)}
			p.To = obj.Addr{Type: obj.TYPE_MEM, Name: obj.NAME_EXTERN, Sym: ctxt.Lookup(req.argsMap)}
			add(p)
		}
		if req.localsMap != "" {
			p := ctxt.NewProg()
			p.As = obj.AFUNCDATA
			p.Pos = pos
			p.From = obj.Addr{Type: obj.TYPE_CONST, Offset: int64(abi.FUNCDATA_LocalsPointerMaps)}
			p.To = obj.Addr{Type: obj.TYPE_MEM, Name: obj.NAME_EXTERN, Sym: ctxt.Lookup(req.localsMap)}
			add(p)
		}
		// Seed PCDATA_StackMapIndex=-1 at entry; dense table rebuilt after LLVM bytes.
		p := ctxt.NewProg()
		p.As = obj.APCDATA
		p.Pos = pos
		p.From = obj.Addr{Type: obj.TYPE_CONST, Offset: int64(abi.PCDATA_StackMapIndex)}
		p.To = obj.Addr{Type: obj.TYPE_CONST, Offset: -1}
		add(p)

		pend = append(pend, pending{sym: s, code: code, baseOff: efn.off, frame: req.frame, calls: req.calls, req: req})
		fmt.Printf("elfpack: %s ← LLVM MC %s (%d bytes, %d annotated CALLs)\n", req.goSym, req.mirName, len(code), len(req.calls))
	}

	plist.Firstpc = head
	obj.Flushplist(ctxt, &plist, nil)

	mirToGo := map[string]string{}
	for _, fn := range meta.Functions {
		mirToGo[fn.MIRName] = fn.GoSym
	}

	pcdataProof := []string{}

	for _, pe := range pend {
		s := pe.sym
		s.P = append([]byte(nil), pe.code...)
		s.Size = int64(len(s.P))
		s.R = nil

		sites := findCallSites(pe.code, pe.baseOff, rels, syms)

		// Reloc translation
		for _, r := range rels {
			if r.Off < pe.baseOff || r.Off >= pe.baseOff+uint64(len(pe.code)) {
				continue
			}
			off := int64(r.Off - pe.baseOff)
			ename := ""
			if r.Sym > 0 && int(r.Sym-1) < len(syms) {
				ename = syms[r.Sym-1].Name
			}
			if goName, ok := mirToGo[ename]; ok {
				ename = goName
			}
			if ename == "" {
				fmt.Fprintf(os.Stderr, "elfpack: FATAL reloc at %s+0x%x with empty symbol\n", s.Name, off)
				os.Exit(1)
			}
			target := ctxt.Lookup(ename)
			var typ objabi.RelocType
			switch r.Type {
			case uint32(elf.R_X86_64_PLT32), uint32(elf.R_X86_64_PC32):
				if off > 0 && pe.code[off-1] == 0xe8 {
					typ = objabi.R_CALL
				} else {
					typ = objabi.R_PCREL
				}
			case uint32(elf.R_X86_64_32S), uint32(elf.R_X86_64_32):
				typ = objabi.R_ADDR
			default:
				fmt.Fprintf(os.Stderr, "elfpack: FATAL unsupported ELF reloc type %d for %s at +0x%x\n", r.Type, s.Name, off)
				os.Exit(1)
			}
			// ELF: S+A-P; Go R_PCREL/R_CALL: S+A'-(P+siz) ⇒ A' = A + 4
			goAdd := r.Add + 4
			if typ == objabi.R_CALL && goAdd != 0 {
				fmt.Fprintf(os.Stderr, "elfpack: FATAL R_CALL addend for %s+0x%x: goAdd=%d want 0 (from ELF A=-4)\n", s.Name, off, goAdd)
				os.Exit(1)
			}
			s.AddRel(ctxt, obj.Reloc{
				Off:  int32(off),
				Siz:  4,
				Type: typ,
				Add:  goAdd,
				Sym:  target,
			})
		}

		// Dense PCDATA at CALL safepoints
		fi := s.Func()
		trans := []pcValue{} // start at -1 implicitly
		nSites := len(sites)
		nMeta := len(pe.calls)
		hasAnnotated := len(pe.calls) > 0
		for _, c := range pe.calls {
			if c.StackmapIndex >= 0 {
				hasAnnotated = true
				break
			}
		}
		if pe.req.localsMap != "" || pe.req.argsMap != "" || hasAnnotated {
			// Match CALL sites to meta call annotations by order.
			if nMeta > 0 && nSites != nMeta {
				// Tolerate: meta may only annotate safepoint CALLs; sites includes all CALLs.
				// Pair by callee name when possible.
				fmt.Printf("elfpack: %s CALL sites=%d meta.calls=%d (pairing by order/callee)\n", s.Name, nSites, nMeta)
			}
			used := make([]bool, nMeta)
			for _, cs := range sites {
				idx := -1
				// Prefer matching unused meta entry with same callee
				for i, mc := range pe.calls {
					if used[i] {
						continue
					}
					if mc.Callee == cs.Callee || strings.HasSuffix(cs.Callee, mc.Callee) || strings.HasSuffix(mc.Callee, cs.Callee) {
						idx = mc.StackmapIndex
						used[i] = true
						break
					}
				}
				if idx < 0 {
					// Fallback: next unused meta, else 0 if has maps
					for i, mc := range pe.calls {
						if !used[i] {
							idx = mc.StackmapIndex
							used[i] = true
							break
						}
					}
				}
				if idx < 0 {
					idx = 0
				}
				trans = append(trans, pcValue{PC: int64(cs.Off), Value: int32(idx)})
				pcdataProof = append(pcdataProof, fmt.Sprintf("%s+0x%x -> idx %d callee=%s", s.Name, cs.Off, idx, cs.Callee))
			}
		}
		// Ensure Pcdata slice has Slot for StackMapIndex; fill nil holes (linker SymSize panics on nil).
		need := abi.PCDATA_StackMapIndex + 1
		if len(fi.Pcln.Pcdata) < need {
			n := make([]*obj.LSym, need)
			copy(n, fi.Pcln.Pcdata)
			fi.Pcln.Pcdata = n
		}
		emptyPc := func() *obj.LSym {
			return &obj.LSym{Type: objabi.SRODATA, Attribute: obj.AttrContentAddressable | obj.AttrPcdata}
		}
		for i := range fi.Pcln.Pcdata {
			if fi.Pcln.Pcdata[i] == nil {
				fi.Pcln.Pcdata[i] = emptyPc()
			}
		}
		if len(trans) == 0 {
			if pe.req.localsMap != "" || pe.req.argsMap != "" {
				fi.Pcln.Pcdata[abi.PCDATA_StackMapIndex] = makeConstPcsp(ctxt, s.Size, 0)
			} else {
				fi.Pcln.Pcdata[abi.PCDATA_StackMapIndex] = makeConstPcsp(ctxt, s.Size, -1)
			}
		} else {
			fi.Pcln.Pcdata[abi.PCDATA_StackMapIndex] = makePcdataFromTransitions(ctxt, s.Size, trans)
		}
	}

	// pcsp: PUSH BP (+8) + SUB $frame → spdelta = frame+8 when frame>0
	for _, pe := range pend {
		s := pe.sym
		fi := s.Func()
		fi.Locals = int32(pe.frame)
		s.Set(obj.AttrNoFrame, pe.frame == 0)
		spdelta := int32(pe.frame)
		if pe.frame > 0 {
			// Prove LLVM bytes contain Go-style prologue: 55 (PUSH BP) near start
			if len(pe.code) < 4 || pe.code[0] != 0x55 {
				fmt.Fprintf(os.Stderr, "elfpack: FATAL %s frame=%d but TEXT does not start with PUSH BP (0x55); pcsp would lie\n", s.Name, pe.frame)
				os.Exit(1)
			}
			spdelta = int32(pe.frame) + 8
		}
		fi.Pcln.Pcsp = makeConstPcsp(ctxt, s.Size, spdelta)
		fi.Pcln.Pcfile = makeConstPcsp(ctxt, s.Size, 1)
		fi.Pcln.Pcline = makeConstPcsp(ctxt, s.Size, 1)
		fmt.Printf("elfpack: pcsp %s size=%d spdelta=%d (frame=%d)\n", s.Name, s.Size, spdelta, pe.frame)
	}

	ctxt.NumberSyms()
	buf, err := bio.Create(*outO)
	if err != nil {
		panic(err)
	}
	buf.WriteString(objabi.HeaderString())
	fmt.Fprintf(buf, "!\n")
	obj.WriteObjFile(ctxt, buf)
	if err := buf.Close(); err != nil {
		panic(err)
	}
	fmt.Println("elfpack: wrote goobj from LLVM MC ELF →", *outO)
	fmt.Println("elfpack: encoding=llvm-llc-mc (AsmPrinter/MCCodeEmitter); dense PCDATA at CALLs")
	// Write proof sidecar for build.sh
	proofPath := filepath.Join(filepath.Dir(*outO), "pcdata_proof.txt")
	_ = os.WriteFile(proofPath, []byte(strings.Join(pcdataProof, "\n")+"\n"), 0644)
	fmt.Println("elfpack: PCDATA proof →", proofPath, "lines=", len(pcdataProof))
}
