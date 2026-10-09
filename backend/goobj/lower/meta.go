package lower

import (
	"encoding/json"
	"fmt"
	"os"
	"regexp"
	"sort"
	"strconv"
	"strings"
)

// FrameOf is the single-function prologue size. amd64 with no recognisable
// prologue keeps the historical 24-byte guess. arm64 reports 0.
func FrameOf(asm, fn, arch string) int {
	if arch == "" {
		arch = "amd64"
	}
	frames := map[string][]string{}
	var cur string
	var body []string
	flush := func() {
		if cur != "" {
			frames[cur] = body
		}
	}
	for _, line := range strings.Split(asm, "\n") {
		if m := asmLabelRE.FindStringSubmatch(line); m != nil {
			flush()
			cur, body = m[1], nil
			continue
		}
		if cur != "" {
			body = append(body, line)
		}
	}
	flush()
	total, inPro := 0, false
	for _, line := range frames[fn] {
		raw := line
		if arch == "arm64" {
			if i := strings.Index(raw, "//"); i >= 0 {
				raw = raw[:i]
			}
		}
		t := strings.TrimSpace(raw)
		if t == "" || strings.HasPrefix(t, ".") || strings.HasPrefix(t, "#") {
			continue
		}
		if arch == "arm64" {
			if pre := armPreRE.FindStringSubmatch(t); pre != nil && armStpRE.MatchString(t) {
				total += parseInt0(pre[1])
			} else if armSubRE.MatchString(t) {
				if m2 := armHashRE.FindStringSubmatch(t); m2 != nil {
					total += parseInt0(m2[1])
				}
			} else if armMovRE.MatchString(t) || (armStpRE.MatchString(t) && strings.Contains(t, "[sp") && !strings.Contains(t, "]!")) {
				// still inside the prologue
			} else {
				break
			}
		} else if amdPushRE.MatchString(t) {
			total += 8
		} else if amdMovRE.MatchString(t) {
			// frame pointer setup
		} else if amdSubRE.MatchString(t) && strings.Contains(t, "%rsp") {
			if m2 := amdDollarRE.FindStringSubmatch(t); m2 != nil {
				total += parseInt0(m2[1])
			}
		} else {
			break
		}
		inPro = true
	}
	if inPro {
		return total
	}
	if arch == "arm64" {
		return 0
	}
	return 24
}

func prologueDelta(lines []string, arch string) int {
	if arch == "arm64" {
		return arm64PrologueDelta(lines)
	}
	total, inPro := 0, false
	for _, line := range lines {
		t := strings.TrimSpace(line)
		if t == "" || strings.HasPrefix(t, ".") || strings.HasPrefix(t, "#") {
			continue
		}
		if amdPushRE.MatchString(t) {
			total += 8
		} else if amdMovRE.MatchString(t) {
		} else if amdSubRE.MatchString(t) && strings.Contains(t, "%rsp") {
			if m2 := amdDollarRE.FindStringSubmatch(t); m2 != nil {
				total += parseInt0(m2[1])
			}
		} else {
			break
		}
		inPro = true
	}
	if inPro {
		return total
	}
	return 0
}

func arm64PrologueDelta(lines []string) int {
	total, inPro := 0, false
	for _, line := range lines {
		t := line
		if i := strings.Index(t, "//"); i >= 0 {
			t = t[:i]
		}
		t = strings.TrimSpace(t)
		if t == "" || strings.HasPrefix(t, ".") || strings.HasPrefix(t, "#") {
			continue
		}
		if pre := armPreRE.FindStringSubmatch(t); pre != nil && armStpRE.MatchString(t) {
			total += parseInt0(pre[1])
			inPro = true
			continue
		}
		if armSubRE.MatchString(t) {
			if m2 := armHashRE.FindStringSubmatch(t); m2 != nil {
				total += parseInt0(m2[1])
			}
			inPro = true
			continue
		}
		if armMovRE.MatchString(t) {
			inPro = true
			continue
		}
		if armStpRE.MatchString(t) && strings.Contains(t, "[sp") && !strings.Contains(t, "]!") {
			inPro = true
			continue
		}
		break
	}
	if inPro {
		return total
	}
	return 0
}

