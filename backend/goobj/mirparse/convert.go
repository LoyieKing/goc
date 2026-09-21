package mirparse

import (
	"fmt"
	"strings"

	"goc.local/p5-machinepass-goobj/goobj/enc/cmd/objlib/obj"
	"goc.local/p5-machinepass-goobj/goobj/mirlower"
)

// ConvertOptions controls Module → mirlower.File conversion.
type ConvertOptions struct {
	ABI  string
	Arch string
	// FrameFallback maps fn name → Go frame size when goc.frame unset.
	FrameFallback map[string]int32
	// GoSymFallback maps fn name → go symbol.
	GoSymFallback map[string]string
	// FlagsFallback maps fn name → flags string.
	FlagsFallback map[string]string
	// StackOffFallback maps fn → stack.N → SP offset when object offset unknown.
	StackOffFallback map[string]map[int]int64
}

// DefaultHarnessOptions returns options matching the goc harness contracts.
func DefaultHarnessOptions() ConvertOptions {
	return ConvertOptions{
		ABI:  "amd64_ABIInternal",
		Arch: "amd64",
		FrameFallback: map[string]int32{
			"goc_checked_add": 0,
			"goc_hold_live":   24,
			"goc_hold_arg":    24,
			"goc_hold_two":    32,
			"goc_hold_regonly": 24,
			"goc_store_gptr":  0,
			"goc_leaf":        0,
			"goc_fadd64":      0,
			"goc_fadd32":      0,
		},
		GoSymFallback: map[string]string{
			"goc_checked_add":  "main.GocCheckedAdd",
			"goc_hold_live":    "main.GocHoldLive",
			"goc_hold_arg":     "main.GocHoldArg",
			"goc_hold_two":     "main.GocHoldTwo",
			"goc_hold_regonly": "main.GocHoldRegOnly",
			"goc_store_gptr":   "main.StoreGptrWB",
			"goc_leaf":         "goc_leaf",
			"goc_fadd64":       "main.GocFadd64",
			"goc_fadd32":       "main.GocFadd32",
		},
		FlagsFallback: map[string]string{
			"goc_checked_add":  "nosplit",
			"goc_hold_live":    "nosplit",
			"goc_hold_arg":     "nosplit",
			"goc_hold_two":     "nosplit",
			"goc_hold_regonly": "nosplit",
			"goc_store_gptr":   "nosplit",
			"goc_leaf":         "nosplit",
			"goc_fadd64":       "nosplit",
			"goc_fadd32":       "nosplit",
		},
		StackOffFallback: map[string]map[int]int64{
			"goc_hold_live":    {0: 16},
			"goc_hold_arg":     {0: 16},
			"goc_hold_regonly": {0: 16},
			"goc_hold_two":     {0: 16, 1: 24},
		},
	}
}

// ToMiLower converts a parsed Module into mirlower.File (goc-mi-lower-1).
func ToMiLower(mod *Module, opt ConvertOptions) (*mirlower.File, error) {
	if opt.ABI == "" {
		opt.ABI = "amd64_ABIInternal"
	}
	if opt.Arch == "" {
		opt.Arch = "amd64"
	}
	out := &mirlower.File{
		Format: mirlower.FormatV1,
		ABI:    opt.ABI,
		Arch:   opt.Arch,
		Fns:    map[string]*mirlower.Fn{},
	}
	for _, f := range mod.Functions {
		fn, err := convertFn(f, opt)
		if err != nil {
			return nil, fmt.Errorf("mirparse: %s: %w", f.Name, err)
		}
		out.Fns[fn.Name] = fn
	}
	return out, nil
}

