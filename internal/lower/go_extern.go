package lower

import (
	"fmt"
	"regexp"
	"sort"
	"strings"
)

var (
	goExternAnnRE = regexp.MustCompile(`goc\.attr\.go_extern:([A-Za-z0-9_./]+)`)
	annGlobRE     = regexp.MustCompile(`@llvm\.global\.annotations\s*=\s*appending\s+global\s+\[(\d+)\s+x\s+\{[^}]+\}]\s*\[(.+)\]`)
	annEntryRE    = regexp.MustCompile(`\{\s*ptr\s+@([A-Za-z_][A-Za-z0-9_.$]*)\s*,\s*ptr\s+@([A-Za-z0-9_.$]+)\s*,`)
	strConstRE    = regexp.MustCompile(`@([A-Za-z0-9_.$]+)\s*=\s*(?:private\s+)?(?:unnamed_addr\s+)?constant\s+\[[0-9]+ x i8\]\s+c"([^"]*)"`)
)

// goExtern is one C declare marked __go_extern("pkg.Func").
type goExtern struct {
	CName string
	GoSym string // package-qualified, e.g. main.gocGoClockGettime
	Fn    llFunc
	Plan  *sigPlan
	Spill int // 16-byte-aligned spill/arg area below RBP before the Go call
}

// parseGoExterns reads @llvm.global.annotations for goc.attr.go_extern:…
// and returns cName → Go symbol.
func parseGoExterns(text string) map[string]string {
	strs := map[string]string{}
	for _, m := range strConstRE.FindAllStringSubmatch(text, -1) {
		raw := m[2]
		// LLVM c"..." escapes: \00 etc.
		decoded := decodeLLVMCString(raw)
		strs[m[1]] = decoded
	}
	out := map[string]string{}
	gm := annGlobRE.FindStringSubmatch(text)
	if gm == nil {
		// Fall back: scan every annotation string use near a declare name.
		for cName, goSym := range parseGoExternsLoose(text, strs) {
			out[cName] = goSym
		}
		return out
	}
	for _, em := range annEntryRE.FindAllStringSubmatch(gm[2], -1) {
		cName, strLabel := em[1], em[2]
		s, ok := strs[strLabel]
		if !ok {
			continue
		}
		if m := goExternAnnRE.FindStringSubmatch(s); m != nil {
			out[cName] = m[1]
		}
	}
	return out
}

func parseGoExternsLoose(text string, strs map[string]string) map[string]string {
	out := map[string]string{}
	for _, s := range strs {
		if m := goExternAnnRE.FindStringSubmatch(s); m != nil {
			// Cannot bind without the annotations array; leave empty.
			_ = m
		}
	}
	// Match annotation entries anywhere (not only in the global aggregate).
	for _, em := range annEntryRE.FindAllStringSubmatch(text, -1) {
		cName, strLabel := em[1], em[2]
		s, ok := strs[strLabel]
		if !ok {
			continue
		}
		if m := goExternAnnRE.FindStringSubmatch(s); m != nil {
			out[cName] = m[1]
		}
	}
	return out
}

func decodeLLVMCString(s string) string {
	var b strings.Builder
	for i := 0; i < len(s); i++ {
		if s[i] == '\\' && i+2 < len(s) {
			var v byte
			for j := 0; j < 2; j++ {
				c := s[i+1+j]
				v <<= 4
				switch {
				case c >= '0' && c <= '9':
					v |= c - '0'
				case c >= 'a' && c <= 'f':
					v |= c - 'a' + 10
				case c >= 'A' && c <= 'F':
					v |= c - 'A' + 10
				}
			}
			if v != 0 {
				b.WriteByte(v)
			}
			i += 2
			continue
		}
		b.WriteByte(s[i])
	}
	return b.String()
}