var (
	asmLabelRE  = regexp.MustCompile(`^([A-Za-z_][A-Za-z0-9_.$]*):`)
	armPreRE    = regexp.MustCompile(`\[sp,\s*#-(0x[0-9a-fA-F]+|\d+)\]!`)
	armStpRE    = regexp.MustCompile(`^(stp|str[bh]?)\b`)
	armSubRE    = regexp.MustCompile(`^sub\s+sp,\s*sp,\s*#`)
	armHashRE   = regexp.MustCompile(`#(0x[0-9a-fA-F]+|\d+)`)
	armMovRE    = regexp.MustCompile(`^(?:mov\s+x29,\s*sp|add\s+x29,\s*sp,\s*#)`)
	amdPushRE   = regexp.MustCompile(`^pushq\s+%`)
	amdMovRE    = regexp.MustCompile(`^movq\s+%rsp,\s*%rbp`)
	amdSubRE    = regexp.MustCompile(`^subq\s+\$`)
	amdDollarRE = regexp.MustCompile(`\$(0x[0-9a-fA-F]+|\d+)`)

	disFnRE    = regexp.MustCompile(`^([0-9a-f]+) <([^>]+)>:`)
	disInsnRE  = regexp.MustCompile(`^\s+([0-9a-f]+):\s+(?:(?:[0-9a-f]{2} )+|[0-9a-f]{8}\s+)\s*(\S+)\s*(.*)$`)
	disRelocRE = regexp.MustCompile(`^\s+([0-9a-f]+):\s+R_(?:X86_64|AARCH64)_(\S+)\s+(\S+)`)
	disTgtRE   = regexp.MustCompile(`#\s*(0x[0-9a-f]+)\s*<([^>]+)>`)
	disArmRE   = regexp.MustCompile(`\b(0x[0-9a-f]+)\s*<([^>]+)>`)
	disJmpRE   = regexp.MustCompile(`^j[a-z]+$`)
	disAddRE   = regexp.MustCompile(`[+-]0x[0-9a-f]+$`)

	defineNameRE  = regexp.MustCompile(`(?m)^define [^@]*@([A-Za-z_][A-Za-z0-9_.$]*)`)
	internalDefRE = regexp.MustCompile(`(?m)^define\s+([^@]*)@([A-Za-z_][A-Za-z0-9_.$]*)\(`)
	sourceFileRE  = regexp.MustCompile(`(?m)^source_filename\s*=\s*"([^"]+)"`)
	tuSuffixRE    = regexp.MustCompile(`\.(c|goc|ll|bc)$`)
	sptrNodeRE    = regexp.MustCompile(`(?m)^!(\d+) = !\{!"sptr"\}\s*$`)
	frameWordRE   = regexp.MustCompile(`(?m)^!(\d+) = !\{!"goc\.frame\.words", !"([^"]+)"((?:, i64 \d+)+)\}\s*$`)
	i64RE         = regexp.MustCompile(`i64 (\d+)`)
	chunkNameRE   = regexp.MustCompile(`[^@]*@([A-Za-z_][A-Za-z0-9_.$]*)`)
	allocaRE      = regexp.MustCompile(`(?m)^\s*(%\S+) = alloca\b`)
	frameTagRE    = regexp.MustCompile(`!goc\.frame\.words !(\d+)\b`)
	storeRE       = regexp.MustCompile(`(?m)^\s*store (?:volatile )?(?:ptr )?(%\S+), ptr (%\S+),[^\n]*`)
	mirNameRE     = regexp.MustCompile(`^name:\s+(\S+)\s*$`)
	mirObjRE      = regexp.MustCompile(`^\s*- \{ id:`)
	mirSlotRE     = regexp.MustCompile(`name: ([^,]+)`)
	mirOffRE      = regexp.MustCompile(`offset: (-?\d+)`)
)

type sideRef struct {
	Callee string
	Off    int
}

type disInsn struct {
	off   int
	mn    string
	ops   string
	tgt   *int
	tsym  string
	reloc string
}

func parseInt0(s string) int {
	n, _ := strconv.ParseInt(s, 0, 64)
	return int(n)
}

func asmFrames(asm, arch string) map[string]int {
	frames := map[string]int{}
	var cur string
	var body []string
	flush := func() {
		if cur != "" {
			frames[cur] = prologueDelta(body, arch)
		}
	}
	for _, line := range strings.Split(asm, "\n") {
		if m := asmLabelRE.FindStringSubmatch(line); m != nil {
			flush()
			cur, body = m[1], nil
			continue
		}
		if cur != "" {
			body = append(body, line)
		}
	}
	flush()
	return frames
}