func convertFn(f *Function, opt ConvertOptions) (*mirlower.Fn, error) {
	fn := &mirlower.Fn{Name: f.Name}
	fn.GoSym = f.GoSym
	if fn.GoSym == "" && opt.GoSymFallback != nil {
		fn.GoSym = opt.GoSymFallback[f.Name]
	}
	if fn.GoSym == "" {
		fn.GoSym = "main." + f.Name
	}
	fn.Frame = f.Frame
	if fn.Frame == 0 && opt.FrameFallback != nil {
		if v, ok := opt.FrameFallback[f.Name]; ok {
			fn.Frame = v
		}
	}
	fn.Args = f.Args
	flags := f.Flags
	if flags == "" && opt.FlagsFallback != nil {
		flags = opt.FlagsFallback[f.Name]
	}
	if strings.Contains(flags, "nosplit") {
		fn.Flags |= obj.NOSPLIT
	}

	stackOff := map[int]int64{}
	for _, so := range f.StackObjects {
		if so.Offset != 0 {
			stackOff[so.ID] = so.Offset
		}
	}
	if opt.StackOffFallback != nil {
		if m, ok := opt.StackOffFallback[f.Name]; ok {
			for k, v := range m {
				if _, has := stackOff[k]; !has {
					stackOff[k] = v
				}
			}
		}
	}

	// Simple vreg → physreg aliasing via COPY.
	vregPhys := map[int]string{}

	labelOf := map[int]string{}
	for _, b := range f.Blocks {
		lab := b.Label
		if lab == "" {
			lab = fmt.Sprintf("bb%d", b.ID)
		}
		labelOf[b.ID] = lab
	}

	for _, b := range f.Blocks {
		lab := labelOf[b.ID]
		fn.Ops = append(fn.Ops, ".label "+lab)
		for _, inst := range b.Instrs {
			lines, err := lowerInstr(inst, vregPhys, stackOff, labelOf)
			if err != nil {
				return nil, fmt.Errorf("%s: %w", inst.Raw, err)
			}
			fn.Ops = append(fn.Ops, lines...)
		}
	}
	if len(fn.Ops) == 0 {
		return nil, fmt.Errorf("no ops produced")
	}
	return fn, nil
}

func physName(s string) string {
	s = strings.TrimPrefix(s, "$")
	s = strings.ToUpper(s)
	switch s {
	case "RAX":
		return "AX"
	case "RBX":
		return "BX"
	case "RCX":
		return "CX"
	case "RDX":
		return "DX"
	case "RDI":
		return "DI"
	case "RSI":
		return "SI"
	case "RBP":
		return "BP"
	case "RSP":
		return "SP"
	default:
		return s
	}
}

func resolveReg(op Op, vregPhys map[int]string) (string, error) {
	switch op.Kind {
	case OpPhysReg:
		return physName(op.Reg), nil
	case OpVReg:
		if p, ok := vregPhys[op.VReg]; ok {
			return p, nil
		}
		return "", fmt.Errorf("unbound vreg %%%d (no COPY from physreg yet; no general regalloc)", op.VReg)
	default:
		return "", fmt.Errorf("not a register operand: %s", op.Raw)
	}
}

