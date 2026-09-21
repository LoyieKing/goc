// Package mirlower: legacy MI→Prog lowering (P9–P11).
//
// P12 primary TEXT encoding is goobj/llvmmc (llc LLVM MC → elfpack), not this
// package's per-opcode switch. Kept for diagnostics / RequireFn helpers.
package mirlower

import (
	"bufio"
	"fmt"
	"os"
	"strconv"
	"strings"

	"goc.local/p5-machinepass-goobj/goobj/enc/cmd/objlib/obj"
	"goc.local/p5-machinepass-goobj/goobj/enc/cmd/objlib/obj/x86"
	"goc.local/p5-machinepass-goobj/goobj/enc/cmd/objlib/src"
)

const FormatV1 = "goc-mi-lower-1"

// File is a parsed mi_lower.txt (recipe metadata + per-fn full MI bodies).
type File struct {
	Format string
	ABI    string
	Arch   string
	Lines  []string
	Fns    map[string]*Fn
}

// Fn is one function's full MI list for lowering.
type Fn struct {
	Name  string
	GoSym string
	Frame int32
	Args  int32
	Flags int
	Ops   []string
}

func Load(path string) (*File, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	out := &File{Fns: map[string]*Fn{}}
	sc := bufio.NewScanner(f)
	var cur *Fn
	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		switch {
		case strings.HasPrefix(line, "format "):
			out.Format = strings.TrimSpace(strings.TrimPrefix(line, "format "))
		case strings.HasPrefix(line, "abi "):
			out.ABI = strings.TrimSpace(strings.TrimPrefix(line, "abi "))
		case strings.HasPrefix(line, "arch "):
			out.Arch = strings.TrimSpace(strings.TrimPrefix(line, "arch "))
		case strings.HasPrefix(line, ".begin_fn "):
			name := strings.TrimSpace(strings.TrimPrefix(line, ".begin_fn "))
			cur = &Fn{Name: name, GoSym: "main." + goSymDefault(name)}
			out.Fns[name] = cur
		case line == ".end_fn":
			cur = nil
		case cur != nil && strings.HasPrefix(line, "go_sym "):
			cur.GoSym = strings.TrimSpace(strings.TrimPrefix(line, "go_sym "))
		case cur != nil && strings.HasPrefix(line, "frame "):
			v, _ := strconv.Atoi(strings.TrimSpace(strings.TrimPrefix(line, "frame ")))
			cur.Frame = int32(v)
		case cur != nil && strings.HasPrefix(line, "args "):
			v, _ := strconv.Atoi(strings.TrimSpace(strings.TrimPrefix(line, "args ")))
			cur.Args = int32(v)
		case cur != nil && strings.HasPrefix(line, "flags "):
			fl := strings.TrimSpace(strings.TrimPrefix(line, "flags "))
			if strings.Contains(fl, "nosplit") {
				cur.Flags |= obj.NOSPLIT
			}
		case cur != nil:
			cur.Ops = append(cur.Ops, line)
			out.Lines = append(out.Lines, line)
		default:
			out.Lines = append(out.Lines, line)
		}
	}
	if err := sc.Err(); err != nil {
		return nil, err
	}
	if out.Format != FormatV1 {
		return nil, fmt.Errorf("mirlower: unsupported format %q (want %s)", out.Format, FormatV1)
	}
	return out, nil
}

func goSymDefault(fn string) string {
	switch fn {
	case "goc_checked_add":
		return "GocCheckedAdd"
	case "goc_hold_live":
		return "GocHoldLive"
	case "goc_hold_arg":
		return "GocHoldArg"
	case "goc_hold_two":
		return "GocHoldTwo"
	case "goc_hold_regonly":
		return "GocHoldRegOnly"
	case "goc_store_gptr":
		return "StoreGptrWB"
	case "goc_leaf":
		return "goc_leaf"
	default:
		return fn
	}
}