func parseDis(dis, arch string) (calls, branches map[string][]sideRef, err error) {
	calls = map[string][]sideRef{}
	branches = map[string][]sideRef{}
	disInsns := map[string][]disInsn{}
	type ord struct {
		addr int
		name string
	}
	var ordered []ord
	var cur string
	var curAddr int
	var insns []disInsn
	flush := func() {
		if cur != "" {
			disInsns[cur] = insns
		}
	}
	for _, line := range strings.Split(dis, "\n") {
		if m := disFnRE.FindStringSubmatch(line); m != nil {
			flush()
			cur = m[2]
			insns = nil
			curAddr = parseInt0("0x" + m[1])
			ordered = append(ordered, ord{curAddr, cur})
			if calls[cur] == nil {
				calls[cur] = []sideRef{}
			}
			if branches[cur] == nil {
				branches[cur] = []sideRef{}
			}
			continue
		}
		if cur == "" {
			continue
		}
		if mr := disRelocRE.FindStringSubmatch(line); mr != nil {
			if len(insns) > 0 {
				insns[len(insns)-1].reloc = disAddRE.ReplaceAllString(mr[3], "")
			}
			continue
		}
		mi := disInsnRE.FindStringSubmatch(line)
		if mi == nil {
			continue
		}
		mn, ops := mi[2], mi[3]
		mt := disTgtRE.FindStringSubmatch(line)
		if mt == nil && (mn == "bl" || mn == "b" || mn == "blr" || mn == "adrp" || mn == "adr" || strings.HasPrefix(mn, "b.")) {
			mt = disArmRE.FindStringSubmatch(ops)
		}
		var tgt *int
		tsym := ""
		if mt != nil {
			v := parseInt0(mt[1])
			tgt = &v
			tsym = mt[2]
		}
		insns = append(insns, disInsn{
			off:  parseInt0("0x"+mi[1]) - curAddr,
			mn:   mn,
			ops:  ops,
			tgt:  tgt,
			tsym: tsym,
		})
	}
	flush()
	ends := map[string]*int{}
	starts := map[string]int{}
	for i, o := range ordered {
		starts[o.name] = o.addr
		if i+1 < len(ordered) {
			n := ordered[i+1].addr
			ends[o.name] = &n
		}
	}
	for name, ilist := range disInsns {
		lo := starts[name]
		hi := ends[name]
		for _, in := range ilist {
			if strings.HasPrefix(in.mn, "call") || in.mn == "bl" || in.mn == "blr" {
				callee := in.reloc
				if callee == "" {
					callee = in.tsym
				}
				if callee == "" {
					callee = "*"
				}
				calls[name] = append(calls[name], sideRef{Callee: callee, Off: in.off})
				continue
			}
			if in.mn == "adrp" || in.mn == "adr" {
				if in.reloc == "" {
					return nil, nil, fmt.Errorf("realbody: %s+0x%x: baked %s has no ELF reloc; elfpack cannot re-relocate a page address", name, in.off, in.mn)
				}
				continue
			}
			direct := disJmpRE.MatchString(in.mn) || in.mn == "b"
			if in.reloc != "" {
				if direct || (strings.HasPrefix(in.mn, "lea") && (in.reloc == ".text" || hasStart(starts, in.reloc))) {
					branches[name] = append(branches[name], sideRef{Callee: in.reloc, Off: in.off})
				}
				continue
			}
			if in.tgt == nil {
				continue
			}
			tgt := *in.tgt
			if tgt > lo && (hi == nil || tgt < *hi) {
				continue
			}
			if direct || strings.HasPrefix(in.mn, "lea") {
				branches[name] = append(branches[name], sideRef{Callee: in.tsym, Off: in.off})
			} else if strings.Contains(in.ops, "(%rip)") {
				return nil, nil, fmt.Errorf("realbody: %s+0x%x: baked RIP-relative reference to %s is neither a call, jump nor lea; elfpack cannot re-relocate it", name, in.off, in.tsym)
			} else if arch == "arm64" && in.tsym != "" {
				return nil, nil, fmt.Errorf("realbody: %s+0x%x: baked reference to %s is neither a call nor a jump; elfpack cannot re-relocate it", name, in.off, in.tsym)
			}
		}
	}
	return calls, branches, nil
}

