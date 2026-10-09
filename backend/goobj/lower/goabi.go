// Package lower is the goc real-body lowering that used to run as Python
// and ripgrep: Go ABI thunks, IR fixups, and the elfpack sidecar.
package lower

import (
	"bytes"
	"encoding/json"
	"fmt"
	"os"
	"regexp"
	"sort"
	"strings"
)

var (
	nameRE      = regexp.MustCompile(`^[A-Za-z_][A-Za-z0-9_.$]*`)
	headerRE    = regexp.MustCompile(`(?m)^\s*(define|declare)\s+(.+?)@([A-Za-z_][A-Za-z0-9_.$]*)\(`)
	typeRE      = regexp.MustCompile(`^(void|ptr|float|double|i[0-9]+)\b`)
	namedTypeRE = regexp.MustCompile(`^%[-A-Za-z$._0-9]+`)
	typedPtrRE  = regexp.MustCompile(`^(?:void|float|double|i[0-9]+)(?:\s+addrspace\([0-9]+\))?\s*\*`)
	addrspaceRE = regexp.MustCompile(`^ptr\s+addrspace\([0-9]+\)`)
	nonCRE      = regexp.MustCompile(`\b(?:fastcc|coldcc|ghccc|hipecc|webkit_jscc|anyregcc|preserve_mostcc|preserve_allcc|swiftcc|cxx_fast_tlscc|tailcc|cfguard_checkcc|x86_stdcallcc|x86_fastcallcc|x86_thiscallcc|x86_vectorcallcc|x86_64_sysvcc|cc\s+[0-9]+)\b`)
	abiParamRE  = regexp.MustCompile(`\b(byval|sret|inalloca|preallocated|byref|inreg|nest|swiftself|swiftasync|swifterror)\b`)
	abiRetRE    = regexp.MustCompile(`\b(sret|inreg)\b`)
	extRE       = regexp.MustCompile(`\b(signext|zeroext|noundef)\b`)
	signRE      = regexp.MustCompile(`\b(signext|zeroext)\b`)
	intTypeRE   = regexp.MustCompile(`^i([0-9]+)$`)
	retTailRE   = regexp.MustCompile(`(%[-A-Za-z$._0-9]+|ptr\s+addrspace\([0-9]+\)|void|ptr|float|double|i[0-9]+)\s*$`)
	internalRE  = regexp.MustCompile(`\binternal\b`)
)

var intWidths = map[int]bool{1: true, 8: true, 16: true, 32: true, 64: true}

var gprViews = map[string]map[int]string{
	"rax": {1: "al", 2: "ax", 4: "eax", 8: "rax"},
	"rbx": {1: "bl", 2: "bx", 4: "ebx", 8: "rbx"},
	"rcx": {1: "cl", 2: "cx", 4: "ecx", 8: "rcx"},
	"rdx": {1: "dl", 2: "dx", 4: "edx", 8: "rdx"},
	"rsi": {1: "sil", 2: "si", 4: "esi", 8: "rsi"},
	"rdi": {1: "dil", 2: "di", 4: "edi", 8: "rdi"},
	"r8":  {1: "r8b", 2: "r8w", 4: "r8d", 8: "r8"},
	"r9":  {1: "r9b", 2: "r9w", 4: "r9d", 8: "r9"},
	"r10": {1: "r10b", 2: "r10w", 4: "r10d", 8: "r10"},
	"r11": {1: "r11b", 2: "r11w", 4: "r11d", 8: "r11"},
	"r12": {1: "r12b", 2: "r12w", 4: "r12d", 8: "r12"},
}

// abiSet is the register file for one GOC_ARCH. The Python script mutated
// module globals; this value is passed explicitly.
type abiSet struct {
	arch                               string
	goInt, goFloat, sysvInt, sysvFloat []string
	sysvIntResult, sysvFloatResult     []string
}