// RequireFn returns the full MI body for name or an error (no silent fallback).
func (f *File) RequireFn(name string) (*Fn, error) {
	if f == nil || f.Fns == nil {
		return nil, fmt.Errorf("mirlower: no mi_lower file loaded")
	}
	fn, ok := f.Fns[name]
	if !ok || len(fn.Ops) == 0 {
		return nil, fmt.Errorf("mirlower: missing full MI body for %s (required TEXT; no template fallback)", name)
	}
	return fn, nil
}

type progBuilder struct {
	ctxt  *obj.Link
	first *obj.Prog
	last  *obj.Prog
	pos   src.XPos
}

func (b *progBuilder) append(as obj.As) *obj.Prog {
	p := b.ctxt.NewProg()
	p.As = as
	p.Pos = b.pos
	p.Ctxt = b.ctxt
	if b.first == nil {
		b.first = p
	} else {
		b.last.Link = p
	}
	b.last = p
	return p
}

func regAddr(r int16) obj.Addr  { return obj.Addr{Type: obj.TYPE_REG, Reg: r} }
func immAddr(v int64) obj.Addr  { return obj.Addr{Type: obj.TYPE_CONST, Offset: v} }
func memRegAddr(r int16, off int64) obj.Addr {
	return obj.Addr{Type: obj.TYPE_MEM, Reg: r, Offset: off}
}
func symSB(ctxt *obj.Link, name string, off int64) obj.Addr {
	return obj.Addr{Type: obj.TYPE_MEM, Name: obj.NAME_EXTERN, Sym: ctxt.Lookup(name), Offset: off}
}

func parseReg(s string) (int16, error) {
	switch strings.ToUpper(s) {
	case "AX", "RAX":
		return x86.REG_AX, nil
	case "BX", "RBX":
		return x86.REG_BX, nil
	case "CX", "RCX":
		return x86.REG_CX, nil
	case "DX", "RDX":
		return x86.REG_DX, nil
	case "DI", "RDI":
		return x86.REG_DI, nil
	case "SI", "RSI":
		return x86.REG_SI, nil
	case "SP", "RSP":
		return x86.REG_SP, nil
	case "BP", "RBP":
		return x86.REG_BP, nil
	case "R8":
		return x86.REG_R8, nil
	case "R9":
		return x86.REG_R9, nil
	case "R10":
		return x86.REG_R10, nil
	case "R11":
		return x86.REG_R11, nil
	case "R12":
		return x86.REG_R12, nil
	case "R13":
		return x86.REG_R13, nil
	case "R14":
		return x86.REG_R14, nil
	case "R15":
		return x86.REG_R15, nil
	case "TLS":
		return x86.REG_TLS, nil
	default:
		return 0, fmt.Errorf("unknown reg %q", s)
	}
}

func kv(line string) map[string]string {
	m := map[string]string{}
	rest := line
	if strings.HasPrefix(rest, "op=") {
		parts := strings.SplitN(rest, " ", 2)
		m["op"] = strings.TrimPrefix(parts[0], "op=")
		if len(parts) == 1 {
			return m
		}
		rest = parts[1]
	}
	if i := strings.Index(rest, " #"); i >= 0 {
		rest = rest[:i]
	}
	for _, tok := range strings.Fields(rest) {
		if eq := strings.IndexByte(tok, '='); eq > 0 {
			m[tok[:eq]] = tok[eq+1:]
		}
	}
	return m
}

type branchPatch struct {
	p    *obj.Prog
	dest string
}