func hasStart(starts map[string]int, name string) bool {
	_, ok := starts[name]
	return ok
}

func sptrNodes(ll string) []string {
	var nodes []string
	for _, m := range sptrNodeRE.FindAllStringSubmatch(ll, -1) {
		nodes = append(nodes, m[1])
	}
	sort.Slice(nodes, func(i, j int) bool { return atoi(nodes[i]) < atoi(nodes[j]) })
	return nodes
}

func sptrRef(nodes []string) string {
	if len(nodes) == 0 {
		return ""
	}
	return `!goc\.color !(?:` + strings.Join(nodes, "|") + `)\b`
}

func hasSptrMap(ll string, nodes []string) map[string]bool {
	out := map[string]bool{}
	ref := sptrRef(nodes)
	var refRE *regexp.Regexp
	if ref != "" {
		refRE = regexp.MustCompile(ref)
	}
	parts := strings.Split(ll, "\ndefine ")
	for _, chunk := range parts[1:] {
		m := chunkNameRE.FindStringSubmatch(chunk)
		if m == nil || strings.HasPrefix(m[1], "llvm.") {
			continue
		}
		out[m[1]] = refRE != nil && refRE.MatchString(chunk)
	}
	return out
}

func mirSlots(path string) map[string][][2]string {
	out := map[string][][2]string{}
	b, err := os.ReadFile(path)
	if err != nil {
		return out
	}
	var cur string
	for _, line := range strings.Split(string(b), "\n") {
		if m := mirNameRE.FindStringSubmatch(line); m != nil {
			cur = m[1]
			out[cur] = [][2]string{}
			continue
		}
		if cur == "" || !mirObjRE.MatchString(line) {
			continue
		}
		nm := mirSlotRE.FindStringSubmatch(line)
		off := mirOffRE.FindStringSubmatch(line)
		if nm != nil && off != nil {
			out[cur] = append(out[cur], [2]string{nm[1], off[1]})
		}
	}
	return out
}

func compilerRoot(name string) bool {
	return strings.HasPrefix(name, "goc.anchor") || strings.HasPrefix(name, "goc.arganchor") || strings.HasPrefix(name, "goc.spill.root")
}

func afterIdent(s string, end int) bool {
	return end >= len(s) || !symCont(s[end])
}

func findBounded(re *regexp.Regexp, chunk, extra string) bool {
	rest := chunk
	base := 0
	for {
		loc := re.FindStringIndex(rest)
		if loc == nil {
			return false
		}
		end := base + loc[1]
		if afterIdent(chunk, end) {
			return true
		}
		// The match swallowed a longer identifier. Step one byte.
		if loc[0]+1 >= len(rest) {
			return false
		}
		base += loc[0] + 1
		rest = chunk[base:]
		_ = extra
	}
}