func abiFor(arch string) (abiSet, error) {
	if arch == "" {
		arch = "amd64"
	}
	if arch != "amd64" && arch != "arm64" {
		return abiSet{}, fmt.Errorf("GOC_ARCH must be amd64 or arm64, got %s", arch)
	}
	a := abiSet{arch: arch}
	if arch == "arm64" {
		for i := 0; i < 16; i++ {
			a.goInt = append(a.goInt, fmt.Sprintf("x%d", i))
			a.goFloat = append(a.goFloat, fmt.Sprintf("d%d", i))
		}
		for i := 0; i < 8; i++ {
			a.sysvInt = append(a.sysvInt, fmt.Sprintf("x%d", i))
			a.sysvFloat = append(a.sysvFloat, fmt.Sprintf("d%d", i))
		}
		a.sysvIntResult = []string{"x0", "x1"}
		a.sysvFloatResult = []string{"d0", "d1"}
		return a, nil
	}
	a.goInt = []string{"rax", "rbx", "rcx", "rdi", "rsi", "r8", "r9", "r10", "r11"}
	for i := 0; i < 15; i++ {
		a.goFloat = append(a.goFloat, fmt.Sprintf("xmm%d", i))
	}
	a.sysvInt = []string{"rdi", "rsi", "rdx", "rcx", "r8", "r9"}
	for i := 0; i < 8; i++ {
		a.sysvFloat = append(a.sysvFloat, fmt.Sprintf("xmm%d", i))
	}
	a.sysvIntResult = []string{"rax", "rdx"}
	a.sysvFloatResult = []string{"xmm0", "xmm1"}
	return a, nil
}

func splitTopLevel(text string) []string {
	var parts []string
	start := 0
	var stack []byte
	pairs := map[byte]byte{')': '(', ']': '[', '}': '{', '>': '<'}
	for i := 0; i < len(text); i++ {
		ch := text[i]
		switch ch {
		case '(', '[', '{', '<':
			stack = append(stack, ch)
		case ')', ']', '}', '>':
			if len(stack) > 0 && stack[len(stack)-1] == pairs[ch] {
				stack = stack[:len(stack)-1]
			}
		case ',':
			if len(stack) == 0 {
				parts = append(parts, strings.TrimSpace(text[start:i]))
				start = i + 1
			}
		}
	}
	tail := strings.TrimSpace(text[start:])
	if tail != "" {
		parts = append(parts, tail)
	}
	return parts
}

func matchingParen(text string, opening int) int {
	depth := 0
	for i := opening; i < len(text); i++ {
		switch text[i] {
		case '(':
			depth++
		case ')':
			depth--
			if depth == 0 {
				return i
			}
		}
	}
	return -1
}

func llvmTypePrefix(text string) string {
	text = strings.TrimSpace(text)
	if strings.HasPrefix(text, "<{") {
		depth := 0
		for i := 1; i < len(text); i++ {
			switch text[i] {
			case '{':
				depth++
			case '}':
				depth--
				if depth == 0 && i+1 < len(text) && text[i+1] == '>' {
					return text[:i+2]
				}
			}
		}
	}
	if strings.HasPrefix(text, "{") {
		depth := 0
		for i := 0; i < len(text); i++ {
			switch text[i] {
			case '{':
				depth++
			case '}':
				depth--
				if depth == 0 {
					return text[:i+1]
				}
			}
		}
		return ""
	}
	if m := typedPtrRE.FindString(text); m != "" {
		return strings.TrimSpace(m)
	}
	if m := addrspaceRE.FindString(text); m != "" {
		return m
	}
	if m := namedTypeRE.FindString(text); m != "" {
		return m
	}
	if m := typeRE.FindStringSubmatch(text); m != nil {
		return m[1]
	}
	return ""
}

func nonNil(ss []string) []string {
	if ss == nil {
		return []string{}
	}
	return ss
}

func parseParam(text string) (string, []string, []string) {
	ty := llvmTypePrefix(text)
	rest := text[len(ty):]
	return ty, nonNil(extRE.FindAllString(rest, -1)), nonNil(abiParamRE.FindAllString(rest, -1))
}

func parseReturn(prefix string) string {
	prefix = strings.TrimRight(prefix, " \t")
	if strings.HasSuffix(prefix, "}") || strings.HasSuffix(prefix, "}>") {
		depth := 0
		end := len(prefix) - 1
		if strings.HasSuffix(prefix, "}>") {
			end = len(prefix) - 2
		}
		for i := end; i >= 0; i-- {
			switch prefix[i] {
			case '}':
				depth++
			case '{':
				depth--
				if depth == 0 {
					if i > 0 && prefix[i-1] == '<' {
						return prefix[i-1:]
					}
					return prefix[i:]
				}
			}
		}
		return ""
	}
	if m := retTailRE.FindStringSubmatch(prefix); m != nil {
		return m[1]
	}
	return ""
}