func collectGoExterns(text string, a abiSet) ([]goExtern, []string, error) {
	m := parseGoExterns(text)
	if len(m) == 0 {
		return nil, nil, nil
	}
	byName := map[string]llFunc{}
	for _, d := range parseFunctions(text, "declare") {
		byName[d.Name] = d
	}
	var names []string
	for n := range m {
		names = append(names, n)
	}
	sort.Strings(names)
	var out []goExtern
	var skipped []string
	for _, cName := range names {
		goSym := m[cName]
		d, ok := byName[cName]
		if !ok {
			return nil, nil, fmt.Errorf("go_extern: %s annotated but not declared", cName)
		}
		plan, reason := analyzeSignature(a, d)
		if reason != "" {
			skipped = append(skipped, fmt.Sprintf("%s (%s)", cName, reason))
			continue
		}
		_, _, area, err := goArgsLayout(plan.ABI.Go)
		if err != nil {
			return nil, nil, fmt.Errorf("go_extern %s: %w", cName, err)
		}
		spill := (area + 15) &^ 15 // keep SP ≡ 8 (mod 16) after pushq %rbp
		out = append(out, goExtern{CName: cName, GoSym: goSym, Fn: d, Plan: plan, Spill: spill})
	}
	return out, skipped, nil
}

// emitGoExternStubs emits SysV → ABIInternal stubs for __go_extern declares.
func emitGoExternStubs(a abiSet, xs []goExtern, labels bool) string {
	if a.arch == "arm64" {
		return emitGoExternStubsA64(xs, labels)
	}
	lines := []string{"# C → Go ABIInternal stubs (__go_extern) — generated", "\t.text"}
	mark := func(l string, ti int) {
		if labels {
			lines = append(lines, fmt.Sprintf(".Lgocx_%s%d:", l, ti))
		}
	}
	// SysV callee-saved that Go ABIInternal may clobber. rbp is the frame
	// pointer; the rest must be saved around the Go call so ordinary C
	// callers keep the SysV contract (the old inline asm listed them as
	// clobbers instead).
	const saveExtra = 40 // rbx,r12,r13,r14,r15 — 5×8
	for ti, x := range xs {
		d := x.Fn
		if labels {
			lines = append(lines, fmt.Sprintf("\t.section\t.text.%s,\"ax\",@progbits", d.Name))
		}
		// Spill for Go args, plus room to save SysV callee-saved regs.
		// Keep (spill+saveExtra) ≡ 0 (mod 16) so after pushq %rbp (SP≡8)
		// SP≡8 before call → Go entry SP≡0.
		frame := x.Spill + saveExtra
		if frame%16 != 0 {
			frame = (frame + 15) &^ 15
		}
		lines = append(lines,
			fmt.Sprintf("\t.globl\t%s", d.Name),
			fmt.Sprintf("\t.type\t%s,@function", d.Name),
			d.Name+":",
			"\tpushq\t%rbp",
		)
		mark("p", ti)
		lines = append(lines, "\tmovq\t%rsp, %rbp")
		if frame > 0 {
			lines = append(lines, fmt.Sprintf("\tsubq\t$%d, %%rsp", frame))
			mark("a", ti)
		}
		// Save SysV callee-saved below the Go spill region.
		off := x.Spill
		for _, reg := range []string{"rbx", "r12", "r13", "r14", "r15"} {
			lines = append(lines, fmt.Sprintf("\tmovq\t%%%s, %d(%%rsp)", reg, off))
			off += 8
		}
		lines = append(lines, emitSysVToGoMoves(d, x.Plan)...)
		lines = append(lines,
			"\tmovq\t%fs:-8, %r14",
			"\tpxor\t%xmm15, %xmm15",
		)
		if labels {
			lines = append(lines, fmt.Sprintf(".Lgocx_c%d:", ti))
		}
		lines = append(lines, "\tcall\t"+x.GoSym+".goabi")
		if labels {
			lines = append(lines, fmt.Sprintf(".Lgocx_r%d:", ti))
		}
		// Capture Go results before restoring callee-saved (rbx may hold a result).
		var resultLines []string
		for _, mv := range x.Plan.ResultMoves {
			src, dst := mv[1], mv[0] // Go → SysV
			resultLines = append(resultLines, fmt.Sprintf("\tmovq\t%%%s, %%%s", src, dst))
		}
		// If a result lives in rbx and we need rbx→rdx, do that before restore.
		lines = append(lines, resultLines...)
		// Restore SysV callee-saved. Skip overwriting a SysV result reg that
		// we just placed: rdx is caller-saved; rbx is callee-saved and is not
		// a SysV integer return reg (rax/rdx), so always restore rbx.
		off = x.Spill
		for _, reg := range []string{"rbx", "r12", "r13", "r14", "r15"} {
			lines = append(lines, fmt.Sprintf("\tmovq\t%d(%%rsp), %%%s", off, reg))
			off += 8
		}
		lines = append(lines, "\tmovq\t%rbp, %rsp")
		mark("m", ti)
		lines = append(lines, "\tpopq\t%rbp")
		mark("o", ti)
		lines = append(lines, "\tret")
		if labels {
			lines = append(lines, fmt.Sprintf(".Lgocx_e%d:", ti))
		}
		lines = append(lines, fmt.Sprintf("\t.size\t%s, .-%s", d.Name, d.Name))
		// Record actual frame for tables (overwrite Spill used as frame below rbp).
		xs[ti].Spill = frame
	}
	lines = append(lines, "")
	return strings.Join(lines, "\n")
}