func sptrSlots(ll string, nodes []string, mir map[string][][2]string, csr bool) (map[string][]int, error) {
	if len(mir) == 0 {
		return map[string][]int{}, nil
	}
	ref := sptrRef(nodes)
	var valRE, ptrRE *regexp.Regexp
	if ref != "" {
		valRE = regexp.MustCompile(`(?m)^\s*(%\S+) = ([a-z]+)[^\n]*` + ref)
		ptrRE = regexp.MustCompile(`(?m)^\s*(%\S+) = [a-z]+\s+ptr\b[^\n]*` + ref)
	}
	frameWords := map[int]struct {
		name string
		offs []int
	}{}
	for _, m := range frameWordRE.FindAllStringSubmatch(ll, -1) {
		var offs []int
		for _, n := range i64RE.FindAllStringSubmatch(m[3], -1) {
			offs = append(offs, atoi(n[1]))
		}
		frameWords[atoi(m[1])] = struct {
			name string
			offs []int
		}{m[2], offs}
	}
	slots := map[string][]int{}
	var dropped []string
	ops := map[string]bool{"alloca": true, "load": true, "getelementptr": true, "bitcast": true, "select": true, "phi": true, "call": true, "inttoptr": true}
	for _, chunk := range strings.Split(ll, "\ndefine ")[1:] {
		mname := chunkNameRE.FindStringSubmatch(chunk)
		if mname == nil || strings.HasPrefix(mname[1], "llvm.") {
			continue
		}
		name := mname[1]
		sptrVals := map[string]bool{}
		if valRE != nil {
			for _, m := range valRE.FindAllStringSubmatch(chunk, -1) {
				if ops[m[2]] {
					sptrVals[m[1]] = true
				}
			}
			for _, m := range ptrRE.FindAllStringSubmatch(chunk, -1) {
				sptrVals[m[1]] = true
			}
		}
		held := map[string]bool{}
		for _, m := range allocaRE.FindAllStringSubmatch(chunk, -1) {
			held[m[1]] = true
		}
		type target struct {
			name string
			off  int
		}
		var targets []target
		seen := map[target]bool{}
		add := func(t target) {
			if !seen[t] {
				seen[t] = true
				targets = append(targets, t)
			}
		}
		for _, tagged := range frameTagRE.FindAllStringSubmatch(chunk, -1) {
			word, ok := frameWords[atoi(tagged[1])]
			if !ok {
				return nil, fmt.Errorf("realbody: FATAL unknown frame-word annotation %s in %s", tagged[1], name)
			}
			if compilerRoot(word.name) {
				continue
			}
			for _, offset := range word.offs {
				add(target{word.name, offset})
			}
		}
		for _, m := range storeRE.FindAllStringSubmatch(chunk, -1) {
			slot := strings.TrimPrefix(m[2], "%")
			if !strings.Contains(m[0], "!goc.frame.words") && sptrVals[m[1]] && held[m[2]] && !compilerRoot(slot) {
				add(target{slot, 0})
			}
		}
		if len(targets) == 0 {
			continue
		}
		obj := map[string]int{}
		for _, pair := range mir[name] {
			obj[pair[0]], _ = strconv.Atoi(pair[1])
		}
		offSet := map[int]bool{}
		for _, t := range targets {
			mirOff, ok := obj[t.name]
			if !ok {
				live := false
				loadRE := regexp.MustCompile(`load\s+[^,\n]+,\s*ptr\s+%` + regexp.QuoteMeta(t.name))
				if findBounded(loadRE, chunk, "") {
					live = true
				}
				if !live {
					gepRE := regexp.MustCompile(`(?m)^\s*(%\S+) = getelementptr[^\n]*%` + regexp.QuoteMeta(t.name))
					rest := chunk
					base := 0
					for {
						loc := gepRE.FindStringSubmatchIndex(rest)
						if loc == nil {
							break
						}
						// group 1 is the SSA result. The %name is at the end of the match.
						end := base + loc[1]
						if afterIdent(chunk, end) {
							ssa := rest[loc[2]:loc[3]]
							use := regexp.MustCompile(`load\s+[^,\n]+,\s*ptr\s+` + regexp.QuoteMeta(ssa))
							if findBounded(use, chunk, "") {
								live = true
								break
							}
						}
						if loc[0]+1 >= len(rest) {
							break
						}
						base += loc[0] + 1
						rest = chunk[base:]
					}
				}
				if live {
					if csr {
						dropped = append(dropped, name+":"+t.name)
						continue
					}
					return nil, fmt.Errorf("realbody: FATAL sptr alloca %s in %s has no post-PEI slot", t.name, name)
				}
				continue
			}
			off := -mirOff - 16 - t.off
			if off < 8 || off%8 != 0 {
				return nil, fmt.Errorf("realbody: FATAL sptr alloca %s+%d in %s has non-pointer BP offset %d", t.name, t.off, name, off)
			}
			offSet[off] = true
		}
		if len(offSet) > 0 {
			var offs []int
			for off := range offSet {
				offs = append(offs, off)
			}
			sort.Ints(offs)
			slots[name] = offs
		}
	}
	if len(dropped) > 0 {
		n := len(dropped)
		if n > 8 {
			dropped = dropped[:8]
		}
		fmt.Fprintf(os.Stderr, "realbody: csr-adjust dropped %d unmapped sptr allocas (first %s)\n", n, strings.Join(dropped, ", "))
	}
	return slots, nil
}