// LowerFn lowers a full per-function MI list to a Prog chain (no ATEXT).
func LowerFn(ctxt *obj.Link, pos src.XPos, fn *Fn) (*obj.Prog, error) {
	if fn == nil || len(fn.Ops) == 0 {
		return nil, fmt.Errorf("mirlower: empty fn")
	}
	b := &progBuilder{ctxt: ctxt, pos: pos}
	labels := map[string]*obj.Prog{}
	var patches []branchPatch

	emitTLSLoad := func(dst int16) {
		p := b.append(x86.AMOVQ)
		p.From = regAddr(x86.REG_TLS)
		p.To = regAddr(x86.REG_CX)
		p = b.append(x86.AMOVQ)
		p.From = obj.Addr{Type: obj.TYPE_MEM, Reg: x86.REG_CX, Index: x86.REG_TLS, Scale: 1, Offset: 0}
		p.To = regAddr(dst)
	}

	for _, line := range fn.Ops {
		if strings.HasPrefix(line, ".label ") {
			name := strings.TrimSpace(strings.TrimPrefix(line, ".label "))
			p := b.append(obj.ANOP)
			labels[name] = p
			continue
		}
		if !strings.HasPrefix(line, "op=") {
			continue
		}
		f := kv(line)
		op := f["op"]
		switch op {
		case "MOV64rm":
			dst, err := parseReg(f["dst"])
			if err != nil {
				return nil, err
			}
			mem := f["mem"]
			switch {
			case mem == "FS:-8" || mem == "TLS:g":
				emitTLSLoad(dst)
			case strings.HasPrefix(mem, "SP:"):
				off, _ := strconv.ParseInt(strings.TrimPrefix(mem, "SP:"), 10, 64)
				p := b.append(x86.AMOVQ)
				p.From = memRegAddr(x86.REG_SP, off)
				p.To = regAddr(dst)
			case strings.HasPrefix(mem, "(") && strings.HasSuffix(mem, ")"):
				rb, err := parseReg(strings.TrimSuffix(strings.TrimPrefix(mem, "("), ")"))
				if err != nil {
					return nil, err
				}
				p := b.append(x86.AMOVQ)
				p.From = memRegAddr(rb, 0)
				p.To = regAddr(dst)
			case strings.Contains(mem, "("):
				i := strings.IndexByte(mem, '(')
				off, _ := strconv.ParseInt(mem[:i], 10, 64)
				rb, err := parseReg(strings.TrimSuffix(mem[i+1:], ")"))
				if err != nil {
					return nil, err
				}
				p := b.append(x86.AMOVQ)
				p.From = memRegAddr(rb, off)
				p.To = regAddr(dst)
			default:
				// Symbol address load: MOVQ sym(SB), dst
				p := b.append(x86.AMOVQ)
				p.From = symSB(ctxt, mem, 0)
				p.To = regAddr(dst)
			}
		case "MOV64mr":
			src, err := parseReg(f["src"])
			if err != nil {
				return nil, err
			}
			mem := f["mem"]
			p := b.append(x86.AMOVQ)
			p.From = regAddr(src)
			switch {
			case strings.HasPrefix(mem, "SP:"):
				off, _ := strconv.ParseInt(strings.TrimPrefix(mem, "SP:"), 10, 64)
				p.To = memRegAddr(x86.REG_SP, off)
			case strings.HasPrefix(mem, "(") && strings.HasSuffix(mem, ")"):
				rb, err := parseReg(strings.TrimSuffix(strings.TrimPrefix(mem, "("), ")"))
				if err != nil {
					return nil, err
				}
				p.To = memRegAddr(rb, 0)
			case strings.Contains(mem, "("):
				i := strings.IndexByte(mem, '(')
				off, _ := strconv.ParseInt(mem[:i], 10, 64)
				rb, err := parseReg(strings.TrimSuffix(mem[i+1:], ")"))
				if err != nil {
					return nil, err
				}
				p.To = memRegAddr(rb, off)
			default:
				return nil, fmt.Errorf("mirlower: MOV64mr bad mem %q", mem)
			}
		case "MOV64rr":
			src, err := parseReg(f["src"])
			if err != nil {
				return nil, err
			}
			dst, err := parseReg(f["dst"])
			if err != nil {
				return nil, err
			}
			p := b.append(x86.AMOVQ)
			p.From = regAddr(src)
			p.To = regAddr(dst)
		case "MOV64ri":
			dst, err := parseReg(f["dst"])
			if err != nil {
				return nil, err
			}
			v, _ := strconv.ParseInt(f["imm"], 10, 64)
			p := b.append(x86.AMOVQ)
			p.From = immAddr(v)
			p.To = regAddr(dst)
		case "CMP64rm":
			r, err := parseReg(f["reg"])
			if err != nil {
				return nil, err
			}
			mem := f["mem"]
			i := strings.IndexByte(mem, '(')
			off, _ := strconv.ParseInt(mem[:i], 10, 64)
			rb, err := parseReg(strings.TrimSuffix(mem[i+1:], ")"))
			if err != nil {
				return nil, err
			}
			p := b.append(x86.ACMPQ)
			p.From = regAddr(r)
			p.To = memRegAddr(rb, off)
		case "CMP64ri":
			r, err := parseReg(f["reg"])
			if err != nil {
				return nil, err
			}
			v, _ := strconv.ParseInt(f["imm"], 10, 64)
			p := b.append(x86.ACMPQ)
			p.From = regAddr(r)
			p.To = immAddr(v)
		case "CMP32mi":
			mem := f["mem"]
			v, _ := strconv.ParseInt(f["imm"], 10, 64)
			p := b.append(x86.ACMPL)
			p.From = symSB(ctxt, mem, 0)
			p.To = immAddr(v)
		case "ADD64mi8", "ADD64mi":
			mem := f["mem"]
			v, _ := strconv.ParseInt(f["imm"], 10, 64)
			p := b.append(x86.AADDQ)
			p.From = immAddr(v)
			p.To = symSB(ctxt, mem, 0)
		case "ADD64rr":
			src, err := parseReg(f["src"])
			if err != nil {
				return nil, err
			}
			dst, err := parseReg(f["dst"])
			if err != nil {
				return nil, err
			}
			p := b.append(x86.AADDQ)
			p.From = regAddr(src)
			p.To = regAddr(dst)
		case "ADD64ri", "SUB64ri":
			dst, err := parseReg(f["dst"])
			if err != nil {
				return nil, err
			}
			v, _ := strconv.ParseInt(f["imm"], 10, 64)
			var as obj.As = x86.AADDQ
			if op == "SUB64ri" {
				as = x86.ASUBQ
			}
			p := b.append(as)
			p.From = immAddr(v)
			p.To = regAddr(dst)
		case "LEA64_32r", "LEA64r":
			// Pseudo LEA: dst = mem base+disp (SP:off or disp(reg))
			dst, err := parseReg(f["dst"])
			if err != nil {
				return nil, err
			}
			mem := f["mem"]
			p := b.append(x86.ALEAQ)
			p.To = regAddr(dst)
			switch {
			case strings.HasPrefix(mem, "SP:"):
				off, _ := strconv.ParseInt(strings.TrimPrefix(mem, "SP:"), 10, 64)
				p.From = memRegAddr(x86.REG_SP, off)
			case strings.Contains(mem, "("):
				i := strings.IndexByte(mem, '(')
				off := int64(0)
				if i > 0 {
					off, _ = strconv.ParseInt(mem[:i], 10, 64)
				}
				rb, err := parseReg(strings.TrimSuffix(mem[i+1:], ")"))
				if err != nil {
					return nil, err
				}
				p.From = memRegAddr(rb, off)
			default:
				return nil, fmt.Errorf("mirlower: LEA bad mem %q", mem)
			}
		case "CALL64pcrel32":
			p := b.append(obj.ACALL)
			p.To = symSB(ctxt, f["sym"], 0)
		case "JMP_1":
			p := b.append(obj.AJMP)
			p.To = obj.Addr{Type: obj.TYPE_BRANCH}
			patches = append(patches, branchPatch{p, f["target"]})
		case "JCC_1":
			cond := f["cond"]
			var as obj.As
			switch {
			case strings.HasPrefix(cond, "BE") || cond == "JBE" || cond == "LS":
				as = x86.AJLS
			case strings.HasPrefix(cond, "AE") || strings.HasPrefix(cond, "HI") || cond == "JHI" || cond == "JA":
				as = x86.AJHI
			case strings.HasPrefix(cond, "E") || cond == "JE" || cond == "EQ":
				as = x86.AJEQ
			case strings.HasPrefix(cond, "NE"):
				as = x86.AJNE
			default:
				return nil, fmt.Errorf("mirlower: bad cond %q", cond)
			}
			p := b.append(as)
			p.To = obj.Addr{Type: obj.TYPE_BRANCH}
			patches = append(patches, branchPatch{p, f["target"]})
		case "MOV32mi":
			v, _ := strconv.ParseInt(f["imm"], 10, 64)
			p := b.append(x86.AMOVL)
			p.From = immAddr(v)
			p.To = obj.Addr{Type: obj.TYPE_MEM, Offset: 0}
		case "PCDATA1":
			v, _ := strconv.ParseInt(f["imm"], 10, 64)
			p := b.append(obj.APCDATA)
			p.From = immAddr(1)
			p.To = immAddr(v)
		case "PUSH64r":
			r, err := parseReg(f["reg"])
			if err != nil {
				return nil, err
			}
			p := b.append(x86.APUSHQ)
			p.From = regAddr(r)
		case "POP64r":
			r, err := parseReg(f["reg"])
			if err != nil {
				return nil, err
			}
			p := b.append(x86.APOPQ)
			p.To = regAddr(r)
		case "RET64", "RET":
			b.append(obj.ARET)
		case "NOP":
			b.append(obj.ANOP)
		default:
			return nil, fmt.Errorf("mirlower: unsupported op %q in %s", op, fn.Name)
		}
	}

	for _, ph := range patches {
		tgt, ok := labels[ph.dest]
		if !ok {
			return nil, fmt.Errorf("mirlower: %s: unknown label %q", fn.Name, ph.dest)
		}
		ph.p.To.SetTarget(tgt)
	}
	if b.first == nil {
		return nil, fmt.Errorf("mirlower: %s produced no progs", fn.Name)
	}
	return b.first, nil
}

