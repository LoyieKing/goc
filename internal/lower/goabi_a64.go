package lower

import (
	"fmt"
	"strings"
)

func arm64IntName(reg string, size int) string {
	n := reg[1:]
	if size == 8 {
		return "x" + n
	}
	return "w" + n
}

func arm64FloatName(reg, ty string) string {
	n := reg[1:]
	if ty == "float" {
		return "s" + n
	}
	return "d" + n
}

// a64LRArea is the word pair at 0(RSP) below an arm64 call's stack
// arguments: the caller's LR copy Go unwinds through, and a scratch word.
// goc-llc places C stack arguments and reads incoming ones 16 bytes up.
const a64LRArea = 16

// outArea is the outgoing area of a thunk: the C stack arguments, plus the
// LR area on arm64 when there are any (with none, the frame's own 0(RSP)).
func outArea(a64 bool, outgoing int) int {
	if a64 && outgoing > 0 {
		return outgoing + a64LRArea
	}
	return outgoing
}

func arm64Mem(base string, offset int) string { return fmt.Sprintf("[%s, #%d]", base, offset) }

type a64gpr struct {
	src, dst string
	size     int
}

func parallelArm64GPR(moves []a64gpr) []string {
	var pending []a64gpr
	for _, m := range moves {
		if m.src != m.dst {
			pending = append(pending, m)
		}
	}
	var output []string
	for len(pending) > 0 {
		sources := map[string]bool{}
		for _, m := range pending {
			sources[m.src] = true
		}
		ready := -1
		for i, m := range pending {
			if !sources[m.dst] {
				ready = i
				break
			}
		}
		if ready >= 0 {
			m := pending[ready]
			pending = append(pending[:ready], pending[ready+1:]...)
			output = append(output, fmt.Sprintf("\tmov\t%s, %s", arm64IntName(m.dst, m.size), arm64IntName(m.src, m.size)))
			continue
		}
		m := pending[0]
		output = append(output, fmt.Sprintf("\tmov\t%s, %s", arm64IntName("x16", m.size), arm64IntName(m.src, m.size)))
		for i := range pending {
			if pending[i].src == m.src {
				pending[i].src = "x16"
			}
		}
	}
	return output
}

type a64fpr struct{ src, dst, ty string }

func parallelArm64FPR(moves []a64fpr) []string {
	var pending []a64fpr
	for _, m := range moves {
		if m.src != m.dst {
			pending = append(pending, m)
		}
	}
	var output []string
	for len(pending) > 0 {
		sources := map[string]bool{}
		for _, m := range pending {
			sources[m.src] = true
		}
		ready := -1
		for i, m := range pending {
			if !sources[m.dst] {
				ready = i
				break
			}
		}
		if ready >= 0 {
			m := pending[ready]
			pending = append(pending[:ready], pending[ready+1:]...)
			output = append(output, fmt.Sprintf("\tfmov\t%s, %s", arm64FloatName(m.dst, m.ty), arm64FloatName(m.src, m.ty)))
			continue
		}
		m := pending[0]
		output = append(output, fmt.Sprintf("\tfmov\t%s, %s", arm64FloatName("d16", m.ty), arm64FloatName(m.src, m.ty)))
		for i := range pending {
			if pending[i].src == m.src {
				pending[i].src = "d16"
			}
		}
	}
	return output
}

func emitArm64StackArgument(ty string, source loc, destOff int) []string {
	info, _ := scalar(ty)
	dest := arm64Mem("sp", destOff)
	if info.bank == "float" {
		fname := "d16"
		if ty == "float" {
			fname = "s16"
		}
		if source.Kind == "register" {
			return []string{fmt.Sprintf("\tstr\t%s, %s", arm64FloatName(source.Reg, ty), dest)}
		}
		src := arm64Mem("x29", 24+source.Off)
		return []string{
			fmt.Sprintf("\tldr\t%s, %s", fname, src),
			fmt.Sprintf("\tstr\t%s, %s", fname, dest),
		}
	}
	op := map[int]string{1: "strb", 2: "strh"}[info.size]
	if op == "" {
		op = "str"
	}
	if source.Kind == "register" {
		return []string{fmt.Sprintf("\t%s\t%s, %s", op, arm64IntName(source.Reg, info.size), dest)}
	}
	load := map[int]string{1: "ldrb", 2: "ldrh"}[info.size]
	if load == "" {
		load = "ldr"
	}
	tmp := arm64IntName("x16", info.size)
	src := arm64Mem("x29", 24+source.Off)
	return []string{
		fmt.Sprintf("\t%s\t%s, %s", load, tmp, src),
		fmt.Sprintf("\t%s\t%s, %s", op, tmp, dest),
	}
}