func sysvPointerRegs(a abiSet, info llFunc) ([]string, error) {
	regs := []string{"rdi", "rsi", "rdx", "rcx", "r8", "r9"}
	if a.arch == "arm64" {
		regs = nil
		for i := 0; i < 8; i++ {
			regs = append(regs, fmt.Sprintf("x%d", i))
		}
	}
	used, sseUsed := 0, 0
	var ptrs []string
	intTy := regexp.MustCompile(`^i(?:1|8|16|32|64)$`)
	for i, ty := range info.Params {
		attrs := info.ParamABI[i]
		if containsStr(attrs, "byval") {
			continue
		}
		if len(attrs) > 0 {
			for _, attr := range attrs {
				if attr != "sret" {
					return nil, fmt.Errorf("realbody: FATAL unmodelled SysV entry attribute in %s: %s", info.Name, attrs)
				}
			}
		}
		switch {
		case ty == "ptr" || intTy.MatchString(ty):
			if used < len(regs) {
				if ty == "ptr" {
					ptrs = append(ptrs, regs[used])
				}
				used++
			}
		case ty == "i128":
			if used <= len(regs)-2 {
				used += 2
			}
		case ty == "float" || ty == "double":
			if sseUsed < 8 {
				sseUsed++
			}
		default:
			aggregate, reason := aggregateLayout(ty, "parameter", false)
			if aggregate == nil || reason != "" {
				return nil, fmt.Errorf("realbody: FATAL unmodelled SysV entry type in %s: %s (%s)", info.Name, ty, reason)
			}
			gprNeed := 0
			for _, field := range aggregate.Fields {
				if field.Bank == "int" {
					gprNeed++
				}
			}
			sseNeed := len(aggregate.Fields) - gprNeed
			if aggregate.Size <= 16 && used+gprNeed <= len(regs) && sseUsed+sseNeed <= 8 {
				for _, field := range aggregate.Fields {
					if field.Bank == "int" {
						if field.Type == "ptr" {
							ptrs = append(ptrs, regs[used])
						}
						used++
					} else {
						sseUsed++
					}
				}
			}
		}
	}
	if ptrs == nil {
		ptrs = []string{}
	}
	return ptrs, nil
}

func containsStr(ss []string, want string) bool {
	for _, s := range ss {
		if s == want {
			return true
		}
	}
	return false
}

type metaCall struct {
	Callee        string `json:"callee"`
	Off           int    `json:"off"`
	StackmapIndex int    `json:"stackmap_index"`
}

type metaBranch struct {
	Callee string `json:"callee"`
	Off    int    `json:"off"`
}

type metaSpill struct {
	Reg  string `json:"reg"`
	Off  int    `json:"off"`
	Size int    `json:"size"`
	Ptr  bool   `json:"ptr"`
}

type metaEntry struct {
	MIRName         string       `json:"mir_name"`
	GoSym           string       `json:"go_sym"`
	Frame           int          `json:"frame"`
	Flags           string       `json:"flags"`
	Encoding        string       `json:"encoding"`
	Calls           []metaCall   `json:"calls"`
	Branches        []metaBranch `json:"branches"`
	HasSptr         bool         `json:"has_sptr"`
	SptrSlots       []int        `json:"sptr_slots"`
	SysvPointerRegs []string     `json:"sysv_pointer_regs"`
	ABI             string       `json:"abi,omitempty"`
	ArgSpills       []metaSpill  `json:"go_arg_spills,omitempty"`
	ArgArea         *int         `json:"go_arg_area,omitempty"`
	StackPtrArgs    []int        `json:"go_stack_ptr_args,omitempty"`
}

type metaCanon struct {
	Mode         string   `json:"mode"`
	Transforms   []string `json:"transforms"`
	CfgRewrite   bool     `json:"cfg_rewrite"`
	FrameInject  bool     `json:"frame_inject"`
	DialectStrip bool     `json:"dialect_strip"`
}

type metaDoc struct {
	Producer   string      `json:"producer"`
	Pipeline   string      `json:"pipeline"`
	NotSource  string      `json:"not_source"`
	MapsStatus string      `json:"maps_status"`
	GoABI      string      `json:"goabi"`
	TU         string      `json:"tu"`
	ABI        string      `json:"abi"`
	Functions  []metaEntry `json:"functions"`
	Mircanon   metaCanon   `json:"mircanon"`
}