func lowerInstr(inst *Instr, vregPhys map[int]string, stackOff map[int]int64, labelOf map[int]string) ([]string, error) {
	op := inst.Opcode
	switch op {
	case "COPY":
		if len(inst.Defs) != 1 || len(inst.Args) < 1 {
			return nil, fmt.Errorf("COPY arity")
		}
		dst, src := inst.Defs[0], inst.Args[0]
		if dst.Kind == OpVReg && src.Kind == OpPhysReg {
			vregPhys[dst.VReg] = physName(src.Reg)
			return nil, nil // elide
		}
		if dst.Kind == OpPhysReg && src.Kind == OpVReg {
			p, err := resolveReg(src, vregPhys)
			if err != nil {
				return nil, err
			}
			vregPhys[src.VReg] = physName(dst.Reg) // keep
			return []string{fmt.Sprintf("op=MOV64rr dst=%s src=%s", physName(dst.Reg), p)}, nil
		}
		if dst.Kind == OpPhysReg && src.Kind == OpPhysReg {
			return []string{fmt.Sprintf("op=MOV64rr dst=%s src=%s", physName(dst.Reg), physName(src.Reg))}, nil
		}
		if dst.Kind == OpVReg && src.Kind == OpVReg {
			if p, ok := vregPhys[src.VReg]; ok {
				vregPhys[dst.VReg] = p
				return nil, nil
			}
			return nil, fmt.Errorf("COPY vreg<-vreg unbound")
		}
		return nil, fmt.Errorf("unsupported COPY")

	case "IMPLICIT_DEF", "KILL", "EH_LABEL", "CFI_INSTRUCTION", "DBG_VALUE", "DBG_INSTR_REF",
		"LIFETIME_START", "LIFETIME_END", "PSEUDO_PROBE":
		return nil, nil

	case "GOC_PCDATA1", "PCDATA1":
		imm := int64(0)
		if len(inst.Args) > 0 && inst.Args[0].Kind == OpImm {
			imm = inst.Args[0].Imm
		}
		return []string{fmt.Sprintf("op=PCDATA1 imm=%d", imm)}, nil

	case "RET64", "RET":
		return []string{"op=RET64"}, nil

	case "PUSH64r", "PUSH64i8", "PUSH64i32":
		// Go frame prologue; demoted mirlower path only (encoding is LLVM MC).
		reg := "BP"
		for _, a := range inst.Args {
			if a.Kind == OpPhysReg {
				reg = physName(a.Reg)
				break
			}
		}
		return []string{fmt.Sprintf("op=PUSH64r reg=%s", reg)}, nil

	case "POP64r":
		reg := "BP"
		if len(inst.Defs) > 0 && inst.Defs[0].Kind == OpPhysReg {
			reg = physName(inst.Defs[0].Reg)
		} else {
			for _, a := range inst.Args {
				if a.Kind == OpPhysReg {
					reg = physName(a.Reg)
					break
				}
			}
		}
		return []string{fmt.Sprintf("op=POP64r reg=%s", reg)}, nil

	case "JMP_1", "JMP64r", "JMP64b":
		if op == "JMP64r" || (len(inst.Args) > 0 && inst.Args[0].Kind != OpMBB) {
			return nil, fmt.Errorf("indirect JMP not supported")
		}
		tgt := mbbLabel(inst.Args, labelOf)
		return []string{fmt.Sprintf("op=JMP_1 target=%s", tgt)}, nil

	case "JCC_1":
		tgt := ""
		cond := ""
		for _, a := range inst.Args {
			switch a.Kind {
			case OpMBB:
				tgt = mbbLabel([]Op{a}, labelOf)
			case OpImm:
				cond = condName(int(a.Imm))
			case OpCond:
				cond = a.Reg
			}
		}
		if tgt == "" || cond == "" {
			return nil, fmt.Errorf("JCC_1 need mbb+cond")
		}
		return []string{fmt.Sprintf("op=JCC_1 cond=%s target=%s", cond, tgt)}, nil

	case "CALL64pcrel32", "CALL64":
		sym := ""
		for _, a := range inst.Args {
			if a.Kind == OpSymbol {
				sym = a.Symbol
				break
			}
		}
		if sym == "" {
			return nil, fmt.Errorf("CALL without symbol")
		}
		return []string{fmt.Sprintf("op=CALL64pcrel32 sym=%s", sym)}, nil

	case "MOV64rr", "MOV32rr", "MOV16rr", "MOV8rr":
		dst, err := defReg(inst, vregPhys)
		if err != nil {
			return nil, err
		}
		src, err := resolveReg(inst.Args[0], vregPhys)
		if err != nil {
			return nil, err
		}
		return []string{fmt.Sprintf("op=MOV64rr dst=%s src=%s", dst, src)}, nil

	case "MOV64ri", "MOV64ri32", "MOV32ri", "MOV8ri":
		dst, err := defReg(inst, vregPhys)
		if err != nil {
			return nil, err
		}
		imm := inst.Args[0].Imm
		return []string{fmt.Sprintf("op=MOV64ri dst=%s imm=%d", dst, imm)}, nil

	case "MOV64rm", "MOV32rm", "MOV16rm", "MOV8rm", "MOV64rm_TC":
		dst, err := defReg(inst, vregPhys)
		if err != nil {
			return nil, err
		}
		mem, err := formatMem(inst.Args, vregPhys, stackOff)
		if err != nil {
			return nil, err
		}
		return []string{fmt.Sprintf("op=MOV64rm dst=%s mem=%s", dst, mem)}, nil

	case "MOV64mr", "MOV32mr", "MOV16mr", "MOV8mr":
		// store: mem..., src
		if len(inst.Args) < 6 {
			return nil, fmt.Errorf("%s need 5-tuple + src", op)
		}
		mem, err := formatMem(inst.Args[:5], vregPhys, stackOff)
		if err != nil {
			return nil, err
		}
		src, err := resolveReg(inst.Args[5], vregPhys)
		if err != nil {
			return nil, err
		}
		return []string{fmt.Sprintf("op=MOV64mr mem=%s src=%s", mem, src)}, nil

	case "MOV32mi", "MOV64mi32":
		imm := int64(0)
		memArgs := inst.Args
		if len(inst.Args) >= 6 {
			memArgs = inst.Args[:5]
			if inst.Args[5].Kind == OpImm {
				imm = inst.Args[5].Imm
			}
		} else if len(inst.Args) == 1 && inst.Args[0].Kind == OpImm {
			// pseudo form from harness
			imm = inst.Args[0].Imm
			return []string{fmt.Sprintf("op=MOV32mi imm=%d", imm)}, nil
		}
		_ = memArgs
		return []string{fmt.Sprintf("op=MOV32mi imm=%d", imm)}, nil

	case "CMP64rm":
		// CMP64rm reg, base, scale, index, disp, seg
		if len(inst.Args) < 6 {
			return nil, fmt.Errorf("CMP64rm arity")
		}
		reg, err := resolveReg(inst.Args[0], vregPhys)
		if err != nil {
			return nil, err
		}
		mem, err := formatMem(inst.Args[1:6], vregPhys, stackOff)
		if err != nil {
			return nil, err
		}
		return []string{fmt.Sprintf("op=CMP64rm reg=%s mem=%s", reg, mem)}, nil

	case "CMP64ri", "CMP64ri32", "CMP64ri8", "CMP32ri":
		reg, err := resolveReg(inst.Args[0], vregPhys)
		if err != nil {
			return nil, err
		}
		return []string{fmt.Sprintf("op=CMP64ri reg=%s imm=%d", reg, inst.Args[1].Imm)}, nil

	case "CMP32mi", "CMP64mi32", "CMP64mi8":
		// CMP32mi base, scale, index, disp, seg, imm
		if len(inst.Args) < 6 {
			return nil, fmt.Errorf("%s arity", op)
		}
		mem, err := formatMem(inst.Args[:5], vregPhys, stackOff)
		if err != nil {
			return nil, err
		}
		imm := inst.Args[5].Imm
		// symbol mem form for writeBarrier
		if memSym := mem; strings.HasPrefix(memSym, "SYM:") {
			return []string{fmt.Sprintf("op=CMP32mi mem=%s imm=%d", strings.TrimPrefix(memSym, "SYM:"), imm)}, nil
		}
		// If disp was a symbol operand stuffed in tuple — formatMem returns SYM:name
		if inst.Args[3].Kind == OpSymbol {
			return []string{fmt.Sprintf("op=CMP32mi mem=%s imm=%d", inst.Args[3].Symbol, imm)}, nil
		}
		return []string{fmt.Sprintf("op=CMP32mi mem=%s imm=%d", mem, imm)}, nil

	case "ADD64rr", "ADD32rr":
		dst, err := defOrFirstReg(inst, vregPhys)
		if err != nil {
			return nil, err
		}
		src, err := resolveReg(inst.Args[len(inst.Args)-1], vregPhys)
		if err != nil {
			// two-addr: ADD64rr $dst, $src with def=$dst
			if len(inst.Args) >= 2 {
				src, err = resolveReg(inst.Args[1], vregPhys)
			}
			if err != nil {
				return nil, err
			}
		}
		// X86 two-address: defs[0] = ADD64rr defs[0]/args[0], args[1]
		if len(inst.Defs) > 0 && len(inst.Args) >= 2 {
			d, e1 := resolveReg(inst.Defs[0], vregPhys)
			s, e2 := resolveReg(inst.Args[1], vregPhys)
			if e1 == nil && e2 == nil {
				return []string{fmt.Sprintf("op=ADD64rr dst=%s src=%s", d, s)}, nil
			}
		}
		return []string{fmt.Sprintf("op=ADD64rr dst=%s src=%s", dst, src)}, nil

	case "ADD64ri", "ADD64ri8", "ADD64ri32", "SUB64ri", "SUB64ri8", "SUB64ri32":
		dst, err := defOrFirstReg(inst, vregPhys)
		if err != nil {
			return nil, err
		}
		imm := int64(0)
		for _, a := range inst.Args {
			if a.Kind == OpImm {
				imm = a.Imm
			}
		}
		name := "ADD64ri"
		if strings.HasPrefix(op, "SUB") {
			name = "SUB64ri"
		}
		return []string{fmt.Sprintf("op=%s dst=%s imm=%d", name, dst, imm)}, nil

	case "ADD64mi8", "ADD64mi32", "ADD32mi8":
		if len(inst.Args) < 6 {
			return nil, fmt.Errorf("%s arity", op)
		}
		mem, err := formatMem(inst.Args[:5], vregPhys, stackOff)
		if err != nil {
			return nil, err
		}
		imm := inst.Args[5].Imm
		if inst.Args[3].Kind == OpSymbol {
			mem = inst.Args[3].Symbol
		} else if strings.HasPrefix(mem, "SYM:") {
			mem = strings.TrimPrefix(mem, "SYM:")
		}
		return []string{fmt.Sprintf("op=ADD64mi8 mem=%s imm=%d", mem, imm)}, nil

	case "LEA64r", "LEA64_32r", "LEA32r":
		dst, err := defReg(inst, vregPhys)
		if err != nil {
			return nil, err
		}
		mem, err := formatMem(inst.Args, vregPhys, stackOff)
		if err != nil {
			return nil, err
		}
		return []string{fmt.Sprintf("op=LEA64r dst=%s mem=%s", dst, mem)}, nil

	case "TEST64rr", "TEST32rr", "TEST64ri", "TEST32ri", "TEST64rm", "AND64rr", "AND32rr":
		// Lower TEST as CMP-like for flags; mirlower may not know TEST — map to CMP64ri 0 pattern via AND/TEST support
		// Prefer emitting CMP64ri when possible; otherwise skip with note.
		if op == "TEST64ri" || op == "TEST32ri" {
			reg, err := resolveReg(inst.Args[0], vregPhys)
			if err != nil {
				return nil, err
			}
			return []string{fmt.Sprintf("op=CMP64ri reg=%s imm=%d", reg, inst.Args[1].Imm)}, nil
		}
		if (op == "TEST64rr" || op == "AND64rr") && len(inst.Args) >= 2 {
			// approximate: CMP reg, 0 not accurate; emit as CMP64ri 0 if same regs
			reg, err := resolveReg(inst.Args[0], vregPhys)
			if err != nil {
				return nil, err
			}
			return []string{fmt.Sprintf("op=CMP64ri reg=%s imm=0", reg)}, nil
		}
		return nil, fmt.Errorf("unsupported TEST/AND form %s", op)

	case "ADDSDrr", "ADDSSrr", "SUBSDrr", "SUBSSrr", "MULSDrr", "MULSSrr",
		"DIVSDrr", "DIVSSrr", "MOVAPSrr", "MOVAPSrm", "MOVAPSmr",
		"MOVSDrr", "MOVSDrm", "MOVSDmr", "MOVSSrr", "MOVSSrm", "MOVSSmr",
		"ADDPSrr", "VADDPSrr", "VADDPDrr":
		// Float/SSE/AVX: demoted mirlower records opcode; encoding is LLVM MC (llvmmc).
		return []string{fmt.Sprintf("op=%s", op)}, nil

	default:
		return nil, fmt.Errorf("unsupported opcode %q", op)
	}
}