func emitSysVToGoMoves(d llFunc, plan *sigPlan) []string {
	var lines []string
	var regMoves [][2]string
	var floatMoves []xmmMove
	var stores []string
	for i, arg := range plan.ABI.Go.Params {
		ty := arg.Type
		goLoc := arg.Location
		sysvLoc := plan.ABI.SysV.Params[i].Location
		bank, _, _ := mustScalar(ty)
		if goLoc.Kind == "stack" {
			dest := fmt.Sprintf("%d(%%rsp)", goLoc.Off)
			if sysvLoc.Kind == "register" {
				stores = append(stores, emitStackArgument(ty, sysvLoc, dest)...)
			} else {
				// SysV stack → Go stack: load via temp then store.
				src := fmt.Sprintf("%d(%%rbp)", 16+sysvLoc.Off)
				if bank == "float" {
					op := "movsd"
					if ty == "float" {
						op = "movss"
					}
					stores = append(stores,
						fmt.Sprintf("\t%s\t%s, %%xmm15", op, src),
						fmt.Sprintf("\t%s\t%%xmm15, %s", op, dest),
					)
				} else {
					stores = append(stores,
						fmt.Sprintf("\t%s\t%s, %%r12", moveMnemonic(mustSize(ty)), src),
						fmt.Sprintf("\t%s\t%%r12, %s", moveMnemonic(mustSize(ty)), dest),
					)
				}
			}
			continue
		}
		// Go register destination.
		if sysvLoc.Kind == "stack" {
			load := emitRegisterArgument(ty, sysvLoc, goLoc.Reg)
			stores = append(stores, load...)
			continue
		}
		if bank == "int" {
			if sysvLoc.Reg != goLoc.Reg {
				regMoves = append(regMoves, [2]string{sysvLoc.Reg, goLoc.Reg})
			}
		} else if sysvLoc.Reg != goLoc.Reg {
			floatMoves = append(floatMoves, xmmMove{sysvLoc.Reg, goLoc.Reg, ty})
		}
	}
	lines = append(lines, parallelGPR(regMoves)...)
	lines = append(lines, parallelXMM(floatMoves)...)
	lines = append(lines, stores...)
	return lines
}

func mustSize(ty string) int {
	info, ok := scalar(ty)
	if !ok {
		return 8
	}
	return info.size
}

