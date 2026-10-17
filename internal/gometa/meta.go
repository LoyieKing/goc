// Package gometa holds the Go-version-dependent facts the converter needs to
// write a Go object file: object-format constants, relocation and symbol
// kind numbers, funcdata/pcdata indices, ABI facts and the few writer
// behaviours that changed between releases. One JSON file per Go minor
// release is produced by cmd/goc-gometa from that
// release's GOROOT/src and embedded here (docs/design/go-versions.md §12).
package gometa

import (
	"embed"
	"encoding/json"
	"fmt"
	"os"
	"regexp"
	"sort"
	"strings"
)

// Schema is the version of the JSON layout this package reads.
const Schema = 1

// Meta is one Go release's facts.
type Meta struct {
	Schema        int    `json:"schema"`
	Go            string `json:"go"`             // minor release covered, e.g. "go1.24"
	SourceVersion string `json:"source_version"` // GOROOT/VERSION it was resolved from
	Resolver      string `json:"resolver"`       // tool that wrote it

	Object  Object            `json:"object"`
	Writer  Writer            `json:"writer"`
	Kinds   map[string]int    `json:"sym_kinds"`
	Relocs  map[string]int    `json:"relocs"`
	ABI     ABI               `json:"abi"`
	Arch    map[string]Arch   `json:"arch"`
	DWARF   DWARF             `json:"dwarf"`
	Tools   Tools             `json:"toolexec"`
	Sources map[string]string `json:"sources"` // fact group -> GOROOT/src files read
}

// Object is the goobj container format (cmd/internal/goobj/objfile.go).
type Object struct {
	Magic        string         `json:"magic"`
	HeaderFormat string         `json:"header_format"` // objabi.HeaderString's layout, for documentation
	Blocks       []string       `json:"blocks"`        // block order, Autolib .. End
	ObjFlags     map[string]int `json:"obj_flags"`
	SymFlags     map[string]int `json:"sym_flags"`
	SymFlags2    map[string]int `json:"sym_flags2"`
	AuxTypes     map[string]int `json:"aux_types"`
	PkgIdx       map[string]int `json:"pkg_idx"`
	SymABIStatic int            `json:"sym_abi_static"`
	HashSize     int            `json:"hash_size"`
	Hash64Size   int            `json:"hash64_size"`
	// Builtins is goobj.builtins: name and ABI, in index order.
	Builtins []Builtin `json:"builtins"`
}

// Builtin is one predeclared runtime symbol.
type Builtin struct {
	Name string `json:"name"`
	ABI  int    `json:"abi"`
}

// Writer records behaviours of cmd/internal/obj's writer that differ
// between releases.
type Writer struct {
	// ContentHash names the hash of content-addressable symbols:
	// "notsha256" (sha256 with every output word complemented) or
	// "sha256-prefix01" (sha256 over 0x01 followed by the data) or "sha1"
	// (go1.18).
	ContentHash string `json:"content_hash"`
	// AsmNamedSymsLinkname: an assembler object flags every named symbol
	// SymFlagLinkname.
	AsmNamedSymsLinkname bool `json:"asm_named_syms_linkname"`
	// Align is "heuristic" (alignment of content-addressable data guessed
	// from name and size) or "explicit" (LSym.Align is written as is).
	Align string `json:"align"`
	// TrimInlineHash: symbol names lose a #hash# inline suffix.
	TrimInlineHash bool `json:"trim_inline_hash"`
	// AsmArgsStackmap: Flushplist gives an ABI0 assembler function of the
	// package an args_stackmap and arginfo<ABI> funcdata reference.
	AsmArgsStackmap bool `json:"asm_args_stackmap"`
	// TypePrefix and ItabPrefix are the name prefixes that mark Go type
	// and itab symbols ("type:"/"go:itab." from go1.20, "type."/"go.itab."
	// before).
	TypePrefix string `json:"type_prefix"`
	ItabPrefix string `json:"itab_prefix"`
	// SortRelocs: WriteObjFile sorts a symbol's relocations by offset
	// (from go1.19).
	SortRelocs bool `json:"sort_relocs"`
	// FuncInfoStartLine: goobj.FuncInfo has a StartLine word after
	// FuncFlag (from go1.20).
	FuncInfoStartLine bool `json:"funcinfo_start_line"`
	// InlFileSymRefs: traverseFuncAux also visits the file symbol of every
	// inlined call site (before go1.22).
	InlFileSymRefs bool `json:"inl_file_sym_refs"`
	// FuncFlags and FuncIDs are internal/abi's.
	FuncFlags map[string]int `json:"func_flags"`
	FuncIDs   map[string]int `json:"func_ids"`
	// RuntimeFuncIDs is objabi.funcIDs: runtime function name (without
	// "runtime.") -> FuncID.
	RuntimeFuncIDs map[string]int `json:"runtime_func_ids"`
}