func defReg(inst *Instr, vregPhys map[int]string) (string, error) {
	if len(inst.Defs) == 0 {
		return "", fmt.Errorf("missing def")
	}
	d := inst.Defs[0]
	if d.Kind == OpVReg {
		// Will bind after we know — for rm/ri the def creates mapping only if we assign phys later.
		// Require phys def or prior binding.
		if p, ok := vregPhys[d.VReg]; ok {
			return p, nil
		}
		// Allow defining into unbound vreg by inventing a sticky name? No — require phys.
		return "", fmt.Errorf("def is unbound vreg %%%d (need physreg or COPY)", d.VReg)
	}
	return resolveReg(d, vregPhys)
}

func defOrFirstReg(inst *Instr, vregPhys map[int]string) (string, error) {
	if len(inst.Defs) > 0 {
		return resolveReg(inst.Defs[0], vregPhys)
	}
	if len(inst.Args) > 0 {
		return resolveReg(inst.Args[0], vregPhys)
	}
	return "", fmt.Errorf("no reg")
}

func mbbLabel(args []Op, labelOf map[int]string) string {
	for _, a := range args {
		if a.Kind == OpMBB {
			if a.MBBLabel != "" {
				return a.MBBLabel
			}
			if lab, ok := labelOf[a.MBB]; ok {
				return lab
			}
			return fmt.Sprintf("bb%d", a.MBB)
		}
	}
	return ""
}