func nzCalls(in []sideRef) []metaCall {
	out := make([]metaCall, 0, len(in))
	for _, c := range in {
		out = append(out, metaCall{Callee: c.Callee, Off: c.Off, StackmapIndex: -1})
	}
	return out
}

func nzBranches(in []sideRef) []metaBranch {
	out := make([]metaBranch, 0, len(in))
	for _, b := range in {
		out = append(out, metaBranch{Callee: b.Callee, Off: b.Off})
	}
	return out
}

func nzInts(in []int) []int {
	if in == nil {
		return []int{}
	}
	return in
}

func nzStrs(in []string) []string {
	if in == nil {
		return []string{}
	}
	return in
}

func goArgsLayout(info abiSide) ([]metaSpill, []int, int, error) {
	off := info.StackArgsSize
	var spills []metaSpill
	var stackPtrs []int
	sizes := map[string]int{"ptr": 8, "float": 4, "double": 8, "i1": 1, "i8": 1, "i16": 2, "i32": 4, "i64": 8}
	for _, param := range info.Params {
		loc := param.Location
		if loc.Kind != "register" {
			if param.Type == "ptr" {
				stackPtrs = append(stackPtrs, loc.Off)
			}
			continue
		}
		size, ok := sizes[param.Type]
		if !ok {
			return nil, nil, 0, fmt.Errorf("realbody: FATAL Go argument type %s has no spill size", param.Type)
		}
		off = (off + size - 1) & -size
		spills = append(spills, metaSpill{Reg: loc.Reg, Off: 8 + off, Size: size, Ptr: param.Type == "ptr"})
		off += size
	}
	if spills == nil {
		spills = []metaSpill{}
	}
	if stackPtrs == nil {
		stackPtrs = []int{}
	}
	return spills, stackPtrs, (off + 7) & -8, nil
}

