// Package lower is the goc real-body lowering that used to run as Python
// and ripgrep: Go ABI thunks, IR fixups, and argument layout.
package lower

import (
	"bytes"
	"encoding/json"
	"fmt"
	"os"
	"sort"
	"strings"
)

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
	outLL, asm, js, logText, _, err = runGoABI(text, arch, false)
	return
}

func runGoABI(text, arch string, tables bool) (outLL, asm string, js []byte, logText, wrappers string, err error) {
	a, err := abiFor(arch)
	if err != nil {
		return "", "", nil, "", "", err
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
	asm = emitThunksL(a, goabi, tables)
	if tables {
		if wrappers, err = emitWrapperTables(a, goabi); err != nil {
			return "", "", nil, "", "", err
		}
	}
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
		return "", "", nil, "", "", err
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
	return text, asm, js, log.String(), wrappers, nil
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
// WriteGoABI writes the renamed IR, the thunk assembly and goabi.json. With
// a non-empty outTables (amd64) it also writes the thunks' .goc.* table
// records as assembler text (docs/gocobj-format.md, 1.3).
func WriteGoABI(inLL, outLL, outS, outJSON, outTables, arch string) error {
	text, err := os.ReadFile(inLL)
	if err != nil {
		return err
	}
	tables := outTables != ""
	ll, asm, js, logText, wrappers, err := runGoABI(string(text), arch, tables)
	if err != nil {
		return err
	}
	if outTables != "" {
		if err := os.WriteFile(outTables, []byte(wrappers), 0o644); err != nil {
			return err
		}
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

// emitWrapperTables writes, for each thunk, a .goc.funcs record (kind 1,
// ABI wrapper), its .goc.wrappers record and the CALL relocation to the C
// body. Strings go to .goc.strtab under their own labels and are referenced
// from goc-llc's .Lgoc_strtab, so this text is appended after goc-llc's.
func emitWrapperTables(a abiSet, goabi []llFunc) (string, error) {
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
		return fmt.Sprintf(".Lgocw_s%d-.Lgoc_strtab", i)
	}
	rec := 0
	begin := func() string {
		l := fmt.Sprintf(".Lgocw_q%d", rec)
		rec++
		fmt.Fprintf(&b, "%s:\n\t.uleb128 %se-%sb\n%sb:\n", l, l, l, l)
		return l
	}
	end := func(l string) { fmt.Fprintf(&b, "%se:\n", l) }
	uleb := func(v any) { fmt.Fprintf(&b, "\t.uleb128 %v\n", v) }
	u8 := func(v int) { fmt.Fprintf(&b, "\t.byte %d\n", v) }
	type thunk struct {
		spills    []argSpill
		stackPtrs []int
		area      int
		slots     []int
		mask      uint64
	}
	// The thunk frame: x86-64 pushes RBP (8 bytes, BP = the saved RBP);
	// arm64 allocates 32 bytes: the LR copy at 0(RSP) Go unwinds through,
	// and the X29/X30 record at +16 (X29 points at it). Outgoing stack
	// arguments sit below that in both. After "mov sp, x29" an arm64
	// thunk is 16 below its entry SP.
	a64 := a.arch == "arm64"
	save, restored := 8, 8
	if a64 {
		save, restored = 32, 16
	}
	ts := make([]thunk, len(goabi))
	for i, d := range goabi {
		spills, stackPtrs, area, err := goArgsLayout(d.ABI.Go)
		if err != nil {
			return "", err
		}
		var slots []int
		for _, p := range d.ABI.SysV.Params {
			if p.Location.Kind == "stack" && p.Type == "ptr" {
				// Bytes below varp: BP on x86-64; on arm64 Go's varp is
				// the entry SP minus 8, i.e. 8 above the saved X29.
				o := d.Outgoing - p.Location.Off
				if a64 {
					o += save - 8
				}
				slots = append(slots, o)
			}
		}
		if a64 {
			// The saved X29 (entry SP - 16) is a raw address into the Go
			// caller's frame; a stack copy must adjust it.
			slots = append(slots, 8)
		}
		sort.Ints(slots)
		// A thunk is entered with the Go ABI; SysV pointer registers
		// describe C entries only.
		ts[i] = thunk{spills, stackPtrs, area, slots, 0}
	}
	b.WriteString("# .goc.* records of the Go ABI thunks (goc-lower goabi)\n")
	// A thunk has no stack check (nosplit: the linker charges its small
	// frame to the caller's guard; the C body checks its own frame). Its
	// SP_DELTA follows the push/sub/mov/pop, and its one safepoint, the call
	// to the C body, scans the Go stack arguments and the outgoing pointer
	// arguments.
	pcv := make([]string, len(goabi))
	b.WriteString("\t.section .goc.pcvalue,\"\",%progbits\n")
	for i, d := range goabi {
		l := begin()
		pcv[i] = l
		at := func(lbl string, v int) {
			fmt.Fprintf(&b, "\t.uleb128 .Lgocw_%s%d-%s\n\t.sleb128 %d\n", lbl, i, d.Name, v)
		}
		uleb(1)
		u8(1) // SP_DELTA
		if d.Outgoing > 0 {
			uleb(5)
		} else {
			uleb(4)
		}
		uleb(0)
		fmt.Fprintf(&b, "\t.sleb128 0\n")
		at("p", save)
		if d.Outgoing > 0 {
			at("a", save+outArea(a64, d.Outgoing))
		}
		at("m", restored)
		at("o", 0)
		end(l)
	}
	gc := make([]string, len(goabi))
	b.WriteString("\t.section .goc.gcmaps,\"\",%progbits\n")
	for i, d := range goabi {
		t := ts[i]
		if t.area%8 != 0 || d.Outgoing%8 != 0 {
			return "", fmt.Errorf("goabi: %s: Go argument area %d or outgoing area %d is not word aligned", d.Name, t.area, d.Outgoing)
		}
		nargs, nlocals := t.area/8, d.Outgoing/8
		if a64 {
			nlocals = (outArea(a64, d.Outgoing) + save - 8) / 8 // varp = entry SP - 8
		}
		args := make([]byte, (nargs+7)/8)
		for _, o := range t.stackPtrs {
			if o%8 != 0 || o/8 >= nargs {
				return "", fmt.Errorf("goabi: %s: stack pointer argument +%d outside the %d-byte area", d.Name, o, t.area)
			}
			args[o/8/8] |= 1 << (o / 8 % 8)
		}
		locals := make([]byte, (nlocals+7)/8)
		for _, o := range t.slots {
			// o bytes below BP; bit 0 is the word at BP-8*nlocals.
			if o < 8 || o%8 != 0 || o/8 > nlocals {
				return "", fmt.Errorf("goabi: %s: outgoing pointer slot BP-%d outside the frame", d.Name, o)
			}
			bit := nlocals - o/8
			locals[bit/8] |= 1 << (bit % 8)
		}
		l := begin()
		gc[i] = l
		uleb(nargs)
		uleb(nlocals)
		uleb(1)
		uleb(fmt.Sprintf(".Lgocw_r%d-%s", i, d.Name))
		for _, v := range args {
			u8(int(v))
		}
		for _, v := range locals {
			u8(int(v))
		}
		uleb(0) // ncolor
		end(l)
	}
	b.WriteString("\t.section .goc.funcs,\"\",%progbits\n")
	for i, d := range goabi {
		t := ts[i]
		l := begin()
		uleb(ref(d.Name))
		uleb(fmt.Sprintf(".Lgocw_e%d-%s", i, d.Name))
		uleb(save + outArea(a64, d.Outgoing)) // frame
		uleb(t.area)                          // arg_size
		u8(1)                                 // ABIInternal
		u8(1)                                 // kind: ABI wrapper
		uleb(4 | 8)                           // flags: nosplit, has_frame_pointer
		u8(0)                                 // func_id
		uleb(0)                               // file
		uleb(0)                               // start_line
		uleb(pcv[i] + "-.Lgoc_pcvbase+1")
		uleb(gc[i] + "-.Lgoc_gcbase+1")
		uleb(0)            // inl
		uleb(t.mask)       // 1.1 sysv_ptr_regs
		uleb(len(t.slots)) // 1.2 sptr_slots
		for _, o := range t.slots {
			uleb(o)
		}
		end(l)
	}
	b.WriteString("\t.section .goc.wrappers,\"\",%progbits\n")
	for i, d := range goabi {
		t := ts[i]
		l := begin()
		uleb(ref(d.Name))
		uleb(ref(d.Impl))
		u8(1) // from ABIInternal
		u8(2) // to the C ABI (SysV)
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
		uleb(len(t.stackPtrs))
		for _, o := range t.stackPtrs {
			uleb(o)
		}
		end(l)
	}
	b.WriteString("\t.section .goc.relocs,\"\",%progbits\n")
	for i, d := range goabi {
		l := begin()
		uleb(ref(d.Name))
		if a64 {
			uleb(fmt.Sprintf(".Lgocw_c%d-%s", i, d.Name))
			u8(4)
			u8(11) // ARM64_CALL26: on the BL itself
			uleb(ref(d.Impl))
			fmt.Fprintf(&b, "\t.sleb128 0\n")
		} else {
			uleb(fmt.Sprintf(".Lgocw_r%d-4-%s", i, d.Name))
			u8(4)
			u8(1) // CALL
			uleb(ref(d.Impl))
			fmt.Fprintf(&b, "\t.sleb128 -4\n")
		}
		u8(0)
		end(l)
	}
	b.WriteString("\t.section .goc.strtab,\"\",%progbits\n")
	for i, s := range list {
		fmt.Fprintf(&b, ".Lgocw_s%d:\n\t.asciz %q\n", i, s)
	}
	return b.String(), nil
}
