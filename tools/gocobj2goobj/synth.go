package main

import (
	"encoding/json"
	"fmt"
	"os"
	"path"
	"reflect"
	"regexp"
	"sort"

	"github.com/LoyieKing/goc/gocobj"
)

// What the converter can now derive from the .goc.* tables instead of from
// goc-lower's text parsing of the assembly and disassembly. The JSON field
// names are the meta.json ones elfpack reads.

type synthCall struct {
	Callee        string `json:"callee"`
	Off           int64  `json:"off"`
	StackmapIndex int    `json:"stackmap_index"`
}

type synthBranch struct {
	Callee string `json:"callee"`
	Off    int64  `json:"off"`
}

type synthFn struct {
	MIRName         string        `json:"mir_name"`
	GoSym           string        `json:"go_sym"`
	Frame           int           `json:"frame"`
	Flags           string        `json:"flags"`
	Encoding        string        `json:"encoding"`
	Calls           []synthCall   `json:"calls"`
	Branches        []synthBranch `json:"branches"`
	HasSptr         bool          `json:"has_sptr"`
	SptrSlots       []int         `json:"sptr_slots"`
	SysvPointerRegs []string      `json:"sysv_pointer_regs"`
	ABI             string        `json:"abi"`
}

var (
	tuCharRE   = regexp.MustCompile(`[^A-Za-z0-9_]`)
	tuSuffixRE = regexp.MustCompile(`\.(c|goc|ll|bc)$`)
	sysvRegs   = []string{"rdi", "rsi", "rdx", "rcx", "r8", "r9"}
)

// tuTag is goc-lower's translation-unit tag: the source basename with every
// character outside [A-Za-z0-9_] replaced by '_'.
func tuTag(f *gocobj.File) string {
	if len(f.Files) == 0 || f.Files[0] == "" {
		return "tu"
	}
	return tuSuffixRE.ReplaceAllString(tuCharRE.ReplaceAllString(path.Base(f.Files[0]), "_"), "")
}

// synthFuncs builds the meta entries of the functions goc-llc compiled.
// flags is the stack-check policy word ("nosplit" or "noframe") and cABI the
// per-function ABI ("ABI0" when Go ABI thunks wrap the C functions).
func synthFuncs(f *gocobj.File, pkg, flags, cABI string) ([]synthFn, error) {
	tu := tuTag(f)
	byFunc := map[string][]gocobj.Reloc{}
	for _, r := range f.Relocs {
		byFunc[r.Func] = append(byFunc[r.Func], r)
	}
	var out []synthFn
	for _, fn := range f.Funcs {
		if fn.Flags&gocobj.FuncSysvUnmodelled != 0 {
			return nil, fmt.Errorf("%s: SysV signature not modelled in the tables", fn.Name)
		}
		e := synthFn{MIRName: fn.Name, GoSym: pkg + "." + fn.Name, Frame: int(fn.FrameSize),
			Flags: flags, Encoding: "clang-real-isel", HasSptr: fn.Flags&gocobj.FuncHasSptr != 0,
			SptrSlots: []int{}, SysvPointerRegs: []string{}, ABI: cABI,
			Calls: []synthCall{}, Branches: []synthBranch{}}
		if fn.Flags&gocobj.FuncInternal != 0 {
			e.GoSym = pkg + "." + tu + "." + fn.Name
		}
		for i, reg := range sysvRegs {
			if fn.SysvPtrRegs&(1<<i) != 0 {
				e.SysvPointerRegs = append(e.SysvPointerRegs, reg)
			}
		}
		elfTarget := func(off uint64) string {
			return f.ELFRelocs[fmt.Sprintf("%s+%d", fn.Name, off)]
		}
		for _, r := range byFunc[fn.Name] {
			switch r.Kind {
			case gocobj.RCall:
				callee := elfTarget(r.Offset)
				if callee == "" {
					callee = "*"
				}
				e.Calls = append(e.Calls, synthCall{callee, int64(r.Offset) - 1, -1})
			case gocobj.RCallIndirectMark:
				e.Calls = append(e.Calls, synthCall{"*", int64(r.Offset), -1})
			case gocobj.RFuncAddr:
				// lea f(%rip): REX.W 8D modrm disp32
				callee := elfTarget(r.Offset)
				if callee == "" {
					if r.Target == fn.Name && r.Addend+4 != 0 {
						continue // inside this function
					}
					callee = r.Target
					if a := r.Addend + 4; a != 0 {
						callee = fmt.Sprintf("%s+%#x", r.Target, a)
					}
				}
				e.Branches = append(e.Branches, synthBranch{callee, int64(r.Offset) - 3})
			}
		}
		sort.Slice(e.Calls, func(a, b int) bool { return e.Calls[a].Off < e.Calls[b].Off })
		sort.Slice(e.Branches, func(a, b int) bool { return e.Branches[a].Off < e.Branches[b].Off })
		out = append(out, e)
	}
	return out, nil
}

// checkAgainstMeta compares the synthesized entries with goc-lower's
// meta.json and returns one line per difference.
func checkAgainstMeta(syn []synthFn, metaPath string) ([]string, error) {
	b, err := os.ReadFile(metaPath)
	if err != nil {
		return nil, err
	}
	var doc struct {
		Functions []map[string]any `json:"functions"`
	}
	if err := json.Unmarshal(b, &doc); err != nil {
		return nil, err
	}
	meta := map[string]map[string]any{}
	for _, m := range doc.Functions {
		meta[fmt.Sprint(m["mir_name"])] = m
	}
	var diffs []string
	for _, s := range syn {
		m := meta[s.MIRName]
		if m == nil {
			diffs = append(diffs, s.MIRName+": not in meta.json")
			continue
		}
		var mine map[string]any
		js, _ := json.Marshal(s)
		_ = json.Unmarshal(js, &mine)
		for k, v := range mine {
			if !reflect.DeepEqual(v, m[k]) {
				diffs = append(diffs, fmt.Sprintf("%s: %s tables=%v meta=%v", s.MIRName, k, clip(v), clip(m[k])))
			}
		}
	}
	sort.Strings(diffs)
	return diffs, nil
}

func clip(v any) string {
	s := fmt.Sprint(v)
	if len(s) > 160 {
		return s[:160] + "..."
	}
	return s
}