// llFunc is one LLVM define or declare header.
type llFunc struct {
	Name        string
	Ret         string
	RetAttrs    []string
	RetABI      []string
	Params      []string
	ParamAttrs  [][]string
	ParamABI    [][]string
	Varargs     bool
	Internal    bool
	NonC        bool
	plan        *sigPlan
	ABI         *abiSides
	GoSignature string
	Outgoing    int
	Impl        string
}

func parseFunctions(text, keyword string) []llFunc {
	var functions []llFunc
	for _, m := range headerRE.FindAllStringSubmatchIndex(text, -1) {
		// submatch groups: 0 full, 1 keyword, 2 prefix, 3 name.
		kw := text[m[2]:m[3]]
		if kw != keyword {
			continue
		}
		opening := m[1] - 1 // the '(' that ended the match
		closing := matchingParen(text, opening)
		if closing < 0 {
			continue
		}
		paramsText := text[opening+1 : closing]
		prefix := text[m[4]:m[5]]
		raw := []string{}
		if strings.TrimSpace(paramsText) != "" {
			raw = splitTopLevel(paramsText)
		}
		varargs := false
		var params []string
		var paramAttrs [][]string
		var paramABI [][]string
		for _, p := range raw {
			if p == "..." {
				varargs = true
				continue
			}
			ty, attrs, abi := parseParam(p)
			params = append(params, ty)
			paramAttrs = append(paramAttrs, attrs)
			paramABI = append(paramABI, abi)
		}
		if params == nil {
			params = []string{}
		}
		if paramAttrs == nil {
			paramAttrs = [][]string{}
		}
		if paramABI == nil {
			paramABI = [][]string{}
		}
		functions = append(functions, llFunc{
			Name:       text[m[6]:m[7]],
			Ret:        parseReturn(prefix),
			RetAttrs:   nonNil(signRE.FindAllString(prefix, -1)),
			RetABI:     nonNil(abiRetRE.FindAllString(prefix, -1)),
			Params:     params,
			ParamAttrs: paramAttrs,
			ParamABI:   paramABI,
			Varargs:    varargs,
			Internal:   internalRE.MatchString(prefix),
			NonC:       nonCRE.MatchString(prefix),
		})
	}
	return functions
}

type scalarInfo struct {
	bank  string
	size  int
	align int
}

func scalar(ty string) (scalarInfo, bool) {
	switch ty {
	case "ptr":
		return scalarInfo{"int", 8, 8}, true
	case "float":
		return scalarInfo{"float", 4, 4}, true
	case "double":
		return scalarInfo{"float", 8, 8}, true
	}
	m := intTypeRE.FindStringSubmatch(ty)
	if m == nil {
		return scalarInfo{}, false
	}
	bits := atoi(m[1])
	if !intWidths[bits] {
		return scalarInfo{}, false
	}
	size := bits / 8
	if bits == 1 {
		size = 1
	}
	return scalarInfo{"int", size, size}, true
}

func alignUp(value, alignment int) int {
	return (value + alignment - 1) & -alignment
}

type fieldLay struct {
	Type   string
	Bank   string
	Size   int
	Offset int
}

type aggLay struct {
	Fields    []fieldLay
	Size      int
	Alignment int
}

func aggregateLayout(ty, purpose string, returnLimit bool) (*aggLay, string) {
	if strings.HasPrefix(ty, "<{") {
		return nil, "packed aggregate " + purpose + " layouts are unsupported"
	}
	if !(strings.HasPrefix(ty, "{") && strings.HasSuffix(ty, "}")) {
		return nil, ""
	}
	body := strings.TrimSpace(ty[1 : len(ty)-1])
	var fields []string
	if body != "" {
		fields = splitTopLevel(body)
	}
	if len(fields) == 0 {
		return nil, "empty aggregate " + purpose + " is unsupported"
	}
	offset := 0
	structAlign := 1
	var laid []fieldLay
	for _, tyField := range fields {
		info, ok := scalar(tyField)
		if !ok {
			return nil, "aggregate field type " + tyField + " is outside the supported scalar subset"
		}
		offset = alignUp(offset, info.align)
		laid = append(laid, fieldLay{Type: tyField, Bank: info.bank, Size: info.size, Offset: offset})
		offset += info.size
		if info.align > structAlign {
			structAlign = info.align
		}
	}
	size := alignUp(offset, structAlign)
	if returnLimit && size > 16 {
		return nil, fmt.Sprintf("aggregate return is %d bytes; SysV register returns are limited to 16 bytes (larger results require sret)", size)
	}
	var eight []int
	for _, field := range laid {
		first := field.Offset / 8
		last := (field.Offset + field.Size - 1) / 8
		if first != last {
			return nil, "aggregate " + purpose + " field crosses an eightbyte boundary"
		}
		eight = append(eight, first)
	}
	seen := map[int]bool{}
	for _, e := range eight {
		if seen[e] {
			return nil, "aggregate " + purpose + " packs several fields into one eightbyte"
		}
		seen[e] = true
	}
	return &aggLay{Fields: laid, Size: size, Alignment: structAlign}, ""
}

