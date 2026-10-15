package lower

import (
	"fmt"
)

// argSpill is where a Go register argument lives in the caller-allocated
// spill area above the return address (offset from the entry SP).
type argSpill struct {
	Reg  string `json:"reg"`
	Off  int    `json:"off"`
	Size int    `json:"size"`
	Ptr  bool   `json:"ptr"`
}

// goArgsLayout returns the spill slots of the register arguments, the offsets
// of the pointer arguments passed on the stack and the size of the Go
// argument area.
func goArgsLayout(info abiSide) ([]argSpill, []int, int, error) {
	off := info.StackArgsSize
	var spills []argSpill
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
		spills = append(spills, argSpill{Reg: loc.Reg, Off: 8 + off, Size: size, Ptr: param.Type == "ptr"})
		off += size
	}
	if spills == nil {
		spills = []argSpill{}
	}
	if stackPtrs == nil {
		stackPtrs = []int{}
	}
	return spills, stackPtrs, (off + 7) & -8, nil
}