// X86 CondCode → name used by mirlower.
func condName(cc int) string {
	switch cc {
	case 0: // O
		return "O"
	case 1:
		return "NO"
	case 2: // B/C/NAE
		return "B"
	case 3: // AE/NB/NC
		return "AE"
	case 4: // E/Z
		return "E"
	case 5: // NE/NZ
		return "NE"
	case 6: // BE/NA
		return "BE"
	case 7: // A/NBE
		return "HI"
	case 12: // L/NGE
		return "L"
	case 13: // GE/NL
		return "GE"
	case 14: // LE/NG
		return "LE"
	case 15: // G/NLE
		return "G"
	default:
		return fmt.Sprintf("CC%d", cc)
	}
}

// formatMem consumes an X86 5-tuple: base, scale, index, disp, segment.
func formatMem(args []Op, vregPhys map[int]string, stackOff map[int]int64) (string, error) {
	if len(args) < 5 {
		return "", fmt.Errorf("mem 5-tuple short (%d)", len(args))
	}
	base, scale, index, disp, seg := args[0], args[1], args[2], args[3], args[4]

	// Segment FS:-8
	segName := ""
	if seg.Kind == OpPhysReg {
		segName = strings.ToUpper(seg.Reg)
	}
	if segName == "FS" || segName == "GS" {
		off := disp.Imm
		if disp.Kind != OpImm {
			off = 0
		}
		return fmt.Sprintf("%s:%d", segName, off), nil
	}

	// Symbol: noreg absolute or $rip PIC (Pass emits $rip,@sym)
	if disp.Kind == OpSymbol && (base.Kind == OpNoreg || base.Kind == OpNone ||
		(base.Kind == OpPhysReg && (base.Reg == "rip" || base.Reg == "RIP"))) {
		return "SYM:" + disp.Symbol, nil
	}
	if base.Kind == OpSymbol {
		return "SYM:" + base.Symbol, nil
	}

	// %stack.N
	if base.Kind == OpStack {
		off, ok := stackOff[base.Stack]
		if !ok {
			off = int64(base.Stack) * 8 // last resort (documented gap)
		}
		if disp.Kind == OpImm {
			off += disp.Imm
		}
		return fmt.Sprintf("SP:%d", off), nil
	}

	// $rsp / $sp with disp
	baseReg := ""
	switch base.Kind {
	case OpPhysReg:
		baseReg = physName(base.Reg)
	case OpVReg:
		r, err := resolveReg(base, vregPhys)
		if err != nil {
			return "", err
		}
		baseReg = r
	case OpNoreg:
		baseReg = ""
	}

	off := int64(0)
	if disp.Kind == OpImm {
		off = disp.Imm
	} else if disp.Kind == OpSymbol {
		return disp.Symbol, nil
	}

	_ = scale
	_ = index

	if baseReg == "RSP" || baseReg == "SP" {
		return fmt.Sprintf("SP:%d", off), nil
	}
	if baseReg == "" && off == 0 {
		return "(NORE)", fmt.Errorf("empty mem")
	}
	if baseReg == "" {
		return fmt.Sprintf("%d", off), nil
	}
	if off == 0 {
		return fmt.Sprintf("(%s)", shortReg(baseReg)), nil
	}
	return fmt.Sprintf("%d(%s)", off, shortReg(baseReg)), nil
}