func goType(ty string, attrs []string, aggregateField bool) string {
	switch ty {
	case "ptr":
		return "unsafe.Pointer"
	case "float":
		return "float32"
	case "double":
		return "float64"
	}
	m := intTypeRE.FindStringSubmatch(ty)
	if m == nil {
		return ""
	}
	bits := atoi(m[1])
	if bits == 1 {
		return "bool"
	}
	if aggregateField {
		return fmt.Sprintf("uint%d", bits)
	}
	prefix := "int"
	for _, a := range attrs {
		if a == "zeroext" {
			prefix = "uint"
			break
		}
	}
	return prefix + m[1]
}

func goParamType(ty string, attrs []string) string {
	if _, ok := scalar(ty); ok {
		return goType(ty, attrs, false)
	}
	aggregate, _ := aggregateLayout(ty, "parameter", false)
	if aggregate == nil {
		return ""
	}
	var fields []string
	for index, field := range aggregate.Fields {
		ft := goType(field.Type, nil, true)
		if ft == "" {
			return ""
		}
		fields = append(fields, fmt.Sprintf("F%d %s", index, ft))
	}
	return "struct { " + strings.Join(fields, "; ") + " }"
}

type loc struct {
	Kind string `json:"kind"`
	Reg  string `json:"register,omitempty"`
	Off  int    `json:"offset,omitempty"`
}

func (l loc) MarshalJSON() ([]byte, error) {
	if l.Kind == "register" {
		return json.Marshal(struct {
			Kind     string `json:"kind"`
			Register string `json:"register"`
		}{l.Kind, l.Reg})
	}
	return json.Marshal(struct {
		Kind   string `json:"kind"`
		Offset int    `json:"offset"`
	}{l.Kind, l.Off})
}

func regLoc(name string) loc { return loc{Kind: "register", Reg: name} }
func stkLoc(off int) loc     { return loc{Kind: "stack", Off: off} }

type asg struct {
	Type string
	Go   loc
	SysV loc
}

type assignResult struct {
	Params        []asg
	GoStackSize   int
	SysVStackSize int
}

