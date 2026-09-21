package mirparse

import (
	"bufio"
	"fmt"
	"os"
	"strconv"
	"strings"
)

// ParseFile reads a MIR file (YAML-ish or MF dump) from path.
func ParseFile(path string) (*Module, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	m, err := Parse(string(b))
	if err != nil {
		return nil, err
	}
	m.Source = path
	return m, nil
}

// Parse parses MIR text from s.
func Parse(s string) (*Module, error) {
	s = strings.ReplaceAll(s, "\r\n", "\n")
	if looksLikeYAMLMIR(s) {
		return parseYAMLMIR(s)
	}
	if looksLikeDump(s) {
		return parseDump(s)
	}
	// Try YAML first, then dump.
	if m, err := parseYAMLMIR(s); err == nil && len(m.Functions) > 0 {
		return m, nil
	}
	if m, err := parseDump(s); err == nil && len(m.Functions) > 0 {
		return m, nil
	}
	return nil, fmt.Errorf("mirparse: unrecognized MIR format (need YAML name:/body:| or MF dump)")
}

func looksLikeYAMLMIR(s string) bool {
	return (strings.Contains(s, "body:") && strings.Contains(s, "|")) || (strings.Contains(s, "\nname:") && strings.Contains(s, "bb."))
}

func looksLikeDump(s string) bool {
	return strings.Contains(s, "# Machine code for function") || strings.Contains(s, "Machine code for function")
}

// isBodyKey reports whether trim is a YAML literal block key for MIR body
// (LLVM MIRPrinter emits "body:             |" with padding spaces).
func isBodyKey(trim string) bool {
	if !strings.HasPrefix(trim, "body:") {
		return false
	}
	rest := strings.TrimSpace(strings.TrimPrefix(trim, "body:"))
	return rest == "|"
}


// ---------- YAML-ish MIR ----------

func parseYAMLMIR(s string) (*Module, error) {
	mod := &Module{}
	lines := strings.Split(s, "\n")
	i := 0
	for i < len(lines) {
		line := strings.TrimSpace(lines[i])
		// Skip IR block --- | ... ...
		if line == "--- |" {
			for i < len(lines) && strings.TrimSpace(lines[i]) != "..." {
				i++
			}
			i++
			continue
		}
		if line == "---" || strings.HasPrefix(line, "--- ") {
			i++
			fn, next, err := parseYAMLFunction(lines, i)
			if err != nil {
				return nil, err
			}
			if fn != nil && fn.Name != "" {
				mod.Functions = append(mod.Functions, fn)
			}
			i = next
			continue
		}
		// Bare function starting with name: (no ---)
		if strings.HasPrefix(line, "name:") {
			fn, next, err := parseYAMLFunction(lines, i)
			if err != nil {
				return nil, err
			}
			if fn != nil && fn.Name != "" {
				mod.Functions = append(mod.Functions, fn)
			}
			i = next
			continue
		}
		i++
	}
	if len(mod.Functions) == 0 {
		return nil, fmt.Errorf("mirparse: YAML MIR: no functions found")
	}
	return mod, nil
}