// ABI holds runtime facts shared by the architectures.
type ABI struct {
	Funcdata         map[string]int `json:"funcdata"`
	Pcdata           map[string]int `json:"pcdata"`
	StackSmall       int            `json:"stack_small"`
	StackBig         int            `json:"stack_big"`
	StackNosplitBase int            `json:"stack_nosplit_base"`
	// GOffsets are byte offsets in runtime.g (64-bit).
	GOffsets map[string]int `json:"g_offsets"`
}

// Arch holds one GOARCH's facts.
type Arch struct {
	PtrSize   int      `json:"ptr_size"`
	MinLC     int      `json:"min_lc"`
	GReg      string   `json:"g_reg"`
	IntArgs   []string `json:"int_arg_regs"`
	FloatArgs []string `json:"float_arg_regs"`
	Morestack string   `json:"morestack"`
	// StackCopyAdjustsFP: the runtime's stack copy (adjustframe) rewrites
	// the frame pointers saved in the frames. goc's code addresses locals
	// from the frame pointer, so it needs this (arm64 from go1.21).
	StackCopyAdjustsFP bool `json:"stack_copy_adjusts_fp"`
}

// DWARF describes the per-function DWARF the assembler path emits.
type DWARF struct {
	AbbrevFunction int `json:"abbrev_function"` // DW_ABRV_FUNCTION
	// FuncDIE is the attribute layout of PutDefaultFunc: "v0" (as v1 without
	// DW_AT_decl_line, before go1.21), "v1" (low/high pc
	// as DW_FORM_addr) or "v2" (emitHiLoPc: DW_FORM_addrx + udata when the
	// dwarf5 experiment is on, else as v1).
	FuncDIE string `json:"func_die"`
	// Lines is generateDebugLinesSymbol's layout ("v1").
	Lines string `json:"lines"`
	// TextAddrx maps a function count to the DW_FORM_addrx relocation
	// (objabi.FuncCountToDwTxtAddrFlavor), smallest first.
	TextAddrx []AddrxFlavor `json:"text_addrx,omitempty"`
}

// AddrxFlavor: up to MaxFuncs functions use Reloc, Size bytes wide.
type AddrxFlavor struct {
	MaxFuncs int    `json:"max_funcs"`
	Reloc    string `json:"reloc"`
	Size     int    `json:"size"`
}

// Tools describes the distribution's tools the toolexec wrapper uses.
type Tools struct {
	PackPrebuilt bool `json:"pack_prebuilt"` // $GOROOT/pkg/tool/<os_arch>/pack ships
}

//go:embed data/*.json
var data embed.FS

// Releases lists the embedded minor releases, oldest first.
func Releases() []string {
	ents, _ := data.ReadDir("data")
	var out []string
	for _, e := range ents {
		out = append(out, strings.TrimSuffix(e.Name(), ".json"))
	}
	sort.Slice(out, func(i, j int) bool { return minorNum(out[i]) < minorNum(out[j]) })
	return out
}

func minorNum(v string) int {
	var maj, min int
	fmt.Sscanf(v, "go%d.%d", &maj, &min)
	return maj*1000 + min
}

var minorRE = regexp.MustCompile(`^go(\d+)\.(\d+)`)

// Minor maps a full Go version ("go1.24.4", "go1.25rc1 X:...") to its
// minor release ("go1.24").
func Minor(version string) (string, bool) {
	m := minorRE.FindStringSubmatch(version)
	if m == nil {
		return "", false
	}
	return "go" + m[1] + "." + m[2], true
}

// ForVersion returns the embedded facts for a Go version.
func ForVersion(version string) (*Meta, error) {
	minor, ok := Minor(version)
	if !ok {
		return nil, fmt.Errorf("cannot parse Go version %q", version)
	}
	b, err := data.ReadFile("data/" + minor + ".json")
	if err != nil {
		return nil, fmt.Errorf("no Go object metadata for %s (embedded: %s); run\n"+
			"\tgo run ./cmd/goc-gometa -goroot $(go env GOROOT) -o internal/gometa/data/%s.json\n"+
			"and rebuild, or pass -go-meta=<file> (docs/design/go-versions.md)",
			version, strings.Join(Releases(), " "), minor)
	}
	return Parse(b)
}

// Load reads facts from a JSON file.
func Load(path string) (*Meta, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	m, err := Parse(b)
	if err != nil {
		return nil, fmt.Errorf("%s: %v", path, err)
	}
	return m, nil
}

