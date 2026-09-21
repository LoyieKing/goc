// Binary goobj writer for goc P5b/P9/P10.
//
// TEXT bodies come entirely from mirlower (pass-exported MI lists).
// This program only loads maps, emits gclocals RODATA, opens ATEXT,
// attaches FUNCDATA, and links the lowered Prog chain. There is NO
// hand-built Prog template for Hold*/WB/CheckedAdd/leaf/morestack bodies.
package main

import (
	"bufio"
	"encoding/binary"
	"encoding/hex"
	"flag"
	"fmt"
	"os"
	"path/filepath"

	"goc.local/p5-machinepass-goobj/goobj/enc/cmd/objlib/bio"
	"goc.local/p5-machinepass-goobj/goobj/enc/cmd/objlib/obj"
	"goc.local/p5-machinepass-goobj/goobj/enc/cmd/objlib/obj/x86"
	"goc.local/p5-machinepass-goobj/goobj/enc/cmd/objlib/objabi"
	"goc.local/p5-machinepass-goobj/goobj/enc/cmd/objlib/src"
	"goc.local/p5-machinepass-goobj/goobj/mirlower"
)

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

func textSym(ctxt *obj.Link, name string, flag int, frame, args int32, pos src.XPos, abi obj.ABI) *obj.Prog {
	s := ctxt.LookupABI(name, abi)
	ctxt.InitTextSym(s, flag, pos)
	p := ctxt.NewProg()
	p.As = obj.ATEXT
	p.Pos = pos
	p.Ctxt = ctxt
	p.From = obj.Addr{Type: obj.TYPE_MEM, Name: obj.NAME_EXTERN, Sym: s}
	p.To = obj.Addr{Type: obj.TYPE_TEXTSIZE, Offset: int64(frame)}
	p.To.Val = args
	s.Func().Text = p
	return p
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

type textReq struct {
	miName    string
	goSym     string
	argsMap   string
	localsMap string
}

func main() {
	outO := flag.String("out-o", "", "output goobj path")
	mapsDir := flag.String("maps", "", "pass-out dir with maps")
	recipe := flag.String("recipe", "", "stackcheck.recipe.txt")
	miLower := flag.String("mi-lower", "", "mi_lower.txt (required)")
	pkg := flag.String("p", "main", "package path")
	flag.Parse()
	if *outO == "" || *mapsDir == "" {
		fmt.Fprintf(os.Stderr, "usage: binwriter -maps DIR -out-o FILE.o [-recipe FILE] -mi-lower FILE [-p main]\n")
		os.Exit(2)
	}
	if *miLower == "" {
		*miLower = filepath.Join(*mapsDir, "mi_lower.txt")
	}

	argsMap := mustRead(filepath.Join(*mapsDir, "args_map.bin"))
	localsMap := mustRead(filepath.Join(*mapsDir, "locals_map.bin"))
	holdTwoLocals := readOpt(filepath.Join(*mapsDir, "hold_two", "locals_map.bin"))
	holdTwoArgs := readOpt(filepath.Join(*mapsDir, "hold_two", "args_map.bin"))
	holdRegLocals := readOpt(filepath.Join(*mapsDir, "hold_regonly", "locals_map.bin"))
	holdRegArgs := readOpt(filepath.Join(*mapsDir, "hold_regonly", "args_map.bin"))

	if *recipe != "" {
		r := string(mustRead(*recipe))
		need := []string{"stackguard0_offset 16", "morestack runtime.morestack_noctxt", "mi_path=real_x86_opcodes"}
		for _, n := range need {
			if !contains(r, n) {
				fmt.Fprintf(os.Stderr, "binwriter: recipe missing %q\n", n)
				os.Exit(1)
			}
		}
		fmt.Println("binwriter: recipe OK (real_x86_opcodes, stackguard0=16, morestack_noctxt)")
	}

	miFile, err := mirlower.Load(*miLower)
	if err != nil {
		fmt.Fprintf(os.Stderr, "binwriter: FATAL mi_lower required: %v\n", err)
		os.Exit(1)
	}
	fmt.Println("binwriter: MIR→goobj mi_lower OK format=", miFile.Format, "abi=", miFile.ABI)
	fmt.Println("binwriter: mirlower limits:\n" + mirlower.Limits())
	fmt.Printf("binwriter: args_map=%s locals_map=%s\n", hex.EncodeToString(argsMap), hex.EncodeToString(localsMap))

	ctxt := obj.Linknew(&x86.Linkamd64)
	ctxt.IsAsm = true
	ctxt.Pkgpath = *pkg
	ctxt.DiagFunc = func(format string, args ...interface{}) {
		fmt.Fprintf(os.Stderr, "binwriter diag: "+format+"\n", args...)
		os.Exit(1)
	}
	ctxt.Bso = bufio.NewWriter(os.Stdout)
	defer ctxt.Bso.Flush()
	if ctxt.Arch.Init != nil {
		ctxt.Arch.Init(ctxt)
	}

	fileBase := src.NewFileBase("goc_binwriter", "goc_binwriter")
	pos := ctxt.PosTable.XPos(src.MakePos(fileBase, 1, 0))

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

	reqs := []textReq{
		{miName: "goc_checked_add", goSym: "main.GocCheckedAdd"},
		{miName: "goc_hold_live", goSym: "main.GocHoldLive", argsMap: "gclocals.gocHoldArgs", localsMap: "gclocals.gocHoldLive"},
		{miName: "goc_hold_arg", goSym: "main.GocHoldArg", argsMap: "gclocals.gocHoldArgs", localsMap: "gclocals.gocHoldLive"},
		{miName: "goc_store_gptr", goSym: "main.StoreGptrWB"},
		{miName: "goc_leaf", goSym: "goc_leaf"},
	}
	if len(holdTwoLocals) > 0 {
		am := "gclocals.gocHoldArgs"
		if len(holdTwoArgs) > 0 {
			am = "gclocals.gocHoldTwoArgs"
		}
		reqs = append(reqs, textReq{miName: "goc_hold_two", goSym: "main.GocHoldTwo", argsMap: am, localsMap: "gclocals.gocHoldTwo"})
	}
	if len(holdRegLocals) > 0 {
		reqs = append(reqs, textReq{miName: "goc_hold_regonly", goSym: "main.GocHoldRegOnly", argsMap: "gclocals.gocHoldRegOnlyArgs", localsMap: "gclocals.gocHoldRegOnly"})
	}

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

	for _, req := range reqs {
		fn, err := miFile.RequireFn(req.miName)
		if err != nil {
			fmt.Fprintf(os.Stderr, "binwriter: FATAL %v\n", err)
			os.Exit(1)
		}
		goSym := req.goSym
		if fn.GoSym != "" {
			goSym = fn.GoSym
		}
		abi := obj.ABIInternal
		if req.miName == "goc_leaf" {
			// Leaf is called via NAME_EXTERN Lookup (ABI0) with SysV-ish DI/SI→AX.
			abi = obj.ABI0
		}
		tp := textSym(ctxt, goSym, fn.Flags, fn.Frame, fn.Args, pos, abi)
		add(tp)

		var glue *obj.Prog
		if req.argsMap != "" || req.localsMap != "" {
			glue = mirlower.EmitFUNCDATA(ctxt, pos, req.argsMap, req.localsMap)
		}
		body, err := mirlower.LowerFn(ctxt, pos, fn)
		if err != nil {
			fmt.Fprintf(os.Stderr, "binwriter: FATAL lower %s: %v\n", req.miName, err)
			os.Exit(1)
		}
		add(mirlower.AttachFront(glue, body))
		fmt.Printf("binwriter: %s ← MIR lower (%s) frame=%d flags=%d ops=%d\n",
			goSym, req.miName, fn.Frame, fn.Flags, len(fn.Ops))
	}

	plist.Firstpc = head
	obj.Flushplist(ctxt, &plist, nil)
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
	fmt.Println("binwriter: wrote binary goobj (WriteObjFile, no go tool asm) →", *outO)
}

func contains(s, sub string) bool {
	return len(sub) == 0 || s == sub || (len(s) >= len(sub) && func() bool {
		for i := 0; i+len(sub) <= len(s); i++ {
			if s[i:i+len(sub)] == sub {
				return true
			}
		}
		return false
	}())
}