func parseYAMLFunction(lines []string, start int) (*Function, int, error) {
	fn := &Function{VRegClass: map[int]string{}}
	i := start
	inBody := false
	var bodyLines []string
	bodyIndent := -1

	for i < len(lines) {
		raw := lines[i]
		trim := strings.TrimSpace(raw)

		if !inBody {
			if trim == "..." || (trim == "---" && fn.Name != "") {
				break
			}
			if trim == "---" && fn.Name == "" {
				i++
				continue
			}
			// goc extensions (custom keys)
			if strings.HasPrefix(trim, "goc.go_sym:") || strings.HasPrefix(trim, "goc_go_sym:") {
				fn.GoSym = yamlStr(trim)
				i++
				continue
			}
			if strings.HasPrefix(trim, "goc.frame:") || strings.HasPrefix(trim, "goc_frame:") {
				v, _ := strconv.Atoi(yamlStr(trim))
				fn.Frame = int32(v)
				i++
				continue
			}
			if strings.HasPrefix(trim, "goc.args:") || strings.HasPrefix(trim, "goc_args:") {
				v, _ := strconv.Atoi(yamlStr(trim))
				fn.Args = int32(v)
				i++
				continue
			}
			if strings.HasPrefix(trim, "goc.flags:") || strings.HasPrefix(trim, "goc_flags:") {
				fn.Flags = yamlStr(trim)
				i++
				continue
			}
			if strings.HasPrefix(trim, "name:") {
				fn.Name = yamlStr(trim)
				i++
				continue
			}
			if strings.HasPrefix(trim, "alignment:") {
				v, _ := strconv.Atoi(yamlStr(trim))
				fn.Alignment = v
				i++
				continue
			}
			if strings.HasPrefix(trim, "tracksRegLiveness:") {
				fn.TracksLiveness = strings.Contains(yamlStr(trim), "true")
				i++
				continue
			}
			if strings.HasPrefix(trim, "stack:") || strings.HasPrefix(trim, "fixedStack:") {
				isFixed := strings.HasPrefix(trim, "fixedStack:")
				i++
				for i < len(lines) {
					t := strings.TrimSpace(lines[i])
					if t == "" || strings.HasPrefix(t, "-") {
						if strings.HasPrefix(t, "-") {
							so := parseStackYAML(t, isFixed)
							fn.StackObjects = append(fn.StackObjects, so)
							i++
							continue
						}
					}
					// next top-level key (no leading spaces beyond 0-2, not list)
					if len(lines[i]) > 0 && lines[i][0] != ' ' && lines[i][0] != '\t' && !strings.HasPrefix(t, "-") {
						break
					}
					if t != "" && !strings.HasPrefix(t, "-") && !strings.HasPrefix(t, "#") &&
						strings.Contains(t, ":") && !strings.HasPrefix(strings.TrimLeft(lines[i], " "), "-") {
						// indented continuation of previous? skip nested for simplicity
						indent := len(lines[i]) - len(strings.TrimLeft(lines[i], " "))
						if indent <= 2 && !strings.HasPrefix(t, "-") {
							break
						}
					}
					i++
				}
				continue
			}
			// Frame Objects dump-style inside YAML (rare)
			if isBodyKey(trim) {
				inBody = true
				bodyIndent = -1
				i++
				continue
			}
			i++
			continue
		}

		// in body
		if trim == "..." || trim == "---" {
			break
		}
		if raw == "" {
			bodyLines = append(bodyLines, "")
			i++
			continue
		}
		ind := len(raw) - len(strings.TrimLeft(raw, " "))
		if bodyIndent < 0 && trim != "" {
			bodyIndent = ind
		}
		if trim != "" && ind == 0 && !strings.HasPrefix(trim, "bb.") && !strings.HasPrefix(trim, ";") && !strings.HasPrefix(trim, "#") {
			// dedented out of body
			break
		}
		content := raw
		if bodyIndent > 0 && len(raw) >= bodyIndent {
			content = raw[bodyIndent:]
		} else {
			content = strings.TrimLeft(raw, " ")
		}
		bodyLines = append(bodyLines, content)
		i++
	}

	if fn.Name == "" {
		return nil, i, nil
	}
	blocks, err := parseBody(strings.Join(bodyLines, "\n"))
	if err != nil {
		return nil, i, fmt.Errorf("function %s: %w", fn.Name, err)
	}
	fn.Blocks = blocks
	return fn, i, nil
}

func yamlStr(line string) string {
	_, rest, ok := strings.Cut(line, ":")
	if !ok {
		return ""
	}
	rest = strings.TrimSpace(rest)
	rest = strings.Trim(rest, `"'`)
	return rest
}

func parseStackYAML(line string, fixed bool) StackObject {
	so := StackObject{IsFixed: fixed}
	// - { id: 0, size: 8, alignment: 8, offset: -8, ... }
	line = strings.TrimSpace(strings.TrimPrefix(strings.TrimSpace(line), "-"))
	line = strings.Trim(line, "{} ")
	for _, part := range splitYAMLMap(line) {
		kv := strings.SplitN(strings.TrimSpace(part), ":", 2)
		if len(kv) != 2 {
			continue
		}
		k := strings.TrimSpace(kv[0])
		v := strings.TrimSpace(kv[1])
		switch k {
		case "id":
			so.ID, _ = strconv.Atoi(v)
		case "size":
			so.Size, _ = strconv.Atoi(v)
		case "alignment", "align":
			so.Align, _ = strconv.Atoi(v)
		case "offset":
			so.Offset, _ = strconv.ParseInt(v, 10, 64)
			if so.Offset < 0 {
				so.Offset = -so.Offset // Go SP offset often positive from SP
			}
		}
	}
	return so
}