// Parse decodes and checks facts.
func Parse(b []byte) (*Meta, error) {
	var m Meta
	if err := json.Unmarshal(b, &m); err != nil {
		return nil, err
	}
	if m.Schema != Schema {
		return nil, fmt.Errorf("metadata schema %d, this build reads %d", m.Schema, Schema)
	}
	if err := m.check(); err != nil {
		return nil, err
	}
	return &m, nil
}

// Encode renders facts as the canonical JSON the resolver writes.
func (m *Meta) Encode() ([]byte, error) {
	b, err := json.MarshalIndent(m, "", "  ")
	if err != nil {
		return nil, err
	}
	return append(b, '\n'), nil
}

func (m *Meta) check() error {
	var missing []string
	need := func(group string, have map[string]int, names ...string) {
		for _, n := range names {
			if _, ok := have[n]; !ok {
				missing = append(missing, group+"."+n)
			}
		}
	}
	// R_ARM64_PCREL_LDSTn (go1.20+) are used if present.
	need("relocs", m.Relocs, "R_ADDR", "R_CALL", "R_PCREL", "R_CALLARM64", "R_ADDRARM64",
		"R_ARM64_GOTPCREL", "R_ARM64_PCREL", "R_ARM64_LDST8", "R_ARM64_LDST16",
		"R_ARM64_LDST32", "R_ARM64_LDST64", "R_ARM64_LDST128")
	need("sym_kinds", m.Kinds, "Sxxx", "STEXT", "SRODATA", "SDATA", "SBSS", "SNOPTRDATA", "SNOPTRBSS",
		"SDWARFFCN", "SDWARFLINES")
	need("object.obj_flags", m.Object.ObjFlags, "FromAssembly")
	need("object.sym_flags", m.Object.SymFlags, "Dupok", "Local", "Typelink", "Leaf", "NoSplit", "ReflectMethod", "GoType")
	need("object.sym_flags2", m.Object.SymFlags2, "UsedInIface", "Itab") // Dict, PkgInit, Linkname, ABIWrapper: newer, used if present
	need("object.aux_types", m.Object.AuxTypes, "Gotype", "FuncInfo", "Funcdata", "DwarfInfo", "DwarfLoc",
		"DwarfRanges", "DwarfLines", "Pcsp", "Pcfile", "Pcline", "Pcinline", "Pcdata")
	need("object.pkg_idx", m.Object.PkgIdx, "None", "Hashed64", "Hashed", "Builtin", "Self")
	need("abi.funcdata", m.ABI.Funcdata, "ArgsPointerMaps", "LocalsPointerMaps", "ArgInfo")
	need("abi.pcdata", m.ABI.Pcdata, "UnsafePoint", "StackMapIndex")
	need("writer.func_flags", m.Writer.FuncFlags, "Asm")
	if len(missing) > 0 {
		return fmt.Errorf("metadata for %s lacks %s", m.Go, strings.Join(missing, ", "))
	}
	if m.Object.Magic == "" || m.Object.HashSize == 0 {
		return fmt.Errorf("metadata for %s lacks the object magic or hash size", m.Go)
	}
	switch m.Writer.ContentHash {
	case "notsha256", "sha256-prefix01", "sha1":
	default:
		return fmt.Errorf("metadata for %s: unknown content hash %q", m.Go, m.Writer.ContentHash)
	}
	switch m.Writer.Align {
	case "heuristic", "explicit":
	default:
		return fmt.Errorf("metadata for %s: unknown align model %q", m.Go, m.Writer.Align)
	}
	switch m.DWARF.FuncDIE {
	case "v0", "v1", "v2":
	default:
		return fmt.Errorf("metadata for %s: unknown DWARF function DIE layout %q", m.Go, m.DWARF.FuncDIE)
	}
	if m.Writer.TypePrefix == "" || m.Writer.ItabPrefix == "" {
		return fmt.Errorf("metadata for %s lacks the type/itab symbol prefixes", m.Go)
	}
	if m.DWARF.Lines != "v1" {
		return fmt.Errorf("metadata for %s: unknown DWARF lines layout %q", m.Go, m.DWARF.Lines)
	}
	for _, a := range []string{"amd64", "arm64"} {
		if ar, ok := m.Arch[a]; !ok || ar.MinLC == 0 || ar.PtrSize == 0 {
			return fmt.Errorf("metadata for %s lacks GOARCH %s", m.Go, a)
		}
	}
	return nil
}

// Reloc returns a relocation type's number.
func (m *Meta) Reloc(name string) uint16 {
	v, ok := m.Relocs[name]
	if !ok {
		panic("gometa: no relocation " + name + " in " + m.Go)
	}
	return uint16(v)
}

// Kind returns a symbol kind's number.
func (m *Meta) Kind(name string) uint8 {
	v, ok := m.Kinds[name]
	if !ok {
		panic("gometa: no symbol kind " + name + " in " + m.Go)
	}
	return uint8(v)
}