func emitGoExternStubsA64(xs []goExtern, labels bool) string {
	// Host QJS path is amd64-only for now; emit a clear diagnostic stub set.
	lines := []string{"# C → Go stubs (__go_extern) arm64 — generated", "\t.text"}
	for ti, x := range xs {
		d := x.Fn
		if labels {
			lines = append(lines, fmt.Sprintf("\t.section\t.text.%s,\"ax\",%%progbits", d.Name))
		}
		lines = append(lines,
			fmt.Sprintf("\t.globl\t%s", d.Name),
			fmt.Sprintf("\t.type\t%s,%%function", d.Name),
			d.Name+":",
			"\tstp\tx29, x30, [sp, #-16]!",
			"\tmov\tx29, sp",
		)
		spill := x.Spill
		if spill < 16 {
			spill = 16
		}
		spill = (spill + 15) &^ 15
		lines = append(lines, fmt.Sprintf("\tsub\tsp, sp, #%d", spill))
		if labels {
			lines = append(lines, fmt.Sprintf(".Lgocx_a%d:", ti))
		}
		// g is in TLS; amd64 uses %fs:-8. On arm64 Go keeps g in X28.
		lines = append(lines,
			"\tmrs\tx28, TPIDR_EL0",
			"\tldr\tx28, [x28, #-8]",
		)
		if labels {
			lines = append(lines, fmt.Sprintf(".Lgocx_c%d:", ti))
		}
		lines = append(lines, "\tbl\t"+x.GoSym+".goabi")
		if labels {
			lines = append(lines, fmt.Sprintf(".Lgocx_r%d:", ti))
		}
		lines = append(lines,
			"\tmov\tsp, x29",
			"\tldp\tx29, x30, [sp], #16",
			"\tret",
		)
		if labels {
			lines = append(lines, fmt.Sprintf(".Lgocx_e%d:", ti))
		}
		lines = append(lines, fmt.Sprintf("\t.size\t%s, .-%s", d.Name, d.Name))
		_ = ti
	}
	lines = append(lines, "")
	return strings.Join(lines, "\n")
}