func splitYAMLMap(s string) []string {
	var parts []string
	depth := 0
	start := 0
	for i, c := range s {
		switch c {
		case '{', '[':
			depth++
		case '}', ']':
			depth--
		case ',':
			if depth == 0 {
				parts = append(parts, s[start:i])
				start = i + 1
			}
		}
	}
	parts = append(parts, s[start:])
	return parts
}

// ---------- MF dump format (goc.mir) ----------

func parseDump(s string) (*Module, error) {
	mod := &Module{}
	sc := bufio.NewScanner(strings.NewReader(s))
	sc.Buffer(make([]byte, 0, 64*1024), 1024*1024)
	var fn *Function
	var body strings.Builder
	inFn := false

	flush := func() error {
		if fn == nil {
			return nil
		}
		blocks, err := parseBody(body.String())
		if err != nil {
			return err
		}
		fn.Blocks = blocks
		mod.Functions = append(mod.Functions, fn)
		fn = nil
		body.Reset()
		inFn = false
		return nil
	}

	for sc.Scan() {
		line := sc.Text()
		trim := strings.TrimSpace(line)
		if strings.Contains(trim, "Machine code for function ") {
			if err := flush(); err != nil {
				return nil, err
			}
			// "# Machine code for function goc_checked_add: IsSSA, TracksLiveness"
			rest := trim
			if idx := strings.Index(rest, "Machine code for function "); idx >= 0 {
				rest = rest[idx+len("Machine code for function "):]
			}
			name := rest
			if i := strings.IndexByte(name, ':'); i >= 0 {
				name = name[:i]
			}
			name = strings.TrimSpace(name)
			fn = &Function{Name: name, VRegClass: map[int]string{}, TracksLiveness: strings.Contains(trim, "TracksLiveness")}
			inFn = true
			continue
		}
		if strings.HasPrefix(trim, "# End machine code") || strings.HasPrefix(trim, "End machine code") {
			if err := flush(); err != nil {
				return nil, err
			}
			continue
		}
		if !inFn || fn == nil {
			continue
		}
		if strings.HasPrefix(trim, "Frame Objects:") {
			continue
		}
		if strings.HasPrefix(trim, "fi#") {
			// fi#0: size=8, align=8, at location [SP+8]
			so := StackObject{}
			if _, err := fmt.Sscanf(trim, "fi#%d:", &so.ID); err == nil {
				if i := strings.Index(trim, "size="); i >= 0 {
					fmt.Sscanf(trim[i:], "size=%d", &so.Size)
				}
				if i := strings.Index(trim, "align="); i >= 0 {
					fmt.Sscanf(trim[i:], "align=%d", &so.Align)
				}
				if i := strings.Index(trim, "SP+"); i >= 0 {
					fmt.Sscanf(trim[i:], "SP+%d", &so.Offset)
				}
				fn.StackObjects = append(fn.StackObjects, so)
			}
			continue
		}
		body.WriteString(line)
		body.WriteByte('\n')
	}
	if err := flush(); err != nil {
		return nil, err
	}
	if err := sc.Err(); err != nil {
		return nil, err
	}
	if len(mod.Functions) == 0 {
		return nil, fmt.Errorf("mirparse: dump: no functions found")
	}
	return mod, nil
}

// ---------- body / MBB / instr ----------

