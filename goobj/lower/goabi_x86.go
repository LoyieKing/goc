package lower

import (
	"fmt"
	"strings"
)

func gprView(reg string, size int) string { return gprViews[reg][size] }

func moveMnemonic(size int) string {
	return map[int]string{1: "movb", 2: "movw", 4: "movl", 8: "movq"}[size]
}

func stackSource(l loc) string { return fmt.Sprintf("%d(%%rbp)", 16+l.Off) }

func parallelGPR(moves [][2]string) []string {
	var pending [][2]string
	for _, m := range moves {
		if m[0] != m[1] {
			pending = append(pending, m)
		}
	}
	var output []string
	for len(pending) > 0 {
		sources := map[string]bool{}
		for _, m := range pending {
			sources[m[0]] = true
		}
		ready := -1
		for i, m := range pending {
			if !sources[m[1]] {
				ready = i
				break
			}
		}
		if ready >= 0 {
			src, dst := pending[ready][0], pending[ready][1]
			pending = append(pending[:ready], pending[ready+1:]...)
			output = append(output, fmt.Sprintf("\tmovq\t%%%s, %%%s", src, dst))
			continue
		}
		src := pending[0][0]
		output = append(output, fmt.Sprintf("\tmovq\t%%%s, %%r12", src))
		for i, m := range pending {
			if m[0] == src {
				pending[i][0] = "r12"
			}
		}
	}
	return output
}

type xmmMove struct{ src, dst, ty string }

func parallelXMM(moves []xmmMove) []string {
	var pending []xmmMove
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
			op := "sd"
			if m.ty == "float" {
				op = "ss"
			}
			output = append(output, fmt.Sprintf("\tmov%s\t%%%s, %%%s", op, m.src, m.dst))
			continue
		}
		m := pending[0]
		op := "sd"
		if m.ty == "float" {
			op = "ss"
		}
		output = append(output, fmt.Sprintf("\tmov%s\t%%%s, %%xmm15", op, m.src))
		for i := range pending {
			if pending[i].src == m.src {
				pending[i].src = "xmm15"
			}
		}
	}
	return output
}

func emitStackArgument(ty string, source loc, dest string) []string {
	info, _ := scalar(ty)
	if info.bank == "float" {
		op := "movsd"
		if info.size == 4 {
			op = "movss"
		}
		if source.Kind == "register" {
			return []string{fmt.Sprintf("\t%s\t%%%s, %s", op, source.Reg, dest)}
		}
		return []string{
			fmt.Sprintf("\t%s\t%s, %%xmm15", op, stackSource(source)),
			fmt.Sprintf("\t%s\t%%xmm15, %s", op, dest),
		}
	}
	op := moveMnemonic(info.size)
	if source.Kind == "register" {
		return []string{fmt.Sprintf("\t%s\t%%%s, %s", op, gprView(source.Reg, info.size), dest)}
	}
	return []string{
		fmt.Sprintf("\t%s\t%s, %%%s", op, stackSource(source), gprView("r12", info.size)),
		fmt.Sprintf("\t%s\t%%%s, %s", op, gprView("r12", info.size), dest),
	}
}

func emitRegisterArgument(ty string, source loc, dest string) []string {
	info, _ := scalar(ty)
	if info.bank == "float" {
		op := "movsd"
		if info.size == 4 {
			op = "movss"
		}
		return []string{fmt.Sprintf("\t%s\t%s, %%%s", op, stackSource(source), dest)}
	}
	return []string{fmt.Sprintf("\t%s\t%s, %%%s", moveMnemonic(info.size), stackSource(source), gprView(dest, info.size))}
}

// emitThunksL is emitThunks; with labels it also defines .Lgocw_c<i> and
// .Lgocw_r<i> around thunk i's call and .Lgocw_e<i> at its end, for the
// .goc.* wrapper tables (labels emit no bytes).
func emitThunksL(a abiSet, goabi []llFunc, labels bool) string {
	if a.arch == "arm64" {
		return emitArm64Thunks(goabi, labels)
	}
	lines := []string{"# Go 1.24 ABIInternal -> SysV entry thunks — generated", "\t.text"}
	// mark defines a label for the .goc.* tables (it emits no bytes).
	mark := func(l string, ti int) {
		if labels {
			lines = append(lines, fmt.Sprintf(".Lgocw_%s%d:", l, ti))
		}
	}
	for ti, d := range goabi {
		if labels {
			// One section per function, as goc-llc -function-sections: every
			// reference between functions then carries an ELF relocation.
			lines = append(lines, fmt.Sprintf("\t.section\t.text.%s,\"ax\",@progbits", d.Name))
		}
		lines = append(lines,
			fmt.Sprintf("\t.globl\t%s", d.Name),
			fmt.Sprintf("\t.type\t%s,@function", d.Name),
			d.Name+":",
			"\tpushq\t%rbp",
		)
		mark("p", ti)
		lines = append(lines, "\tmovq\t%rsp, %rbp")
		if d.plan.Outgoing > 0 {
			lines = append(lines, fmt.Sprintf("\tsubq\t$%d, %%rsp", d.plan.Outgoing))
			mark("a", ti)
		}
		var regMoves [][2]string
		var floatMoves []xmmMove
		var intLoads, floatLoads []string
		for i, arg := range d.plan.ABI.Go.Params {
			ty := arg.Type
			goLoc := arg.Location
			sysvLoc := d.plan.ABI.SysV.Params[i].Location
			if sysvLoc.Kind == "stack" {
				lines = append(lines, emitStackArgument(ty, goLoc, fmt.Sprintf("%d(%%rsp)", sysvLoc.Off))...)
				continue
			}
			bank, _, _ := mustScalar(ty)
			if goLoc.Kind == "stack" {
				load := emitRegisterArgument(ty, goLoc, sysvLoc.Reg)
				if bank == "int" {
					intLoads = append(intLoads, load...)
				} else {
					floatLoads = append(floatLoads, load...)
				}
			} else if bank == "int" {
				regMoves = append(regMoves, [2]string{goLoc.Reg, sysvLoc.Reg})
			} else if goLoc.Reg != sysvLoc.Reg {
				floatMoves = append(floatMoves, xmmMove{goLoc.Reg, sysvLoc.Reg, ty})
			}
		}
		lines = append(lines, parallelGPR(regMoves)...)
		lines = append(lines, parallelXMM(floatMoves)...)
		lines = append(lines, intLoads...)
		lines = append(lines, floatLoads...)
		if labels {
			lines = append(lines, fmt.Sprintf(".Lgocw_c%d:", ti))
		}
		lines = append(lines, "\tcall\t"+d.Impl)
		if labels {
			lines = append(lines, fmt.Sprintf(".Lgocw_r%d:", ti))
		}
		for _, mv := range d.plan.ResultMoves {
			lines = append(lines, fmt.Sprintf("\tmovq\t%%%s, %%%s", mv[0], mv[1]))
		}
		lines = append(lines,
			"\tpxor\t%xmm15, %xmm15",
			"\tmovq\t%rbp, %rsp")
		mark("m", ti)
		lines = append(lines, "\tpopq\t%rbp")
		mark("o", ti)
		lines = append(lines, "\tret")
		if labels {
			lines = append(lines, fmt.Sprintf(".Lgocw_e%d:", ti))
		}
		lines = append(lines, fmt.Sprintf("\t.size\t%s, .-%s", d.Name, d.Name))
	}
	lines = append(lines, "")
	return strings.Join(lines, "\n")
}