func assignParams(a abiSet, params []string) (*assignResult, string) {
	goInt := 0
	goFloat := 0
	cInt := 0
	cFloat := 0
	goStack := 0
	cStack := 0
	var assignments []asg
	for _, ty := range params {
		info, ok := scalar(ty)
		if !ok {
			aggregate, reason := aggregateLayout(ty, "parameter", false)
			if reason != "" {
				return nil, reason
			}
			if aggregate != nil {
				goNeedInt := 0
				for _, field := range aggregate.Fields {
					if field.Bank == "int" {
						goNeedInt++
					}
				}
				goNeedFloat := len(aggregate.Fields) - goNeedInt
				goInRegs := goInt+goNeedInt <= len(a.goInt) && goFloat+goNeedFloat <= len(a.goFloat)
				var goLocs []loc
				if goInRegs {
					for _, field := range aggregate.Fields {
						if field.Bank == "int" {
							goLocs = append(goLocs, regLoc(a.goInt[goInt]))
							goInt++
						} else {
							goLocs = append(goLocs, regLoc(a.goFloat[goFloat]))
							goFloat++
						}
					}
				} else {
					goBase := alignUp(goStack, aggregate.Alignment)
					for _, field := range aggregate.Fields {
						goLocs = append(goLocs, stkLoc(goBase+field.Offset))
					}
					goStack = goBase + aggregate.Size
				}
				cNeedInt := 0
				for _, field := range aggregate.Fields {
					if field.Bank == "int" {
						cNeedInt++
					}
				}
				cNeedFloat := len(aggregate.Fields) - cNeedInt
				cInRegs := aggregate.Size <= 16 && cInt+cNeedInt <= len(a.sysvInt) && cFloat+cNeedFloat <= len(a.sysvFloat)
				var cLocs []loc
				if cInRegs {
					for _, field := range aggregate.Fields {
						if field.Bank == "int" {
							cLocs = append(cLocs, regLoc(a.sysvInt[cInt]))
							cInt++
						} else {
							cLocs = append(cLocs, regLoc(a.sysvFloat[cFloat]))
							cFloat++
						}
					}
				} else {
					cBase := alignUp(cStack, 8)
					for _, field := range aggregate.Fields {
						cLocs = append(cLocs, stkLoc(cBase+field.Offset))
					}
					cStack = cBase + alignUp(aggregate.Size, 8)
				}
				for i, field := range aggregate.Fields {
					assignments = append(assignments, asg{Type: field.Type, Go: goLocs[i], SysV: cLocs[i]})
				}
				continue
			}
			if strings.HasPrefix(ty, "%") {
				return nil, "identified aggregate parameter type " + ty + " has no literal field layout and is unsupported"
			}
			if strings.HasPrefix(ty, "{") || strings.HasPrefix(ty, "<{") {
				return nil, "aggregate parameter type " + ty + " is unsupported"
			}
			return nil, "parameter type " + ty + " is outside the scalar subset"
		}
		var goL, cL loc
		if info.bank == "int" && goInt < len(a.goInt) {
			goL = regLoc(a.goInt[goInt])
			goInt++
		} else if info.bank == "float" && goFloat < len(a.goFloat) {
			goL = regLoc(a.goFloat[goFloat])
			goFloat++
		} else {
			goStack = alignUp(goStack, min(info.align, 8))
			goL = stkLoc(goStack)
			goStack += info.size
		}
		if info.bank == "int" && cInt < len(a.sysvInt) {
			cL = regLoc(a.sysvInt[cInt])
			cInt++
		} else if info.bank == "float" && cFloat < len(a.sysvFloat) {
			cL = regLoc(a.sysvFloat[cFloat])
			cFloat++
		} else {
			cL = stkLoc(cStack)
			cStack += 8
		}
		assignments = append(assignments, asg{Type: ty, Go: goL, SysV: cL})
	}
	if assignments == nil {
		assignments = []asg{}
	}
	return &assignResult{Params: assignments, GoStackSize: alignUp(goStack, 8), SysVStackSize: cStack}, ""
}

type resLoc struct {
	Type     string
	Location loc
}

func returnAssignment(a abiSet, ret string) (goR, sysvR []resLoc, resultTypes []string, reason string) {
	if ret == "void" {
		return []resLoc{}, []resLoc{}, []string{}, ""
	}
	if info, ok := scalar(ret); ok {
		reg := a.goInt[0]
		if info.bank != "int" {
			reg = a.goFloat[0]
		}
		one := []resLoc{{Type: ret, Location: regLoc(reg)}}
		return one, []resLoc{{Type: ret, Location: regLoc(reg)}}, []string{}, ""
	}
	aggregate, why := aggregateLayout(ret, "return", true)
	if why != "" {
		return nil, nil, nil, why
	}
	if aggregate != nil {
		intResult := 0
		floatResult := 0
		for _, field := range aggregate.Fields {
			var reg, sysvReg string
			if field.Bank == "int" {
				reg = a.goInt[intResult]
				sysvReg = a.sysvIntResult[intResult]
				intResult++
			} else {
				reg = a.goFloat[floatResult]
				sysvReg = a.sysvFloatResult[floatResult]
				floatResult++
			}
			goR = append(goR, resLoc{Type: field.Type, Location: regLoc(reg)})
			sysvR = append(sysvR, resLoc{Type: field.Type, Location: regLoc(sysvReg)})
			resultTypes = append(resultTypes, field.Type)
		}
		return goR, sysvR, resultTypes, ""
	}
	if strings.HasPrefix(ret, "{") || strings.HasPrefix(ret, "<{") {
		return nil, nil, nil, "aggregate return type " + ret + " is unsupported"
	}
	if strings.HasPrefix(ret, "%") {
		return nil, nil, nil, "identified aggregate return type " + ret + " has no literal field layout and is unsupported"
	}
	return nil, nil, nil, "return type " + ret + " is outside the supported result subset"
}

func outgoingBytes(stackSize int) (int, error) {
	if stackSize < 0 || stackSize%8 != 0 {
		return 0, fmt.Errorf("stack argument area %d is not a multiple of 8", stackSize)
	}
	return (stackSize + 15) &^ 15, nil
}

type sigPlan struct {
	ABI         abiSides
	GoSignature string
	ResultMoves [][2]string
	Outgoing    int
}