func parseBody(body string) ([]*Block, error) {
	var blocks []*Block
	var cur *Block
	for _, line := range strings.Split(body, "\n") {
		trim := strings.TrimSpace(line)
		if trim == "" || strings.HasPrefix(trim, ";") || strings.HasPrefix(trim, "#") {
			// predecessors / successors comments
			if cur != nil && strings.HasPrefix(trim, "; predecessors:") {
				cur.Predecessors = parseBBList(trim)
			}
			continue
		}
		if strings.HasPrefix(trim, "bb.") {
			cur = parseBBHeader(trim)
			blocks = append(blocks, cur)
			continue
		}
		if cur == nil {
			// orphan line — start implicit bb.0
			cur = &Block{ID: 0, Label: "entry"}
			blocks = append(blocks, cur)
		}
		if strings.HasPrefix(trim, "liveins:") || strings.HasPrefix(trim, "successors:") {
			if strings.HasPrefix(trim, "successors:") {
				cur.Successors = parseBBList(trim)
			}
			if strings.HasPrefix(trim, "liveins:") {
				cur.LiveIns = parseLiveIns(trim)
			}
			continue
		}
		inst, err := parseInstr(trim)
		if err != nil {
			return nil, fmt.Errorf("bb.%d: %w\n  line: %s", cur.ID, err, trim)
		}
		if inst != nil {
			cur.Instrs = append(cur.Instrs, inst)
		}
	}
	return blocks, nil
}

func parseBBHeader(trim string) *Block {
	// bb.0:
	// bb.0.entry:
	// bb.1 (%ir-block.1):
	b := &Block{}
	rest := strings.TrimPrefix(trim, "bb.")
	rest = strings.TrimSuffix(rest, ":")
	rest = strings.TrimSpace(rest)
	// strip (%ir-block...)
	if i := strings.Index(rest, "("); i >= 0 {
		rest = strings.TrimSpace(rest[:i])
	}
	parts := strings.SplitN(rest, ".", 2)
	b.ID, _ = strconv.Atoi(parts[0])
	if len(parts) == 2 {
		b.Label = parts[1]
	} else {
		b.Label = fmt.Sprintf("bb%d", b.ID)
	}
	return b
}

func parseBBList(s string) []int {
	var out []int
	for _, f := range strings.FieldsFunc(s, func(r rune) bool {
		return r == ' ' || r == ',' || r == ';' || r == '%' || r == '(' || r == ')' || r == ':'
	}) {
		f = strings.TrimPrefix(f, "bb.")
		if n, err := strconv.Atoi(f); err == nil {
			out = append(out, n)
		}
	}
	return out
}

func parseLiveIns(s string) []string {
	var out []string
	s = strings.TrimPrefix(strings.TrimSpace(s), "liveins:")
	for _, f := range strings.FieldsFunc(s, func(r rune) bool { return r == ',' || r == ' ' }) {
		f = strings.TrimPrefix(f, "$")
		if f != "" {
			out = append(out, f)
		}
	}
	return out
}

func parseInstr(line string) (*Instr, error) {
	line = strings.TrimSpace(line)
	if line == "" {
		return nil, nil
	}
	// Strip trailing MMO :: (load ...)
	if i := strings.Index(line, " :: "); i >= 0 {
		line = strings.TrimSpace(line[:i])
	}
	inst := &Instr{Raw: line}

	var defPart, rest string
	if i := strings.Index(line, "="); i >= 0 {
		// careful: only first '=' that is def (not inside something)
		left := strings.TrimSpace(line[:i])
		right := strings.TrimSpace(line[i+1:])
		// defs are register-like tokens
		if isDefSide(left) {
			defPart = left
			rest = right
		} else {
			rest = line
		}
	} else {
		rest = line
	}

	if defPart != "" {
		for _, d := range splitOperands(defPart) {
			op, err := parseOperand(d)
			if err != nil {
				return nil, err
			}
			inst.Defs = append(inst.Defs, op)
		}
	}

	rest = strings.TrimSpace(rest)
	fields := splitAware(rest)
	if len(fields) == 0 {
		return nil, fmt.Errorf("empty instruction")
	}
	inst.Opcode = fields[0]
	for _, f := range fields[1:] {
		lf := strings.ToLower(f)
		if strings.HasPrefix(lf, "implicit") || lf == "killed" || lf == "dead" ||
			lf == "early-clobber" || lf == "debug-use" || strings.HasPrefix(lf, "csr_") ||
			lf == "internal" {
			inst.Traits = append(inst.Traits, f)
			continue
		}
		op, err := parseOperand(f)
		if err != nil {
			return nil, err
		}
		inst.Args = append(inst.Args, op)
	}
	return inst, nil
}

func isDefSide(s string) bool {
	s = strings.TrimSpace(s)
	if s == "" {
		return false
	}
	// $rax or %0:gr64 or %0
	for _, p := range splitOperands(s) {
		p = strings.TrimSpace(p)
		if strings.HasPrefix(p, "$") || strings.HasPrefix(p, "%") {
			continue
		}
		return false
	}
	return true
}