// emitGoExternTables writes .goc.* records for C→Go stubs (pcsp so morestack
// in the Go callee is safe). Symmetric to emitWrapperTables for Go→C thunks.
func emitGoExternTables(a abiSet, xs []goExtern) (string, error) {
	if len(xs) == 0 {
		return "", nil
	}
	if a.arch == "arm64" {
		return emitGoExternTablesA64(xs)
	}
	var b strings.Builder
	strs := map[string]int{}
	var list []string
	ref := func(s string) string {
		i, ok := strs[s]
		if !ok {
			i = len(list)
			strs[s] = i
			list = append(list, s)
		}
		return fmt.Sprintf(".Lgocx_s%d-.Lgoc_strtab", i)
	}
	rec := 0
	begin := func() string {
		l := fmt.Sprintf(".Lgocx_q%d", rec)
		rec++
		fmt.Fprintf(&b, "%s:\n\t.uleb128 %se-%sb\n%sb:\n", l, l, l, l)
		return l
	}
	end := func(l string) { fmt.Fprintf(&b, "%se:\n", l) }
	uleb := func(v any) { fmt.Fprintf(&b, "\t.uleb128 %v\n", v) }
	u8 := func(v int) { fmt.Fprintf(&b, "\t.byte %d\n", v) }

	type info struct {
		spills []argSpill
		area   int
	}
	ts := make([]info, len(xs))
	for i, x := range xs {
		spills, _, area, err := goArgsLayout(x.Plan.ABI.Go)
		if err != nil {
			return "", err
		}
		ts[i] = info{spills, area}
	}

	b.WriteString("# .goc.* records of the C → Go stubs (goc-lower go_extern)\n")
	pcv := make([]string, len(xs))
	b.WriteString("\t.section .goc.pcvalue,\"\",%progbits\n")
	for i, x := range xs {
		l := begin()
		pcv[i] = l
		at := func(lbl string, v int) {
			fmt.Fprintf(&b, "\t.uleb128 .Lgocx_%s%d-%s\n\t.sleb128 %d\n", lbl, i, x.CName, v)
		}
		uleb(1)
		u8(1) // SP_DELTA
		if x.Spill > 0 {
			uleb(5)
		} else {
			uleb(4)
		}
		uleb(0)
		fmt.Fprintf(&b, "\t.sleb128 0\n")
		at("p", 8) // after pushq %rbp
		if x.Spill > 0 {
			at("a", 8+x.Spill)
		}
		at("m", 8)
		at("o", 0)
		end(l)
	}

	gc := make([]string, len(xs))
	b.WriteString("\t.section .goc.gcmaps,\"\",%progbits\n")
	for i, x := range xs {
		t := ts[i]
		nargs := t.area / 8
		if t.area%8 != 0 {
			return "", fmt.Errorf("go_extern: %s: Go argument area %d not word aligned", x.CName, t.area)
		}
		// Locals: spill region below RBP (x.Spill bytes) — no pointer slots
		// by default (register spills are written by the Go callee into the
		// area above its return address, which is inside this frame; the
		// Go callee's own stack map covers live pointers there during its
		// execution. At the call safepoint we scan Go stack args only.)
		args := make([]byte, (nargs+7)/8)
		for _, sp := range t.spills {
			if !sp.Ptr {
				continue
			}
			// Spill Off is from Go entry SP; bit relative to arg area word 0.
			o := sp.Off - 8
			if o < 0 || o%8 != 0 || o/8 >= nargs {
				continue
			}
			args[o/8/8] |= 1 << (o / 8 % 8)
		}
		for _, p := range x.Plan.ABI.Go.Params {
			if p.Location.Kind == "stack" && p.Type == "ptr" {
				o := p.Location.Off
				if o%8 == 0 && o/8 < nargs {
					args[o/8/8] |= 1 << (o / 8 % 8)
				}
			}
		}
		nlocals := x.Spill / 8
		locals := make([]byte, (nlocals+7)/8)
		l := begin()
		gc[i] = l
		uleb(nargs)
		uleb(nlocals)
		uleb(1)
		uleb(fmt.Sprintf(".Lgocx_r%d-%s", i, x.CName))
		for _, v := range args {
			u8(int(v))
		}
		for _, v := range locals {
			u8(int(v))
		}
		uleb(0)
		end(l)
	}

	b.WriteString("\t.section .goc.funcs,\"\",%progbits\n")
	for i, x := range xs {
		t := ts[i]
		l := begin()
		uleb(ref(x.CName))
		uleb(fmt.Sprintf(".Lgocx_e%d-%s", i, x.CName))
		uleb(8 + x.Spill) // frame
		uleb(t.area)      // arg_size (Go-facing)
		u8(0)             // ABI0: SysV-callable from C
		u8(1)             // kind: ABI wrapper
		uleb(4 | 8)       // nosplit | has_frame_pointer
		u8(0)
		uleb(0)
		uleb(0)
		uleb(pcv[i] + "-.Lgoc_pcvbase+1")
		uleb(gc[i] + "-.Lgoc_gcbase+1")
		uleb(0)
		uleb(0) // sysv_ptr_regs
		uleb(0) // sptr_slots
		end(l)
	}

	b.WriteString("\t.section .goc.wrappers,\"\",%progbits\n")
	for i, x := range xs {
		t := ts[i]
		l := begin()
		uleb(ref(x.CName))
		uleb(ref(x.GoSym + ".goabi"))
		u8(2) // from SysV
		u8(1) // to ABIInternal
		uleb(len(t.spills))
		for _, sp := range t.spills {
			uleb(ref(sp.Reg))
			uleb(sp.Off)
			uleb(sp.Size)
			if sp.Ptr {
				u8(1)
			} else {
				u8(0)
			}
		}
		uleb(0) // no Go stack pointer args listed separately here
		end(l)
	}

	b.WriteString("\t.section .goc.relocs,\"\",%progbits\n")
	for i, x := range xs {
		l := begin()
		uleb(ref(x.CName))
		uleb(fmt.Sprintf(".Lgocx_r%d-4-%s", i, x.CName))
		u8(4)
		u8(1) // CALL
		uleb(ref(x.GoSym + ".goabi"))
		fmt.Fprintf(&b, "\t.sleb128 -4\n")
		u8(0)
		end(l)
	}

	b.WriteString("\t.section .goc.strtab,\"\",%progbits\n")
	for i, s := range list {
		fmt.Fprintf(&b, ".Lgocx_s%d:\n\t.asciz %q\n", i, s)
	}
	return b.String(), nil
}

func emitGoExternTablesA64(xs []goExtern) (string, error) {
	// Minimal: no table records yet for arm64 host; stubs still assemble.
	_ = xs
	return "# go_extern arm64: tables deferred (QJS host is amd64)\n", nil
}
