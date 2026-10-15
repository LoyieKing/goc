package lower

import (
	"encoding/json"
	"fmt"
	"strings"
)

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