// AttachFront prepends a Prog chain before body; returns new head.
func AttachFront(head, body *obj.Prog) *obj.Prog {
	if head == nil {
		return body
	}
	if body == nil {
		return head
	}
	cur := head
	for cur.Link != nil {
		cur = cur.Link
	}
	cur.Link = body
	return head
}

// EmitFUNCDATA builds FUNCDATA Progs for args/locals maps.
func EmitFUNCDATA(ctxt *obj.Link, pos src.XPos, argsSym, localsSym string) *obj.Prog {
	b := &progBuilder{ctxt: ctxt, pos: pos}
	if argsSym != "" {
		p := b.append(obj.AFUNCDATA)
		p.From = immAddr(0)
		p.To = symSB(ctxt, argsSym, 0)
	}
	if localsSym != "" {
		p := b.append(obj.AFUNCDATA)
		p.From = immAddr(1)
		p.To = symSB(ctxt, localsSym, 0)
	}
	return b.first
}

// Limits documents remaining non-goals.
func Limits() string {
	return strings.Join([]string{
		"P12 PRIMARY encoding: goobj/llvmmc (mircanon → llc-19 MCCodeEmitter/AsmPrinter → elfpack)",
		"demoted: this package's opcode switch is NOT the harness TEXT encoding path",
		"retained: Load/RequireFn helpers + legacy binwriter path for experiments only",
		"real (P11): mirparse still available for MIR→mi_lower diagnostics",
		"policy: NOSPLIT+MIR morestack; FUNCDATA from maps via elfpack",
	}, "\n")
}

func StubNote() string { return Limits() }
