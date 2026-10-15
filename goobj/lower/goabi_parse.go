package lower

import (
	"fmt"
	"regexp"
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