type abiParam struct {
	Type     string `json:"type"`
	Location loc    `json:"location"`
}

type abiSide struct {
	Params        []abiParam `json:"params"`
	Results       []resJSON  `json:"results"`
	StackArgsSize int        `json:"stack_args_size"`
}

type resJSON struct {
	Type     string `json:"type"`
	Location loc    `json:"location"`
}

type abiSides struct {
	Go   abiSide `json:"go"`
	SysV abiSide `json:"sysv"`
}

func analyzeSignature(a abiSet, d llFunc) (*sigPlan, string) {
	if strings.HasPrefix(d.Name, "llvm.") {
		return nil, "LLVM intrinsics are not C entrypoints"
	}
	if d.Internal {
		return nil, "file-local functions are not Go-callable"
	}
	if d.Varargs {
		return nil, "variadic functions are unsupported"
	}
	if d.NonC {
		return nil, "non-C LLVM calling conventions are unsupported"
	}
	for _, attrs := range d.ParamABI {
		if len(attrs) > 0 {
			return nil, "ABI-changing LLVM parameter attributes are unsupported"
		}
	}
	if len(d.RetABI) > 0 {
		return nil, "ABI-changing LLVM return attributes are unsupported"
	}
	if d.Ret == "" {
		return nil, "LLVM return type could not be parsed"
	}
	assigned, reason := assignParams(a, d.Params)
	if reason != "" {
		return nil, reason
	}
	goResults, sysvResults, resultTypes, reason := returnAssignment(a, d.Ret)
	if reason != "" {
		return nil, reason
	}
	var moves [][2]string
	for i := range sysvResults {
		src := sysvResults[i].Location.Reg
		dst := goResults[i].Location.Reg
		if src != dst {
			moves = append(moves, [2]string{src, dst})
		}
	}
	if moves == nil {
		moves = [][2]string{}
	}
	goParams := make([]string, len(d.Params))
	for i, ty := range d.Params {
		goParams[i] = goParamType(ty, d.ParamAttrs[i])
		if goParams[i] == "" {
			return nil, "a parameter has no supported Go-facing type"
		}
	}
	var goResultTypes []string
	if len(resultTypes) > 0 {
		goResultTypes = make([]string, len(resultTypes))
		for i, ty := range resultTypes {
			goResultTypes[i] = goType(ty, nil, true)
		}
	} else if d.Ret != "void" {
		goResultTypes = []string{goType(d.Ret, d.RetAttrs, false)}
	}
	for _, ty := range goResultTypes {
		if ty == "" {
			return nil, "the result has no supported Go-facing type"
		}
	}
	signature := fmt.Sprintf("func %s(%s)", d.Name, strings.Join(goParams, ", "))
	switch len(goResultTypes) {
	case 0:
	case 1:
		signature += " " + goResultTypes[0]
	default:
		signature += " (" + strings.Join(goResultTypes, ", ") + ")"
	}
	toParams := func(pick func(asg) loc) []abiParam {
		out := make([]abiParam, len(assigned.Params))
		for i, p := range assigned.Params {
			out[i] = abiParam{Type: p.Type, Location: pick(p)}
		}
		return out
	}
	toRes := func(rs []resLoc) []resJSON {
		if rs == nil {
			rs = []resLoc{}
		}
		out := make([]resJSON, len(rs))
		for i, r := range rs {
			out[i] = resJSON{Type: r.Type, Location: r.Location}
		}
		return out
	}
	out, err := outgoingBytes(assigned.SysVStackSize)
	if err != nil {
		return nil, err.Error()
	}
	return &sigPlan{
		ABI: abiSides{
			Go: abiSide{
				Params:        toParams(func(p asg) loc { return p.Go }),
				Results:       toRes(goResults),
				StackArgsSize: assigned.GoStackSize,
			},
			SysV: abiSide{
				Params:        toParams(func(p asg) loc { return p.SysV }),
				Results:       toRes(sysvResults),
				StackArgsSize: assigned.SysVStackSize,
			},
		},
		GoSignature: signature,
		ResultMoves: moves,
		Outgoing:    out,
	}, ""
}

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