func emitArm64RegisterArgument(ty string, source loc, dest string) []string {
	info, _ := scalar(ty)
	src := arm64Mem("x29", 24+source.Off)
	if info.bank == "float" {
		return []string{fmt.Sprintf("\tldr\t%s, %s", arm64FloatName(dest, ty), src)}
	}
	load := map[int]string{1: "ldrb", 2: "ldrh"}[info.size]
	if load == "" {
		load = "ldr"
	}
	return []string{fmt.Sprintf("\t%s\t%s, %s", load, arm64IntName(dest, info.size), src)}
}

func emitArm64Thunks(goabi []llFunc, labels bool) string {
	lines := []string{"// Go 1.24 arm64 ABIInternal -> AAPCS64 entry thunks — generated", "\t.text"}
	// mark defines a label for the .goc.* tables (it emits no bytes).
	mark := func(l string, ti int) {
		if labels {
			lines = append(lines, fmt.Sprintf(".Lgocw_%s%d:", l, ti))
		}
	}
	for ti, d := range goabi {
		if labels {
			lines = append(lines, fmt.Sprintf("\t.section\t.text.%s,\"ax\",%%progbits", d.Name))
		}
		lines = append(lines,
			fmt.Sprintf("\t.globl\t%s", d.Name),
			fmt.Sprintf("\t.type\t%s, %%function", d.Name),
			"\t.balign\t4",
			d.Name+":",
			// 32-byte frame: Go's unwinder reads this frame's return
			// address at 0(RSP); the AAPCS frame record sits above it.
			"\tsub\tsp, sp, #32",
		)
		mark("p", ti)
		lines = append(lines, "\tstp\tx29, x30, [sp, #16]", "\tadd\tx29, sp, #16", "\tstr\tx30, [sp]")
		if d.plan.Outgoing > 0 {
			// The callee's stack arguments start 16 above SP; 0(RSP) is
			// again the LR copy Go unwinds through (see goc-llc AArch64Frame).
			lines = append(lines, fmt.Sprintf("\tsub\tsp, sp, #%d", outArea(true, d.plan.Outgoing)))
			mark("a", ti)
			lines = append(lines, "\tstr\tx30, [sp]")
		}
		var regMoves []a64gpr
		var floatMoves []a64fpr
		var intLoads, floatLoads []string
		for i, arg := range d.plan.ABI.Go.Params {
			ty := arg.Type
			goLoc := arg.Location
			sysvLoc := d.plan.ABI.SysV.Params[i].Location
			if sysvLoc.Kind == "stack" {
				lines = append(lines, emitArm64StackArgument(ty, goLoc, sysvLoc.Off+a64LRArea)...)
				continue
			}
			bank, size, _ := mustScalar(ty)
			if goLoc.Kind == "stack" {
				load := emitArm64RegisterArgument(ty, goLoc, sysvLoc.Reg)
				if bank == "int" {
					intLoads = append(intLoads, load...)
				} else {
					floatLoads = append(floatLoads, load...)
				}
			} else if bank == "int" {
				regMoves = append(regMoves, a64gpr{goLoc.Reg, sysvLoc.Reg, size})
			} else if goLoc.Reg != sysvLoc.Reg {
				floatMoves = append(floatMoves, a64fpr{goLoc.Reg, sysvLoc.Reg, ty})
			}
		}
		lines = append(lines, parallelArm64GPR(regMoves)...)
		lines = append(lines, parallelArm64FPR(floatMoves)...)
		lines = append(lines, intLoads...)
		lines = append(lines, floatLoads...)
		mark("c", ti)
		lines = append(lines, "\tbl\t"+d.Impl)
		mark("r", ti)
		for _, mv := range d.plan.ResultMoves {
			if strings.ContainsAny(mv[0][:1], "dsqv") {
				lines = append(lines, fmt.Sprintf("\tfmov\t%s, %s", mv[1], mv[0]))
			} else {
				lines = append(lines, fmt.Sprintf("\tmov\t%s, %s", mv[1], mv[0]))
			}
		}
		lines = append(lines, "\tmov\tsp, x29")
		mark("m", ti)
		lines = append(lines, "\tldp\tx29, x30, [sp], #16")
		mark("o", ti)
		lines = append(lines, "\tret")
		mark("e", ti)
		lines = append(lines, fmt.Sprintf("\t.size\t%s, .-%s", d.Name, d.Name))
	}
	lines = append(lines, "")
	return strings.Join(lines, "\n")
}