// WriteMeta builds the multi-function elfpack sidecar.
func WriteMeta(llPath, asmPath, disPath, outPath, symPrefix, goabiPath, abi0Arg, mirPath, arch string, noNosplit bool) error {
	if arch == "" {
		arch = "amd64"
	}
	a, err := abiFor(arch)
	if err != nil {
		return err
	}
	llb, err := os.ReadFile(llPath)
	if err != nil {
		return err
	}
	ll := string(llb)
	asmb, err := os.ReadFile(asmPath)
	if err != nil {
		return err
	}
	disb, err := os.ReadFile(disPath)
	if err != nil {
		return err
	}
	abi0 := abi0Arg == "1"
	var funcs []string
	for _, m := range defineNameRE.FindAllStringSubmatch(ll, -1) {
		if !strings.HasPrefix(m[1], "llvm.") {
			funcs = append(funcs, m[1])
		}
	}
	internal := map[string]bool{}
	for _, m := range internalDefRE.FindAllStringSubmatch(ll, -1) {
		if internalRE.MatchString(m[1]) {
			internal[m[2]] = true
		}
	}
	tu := "tu"
	if sm := sourceFileRE.FindStringSubmatch(ll); sm != nil {
		base := sm[1]
		if i := strings.LastIndex(base, "/"); i >= 0 {
			base = base[i+1:]
		}
		tu = regexp.MustCompile(`[^A-Za-z0-9_]`).ReplaceAllString(base, "_")
		tu = tuSuffixRE.ReplaceAllString(tu, "")
	}
	goabi := map[string]goFuncJSON{}
	var goabiOrder []string
	if b, err := os.ReadFile(goabiPath); err == nil && len(bytesTrim(b)) > 0 {
		var gj goABIFile
		if json.Unmarshal(b, &gj) == nil {
			for _, f := range gj.Functions {
				if _, ok := goabi[f.Name]; !ok {
					goabiOrder = append(goabiOrder, f.Name)
				}
				goabi[f.Name] = f
			}
		}
	}
	frames := asmFrames(string(asmb), arch)
	calls, branches, err := parseDis(string(disb), arch)
	if err != nil {
		return err
	}
	nodes := sptrNodes(ll)
	has := hasSptrMap(ll, nodes)
	var mir map[string][][2]string
	if mirPath != "" {
		if st, err := os.Stat(mirPath); err == nil && !st.IsDir() {
			mir = mirSlots(mirPath)
		}
	}
	csr := os.Getenv("GOC_CSR_ADJUST") == "1"
	slots := map[string][]int{}
	if len(mir) > 0 {
		slots, err = sptrSlots(ll, nodes, mir, csr)
		if err != nil {
			return err
		}
	}
	defs := parseFunctions(ll, "define")
	sysv := map[string][]string{}
	for _, info := range defs {
		ptrs, err := sysvPointerRegs(a, info)
		if err != nil {
			return err
		}
		sysv[info.Name] = ptrs
	}
	flags := "nosplit"
	if noNosplit {
		flags = "noframe"
	}
	cABI := ""
	if len(goabi) > 0 {
		cABI = "ABI0"
	}
	var mfns []metaEntry
	arm64Thunk := arch == "arm64"
	for _, name := range goabiOrder {
		info := goabi[name]
		outgoing := info.Outgoing
		thunkFrame := 8 + outgoing
		if arm64Thunk {
			thunkFrame = 16 + outgoing
		}
		spills, stackPtrs, argArea, err := goArgsLayout(info.ABI.Go)
		if err != nil {
			return err
		}
		area := argArea
		var slotOffs []int
		for _, p := range info.ABI.SysV.Params {
			if p.Location.Kind == "stack" && p.Type == "ptr" {
				slotOffs = append(slotOffs, outgoing-p.Location.Off)
			}
		}
		sort.Ints(slotOffs)
		mfns = append(mfns, metaEntry{
			MIRName: name, GoSym: symPrefix + "." + name, Frame: thunkFrame,
			Flags: flags, Encoding: "clang-real-isel",
			Calls: nzCalls(calls[name]), Branches: nzBranches(branches[name]),
			HasSptr: false, SptrSlots: nzInts(slotOffs), SysvPointerRegs: nzStrs(sysv[name]),
			ABI: "ABIInternal", ArgSpills: spills, ArgArea: &area, StackPtrArgs: stackPtrs,
		})
	}
	for _, f := range funcs {
		goName := symPrefix + "." + f
		if internal[f] {
			goName = symPrefix + "." + tu + "." + f
		}
		hasSp := true
		if v, ok := has[f]; ok {
			hasSp = v
		}
		abi := cABI
		if abi == "ABI0" {
			if _, ok := sysv[f]; !ok {
				return fmt.Errorf("realbody: FATAL missing C signature for %s", f)
			}
		}
		mfns = append(mfns, metaEntry{
			MIRName: f, GoSym: goName, Frame: frames[f],
			Flags: flags, Encoding: "clang-real-isel",
			Calls: nzCalls(calls[f]), Branches: nzBranches(branches[f]),
			HasSptr: hasSp, SptrSlots: nzInts(slots[f]), SysvPointerRegs: nzStrs(sysv[f]),
			ABI: abi,
		})
	}
	goabiLabel := "off"
	if len(goabi) > 0 {
		goabiLabel = "int/ptr subset thunks"
	}
	topABI := "ABIInternal"
	if abi0 {
		topABI = "ABI0"
	}
	doc := metaDoc{
		Producer:   "goc-p29-realbody-all",
		Pipeline:   "clang.c→real IR→llc ISel→elfpack (not P21 seed templates)",
		NotSource:  "p21-color-vertical seed templates",
		MapsStatus: "unavailable-real-mf: stackmap_index=-1, no pointer maps (P29 gap)",
		GoABI:      goabiLabel,
		TU:         tu,
		ABI:        topABI,
		Functions:  mfns,
		Mircanon: metaCanon{
			Mode: "identity", Transforms: []string{},
		},
	}
	if doc.Functions == nil {
		doc.Functions = []metaEntry{}
	}
	js, err := marshalIndent(doc)
	if err != nil {
		return err
	}
	if err := os.WriteFile(outPath, js, 0o644); err != nil {
		return err
	}
	framesLog := "{"
	for i, f := range mfns {
		if i > 0 {
			framesLog += ", "
		}
		framesLog += fmt.Sprintf("'%s': %d", f.MIRName, f.Frame)
	}
	framesLog += "}"
	fmt.Printf("realbody --all: %d TEXT entries; frames=%s\n", len(mfns), framesLog)
	return nil
}

func bytesTrim(b []byte) []byte {
	return []byte(strings.TrimSpace(string(b)))
}