func emitThunks(a abiSet, goabi []llFunc) string {
	if a.arch == "arm64" {
		return emitArm64Thunks(goabi)
	}
	lines := []string{"# Go 1.24 ABIInternal -> SysV entry thunks — generated", "\t.text"}
	for _, d := range goabi {
		lines = append(lines,
			fmt.Sprintf("\t.globl\t%s", d.Name),
			fmt.Sprintf("\t.type\t%s,@function", d.Name),
			d.Name+":",
			"\tpushq\t%rbp",
			"\tmovq\t%rsp, %rbp",
		)
		if d.plan.Outgoing > 0 {
			lines = append(lines, fmt.Sprintf("\tsubq\t$%d, %%rsp", d.plan.Outgoing))
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
		lines = append(lines, "\tcall\t"+d.Impl)
		for _, mv := range d.plan.ResultMoves {
			lines = append(lines, fmt.Sprintf("\tmovq\t%%%s, %%%s", mv[0], mv[1]))
		}
		lines = append(lines,
			"\tpxor\t%xmm15, %xmm15",
			"\tmovq\t%rbp, %rsp",
			"\tpopq\t%rbp",
			"\tret",
			fmt.Sprintf("\t.size\t%s, .-%s", d.Name, d.Name),
		)
	}
	lines = append(lines, "")
	return strings.Join(lines, "\n")
}

func emitArm64Thunks(goabi []llFunc) string {
	lines := []string{"// Go 1.24 arm64 ABIInternal -> AAPCS64 entry thunks — generated", "\t.text"}
	for _, d := range goabi {
		lines = append(lines,
			fmt.Sprintf("\t.globl\t%s", d.Name),
			fmt.Sprintf("\t.type\t%s, %%function", d.Name),
			"\t.balign\t4",
			d.Name+":",
			"\tstp\tx29, x30, [sp, #-16]!",
			"\tmov\tx29, sp",
		)
		if d.plan.Outgoing > 0 {
			lines = append(lines, fmt.Sprintf("\tsub\tsp, sp, #%d", d.plan.Outgoing))
		}
		var regMoves []a64gpr
		var floatMoves []a64fpr
		var intLoads, floatLoads []string
		for i, arg := range d.plan.ABI.Go.Params {
			ty := arg.Type
			goLoc := arg.Location
			sysvLoc := d.plan.ABI.SysV.Params[i].Location
			if sysvLoc.Kind == "stack" {
				lines = append(lines, emitArm64StackArgument(ty, goLoc, sysvLoc.Off)...)
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
		lines = append(lines, "\tbl\t"+d.Impl)
		for _, mv := range d.plan.ResultMoves {
			if strings.ContainsAny(mv[0][:1], "dsqv") {
				lines = append(lines, fmt.Sprintf("\tfmov\t%s, %s", mv[1], mv[0]))
			} else {
				lines = append(lines, fmt.Sprintf("\tmov\t%s, %s", mv[1], mv[0]))
			}
		}
		lines = append(lines,
			"\tmov\tsp, x29",
			"\tldp\tx29, x30, [sp], #16",
			"\tret",
			fmt.Sprintf("\t.size\t%s, .-%s", d.Name, d.Name),
		)
	}
	lines = append(lines, "")
	return strings.Join(lines, "\n")
}

func mustScalar(ty string) (string, int, int) {
	info, ok := scalar(ty)
	if !ok {
		return "", 0, 0
	}
	return info.bank, info.size, info.align
}

func symCont(b byte) bool {
	return (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || (b >= '0' && b <= '9') || b == '_' || b == '.' || b == '$'
}

// replaceAtName replaces @name when the next byte is not an identifier
// continuation. Go's regexp engine has no lookahead.
func replaceAtName(text, name, repl string) string {
	needle := "@" + name
	var b strings.Builder
	i := 0
	for {
		j := strings.Index(text[i:], needle)
		if j < 0 {
			b.WriteString(text[i:])
			return b.String()
		}
		j += i
		end := j + len(needle)
		if end < len(text) && symCont(text[end]) {
			b.WriteString(text[i:end])
			i = end
			continue
		}
		b.WriteString(text[i:j])
		b.WriteString("@" + repl)
		i = end
	}
}

type goFuncJSON struct {
	Name       string     `json:"name"`
	Ret        string     `json:"ret"`
	RetAttrs   []string   `json:"ret_attrs"`
	RetABI     []string   `json:"ret_abi_attrs"`
	Params     []string   `json:"params"`
	ParamAttrs [][]string `json:"param_attrs"`
	ParamABI   [][]string `json:"param_abi_attrs"`
	Varargs    bool       `json:"varargs"`
	Internal   bool       `json:"internal"`
	NonC       bool       `json:"non_c_calling_convention"`
	ABI        abiSides   `json:"abi"`
	GoSig      string     `json:"go_signature"`
	Outgoing   int        `json:"outgoing"`
	Impl       string     `json:"impl"`
}

type goABIFile struct {
	Functions []goFuncJSON `json:"functions"`
	Skipped   []string     `json:"skipped"`
}

// RunGoABI rewrites eligible definitions to name.impl and returns the thunk
// assembly plus the sidecar JSON. logText is the human summary printed by
// the old goc_goabi.py.
func RunGoABI(text, arch string) (outLL, asm string, js []byte, logText string, err error) {
	a, err := abiFor(arch)
	if err != nil {
		return "", "", nil, "", err
	}
	defs := parseFunctions(text, "define")
	var goabi []llFunc
	var skipped []string
	reasons := map[string]string{}
	for _, d := range defs {
		plan, reason := analyzeSignature(a, d)
		if reason != "" {
			skipped = append(skipped, d.Name)
			reasons[d.Name] = reason
			continue
		}
		d.plan = plan
		d.ABI = &plan.ABI
		d.GoSignature = plan.GoSignature
		d.Outgoing = plan.Outgoing
		d.Impl = d.Name + ".impl"
		goabi = append(goabi, d)
	}
	if skipped == nil {
		skipped = []string{}
	}
	rename := map[string]bool{}
	for _, d := range goabi {
		rename[d.Name] = true
	}
	for _, d := range parseFunctions(text, "declare") {
		if rename[d.Name] {
			continue
		}
		if plan, _ := analyzeSignature(a, d); plan != nil {
			rename[d.Name] = true
		}
	}
	names := make([]string, 0, len(rename))
	for name := range rename {
		names = append(names, name)
	}
	sort.Strings(names)
	for _, name := range names {
		text = replaceAtName(text, name, name+".impl")
	}
	asm = emitThunks(a, goabi)
	file := goABIFile{Functions: []goFuncJSON{}, Skipped: skipped}
	for _, d := range goabi {
		file.Functions = append(file.Functions, goFuncJSON{
			Name: d.Name, Ret: d.Ret, RetAttrs: d.RetAttrs, RetABI: d.RetABI,
			Params: d.Params, ParamAttrs: d.ParamAttrs, ParamABI: d.ParamABI,
			Varargs: d.Varargs, Internal: d.Internal, NonC: d.NonC,
			ABI: *d.ABI, GoSig: d.GoSignature, Outgoing: d.Outgoing, Impl: d.Impl,
		})
	}
	js, err = marshalIndent(file)
	if err != nil {
		return "", "", nil, "", err
	}
	var log bytes.Buffer
	extra := ""
	if len(skipped) > 0 {
		extra = " [" + strings.Join(skipped, ", ") + "]"
	}
	fmt.Fprintf(&log, "goabi: %d rewritten, %d left SysV-only%s\n", len(goabi), len(skipped), extra)
	for _, name := range skipped {
		fmt.Fprintf(&log, "goabi: skipped %s: %s\n", name, reasons[name])
	}
	return text, asm, js, log.String(), nil
}

func atoi(s string) int {
	n := 0
	for _, c := range s {
		n = n*10 + int(c-'0')
	}
	return n
}

func marshalIndent(v any) ([]byte, error) {
	var buf bytes.Buffer
	enc := json.NewEncoder(&buf)
	enc.SetEscapeHTML(false)
	enc.SetIndent("", "  ")
	if err := enc.Encode(v); err != nil {
		return nil, err
	}
	// encoding/json adds a trailing newline. Python json.dump does not.
	b := buf.Bytes()
	if len(b) > 0 && b[len(b)-1] == '\n' {
		b = b[:len(b)-1]
	}
	return b, nil
}

// WriteGoABI is the goc-lower goabi subcommand.
func WriteGoABI(inLL, outLL, outS, outJSON, arch string) error {
	text, err := os.ReadFile(inLL)
	if err != nil {
		return err
	}
	ll, asm, js, logText, err := RunGoABI(string(text), arch)
	if err != nil {
		return err
	}
	if err := os.WriteFile(outLL, []byte(ll), 0o644); err != nil {
		return err
	}
	if err := os.WriteFile(outS, []byte(asm), 0o644); err != nil {
		return err
	}
	if err := os.WriteFile(outJSON, js, 0o644); err != nil {
		return err
	}
	os.Stdout.WriteString(logText)
	return nil
}

// silence unused in case nameRE is wanted by later files in the package.
var _ = nameRE