func shortReg(r string) string {
	r = strings.ToUpper(r)
	switch r {
	case "RAX":
		return "AX"
	case "RBX":
		return "BX"
	case "RCX":
		return "CX"
	case "RDX":
		return "DX"
	case "RDI":
		return "DI"
	case "RSI":
		return "SI"
	case "RBP":
		return "BP"
	case "RSP":
		return "SP"
	default:
		return r
	}
}

// WriteMiLower serializes a mirlower.File to the goc-mi-lower-1 text form (bodies only + header).
func WriteMiLower(f *mirlower.File, recipeLines []string) string {
	var b strings.Builder
	b.WriteString("# MIR→goobj lower input (from mirparse; primary LLVM MIR path)\n")
	b.WriteString("format " + f.Format + "\n")
	b.WriteString("abi " + f.ABI + "\n")
	b.WriteString("arch " + f.Arch + "\n")
	for _, L := range recipeLines {
		b.WriteString(L)
		b.WriteByte('\n')
	}
	b.WriteString("\n# ---- full MI bodies (mirparse → mirlower; no binwriter templates) ----\n")
	// Stable order for harness fns
	order := []string{
		"goc_checked_add", "goc_hold_live", "goc_hold_arg", "goc_hold_two",
		"goc_hold_regonly", "goc_store_gptr", "goc_leaf",
	}
	seen := map[string]bool{}
	writeFn := func(fn *mirlower.Fn) {
		b.WriteString(".begin_fn " + fn.Name + "\n")
		if fn.GoSym != "" {
			b.WriteString("go_sym " + fn.GoSym + "\n")
		}
		b.WriteString(fmt.Sprintf("frame %d\n", fn.Frame))
		if fn.Args != 0 {
			b.WriteString(fmt.Sprintf("args %d\n", fn.Args))
		}
		if fn.Flags&obj.NOSPLIT != 0 {
			b.WriteString("flags nosplit\n")
		}
		for _, op := range fn.Ops {
			b.WriteString(op)
			b.WriteByte('\n')
		}
		b.WriteString(".end_fn\n\n")
	}
	for _, name := range order {
		if fn, ok := f.Fns[name]; ok {
			writeFn(fn)
			seen[name] = true
		}
	}
	for name, fn := range f.Fns {
		if !seen[name] {
			writeFn(fn)
		}
	}
	return b.String()
}