// splitAware splits on commas / spaces but keeps memory 5-tuples grouped by
// parsing opcode then consuming operands with parseOperand on comma-separated list.
func splitAware(s string) []string {
	// First token is opcode (no spaces inside).
	s = strings.TrimSpace(s)
	if s == "" {
		return nil
	}
	opEnd := 0
	for opEnd < len(s) && s[opEnd] != ' ' && s[opEnd] != '\t' {
		opEnd++
	}
	op := s[:opEnd]
	rest := strings.TrimSpace(s[opEnd:])
	out := []string{op}
	if rest == "" {
		return out
	}
	for _, p := range splitOperands(rest) {
		out = append(out, p)
	}
	return out
}

func splitOperands(s string) []string {
	var parts []string
	depth := 0
	start := 0
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch c {
		case '(', '[', '{':
			depth++
		case ')', ']', '}':
			if depth > 0 {
				depth--
			}
		case ',':
			if depth == 0 {
				parts = append(parts, strings.TrimSpace(s[start:i]))
				start = i + 1
			}
		}
	}
	parts = append(parts, strings.TrimSpace(s[start:]))
	var out []string
	for _, p := range parts {
		if p != "" {
			out = append(out, p)
		}
	}
	return out
}

func parseOperand(tok string) (Op, error) {
	tok = strings.TrimSpace(tok)
	op := Op{Raw: tok}
	if tok == "" {
		return op, fmt.Errorf("empty operand")
	}
	low := strings.ToLower(tok)
	if low == "$noreg" || low == "noreg" || tok == "_" {
		op.Kind = OpNoreg
		return op, nil
	}
	if strings.HasPrefix(tok, "$") {
		op.Kind = OpPhysReg
		op.Reg = strings.TrimPrefix(tok, "$")
		// strip subreg :sub_32bit etc
		if i := strings.IndexByte(op.Reg, ':'); i >= 0 {
			op.Reg = op.Reg[:i]
		}
		return op, nil
	}
	if strings.HasPrefix(tok, "%stack.") {
		op.Kind = OpStack
		fmt.Sscanf(tok, "%%stack.%d", &op.Stack)
		return op, nil
	}
	if strings.HasPrefix(tok, "%bb.") || strings.HasPrefix(tok, "bb.") {
		op.Kind = OpMBB
		t := strings.TrimPrefix(tok, "%")
		t = strings.TrimPrefix(t, "bb.")
		if i := strings.IndexByte(t, '.'); i >= 0 {
			op.MBBLabel = t[i+1:]
			t = t[:i]
		}
		op.MBB, _ = strconv.Atoi(t)
		return op, nil
	}
	if strings.HasPrefix(tok, "%") {
		op.Kind = OpVReg
		body := strings.TrimPrefix(tok, "%")
		if i := strings.IndexByte(body, ':'); i >= 0 {
			op.VClass = body[i+1:]
			body = body[:i]
		}
		n, err := strconv.Atoi(body)
		if err != nil {
			op.Kind = OpOther
			return op, nil
		}
		op.VReg = n
		return op, nil
	}
	if strings.HasPrefix(tok, "@") || strings.HasPrefix(tok, "&") {
		op.Kind = OpSymbol
		op.Symbol = strings.TrimPrefix(strings.TrimPrefix(tok, "@"), "&")
		return op, nil
	}
	if strings.HasPrefix(tok, "target-flags") {
		// target-flags(x86-plt) @foo — treat whole as other; caller may not use
		op.Kind = OpOther
		return op, nil
	}
	// integer immediate
	if n, err := strconv.ParseInt(tok, 0, 64); err == nil {
		op.Kind = OpImm
		op.Imm = n
		return op, nil
	}
	// named cond codes sometimes appear
	switch strings.ToUpper(tok) {
	case "E", "NE", "BE", "LE", "G", "GE", "L", "A", "AE", "B", "HS", "LO", "HI", "LS", "EQ":
		op.Kind = OpCond
		op.Reg = strings.ToUpper(tok) // reuse Reg field for name
		return op, nil
	}
	op.Kind = OpOther
	return op, nil
}
