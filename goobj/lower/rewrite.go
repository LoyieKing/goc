package lower

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"os"
	"regexp"
	"strings"
)

// FixIR stamps the amd64 Go stack alignment onto LLVM IR and marks every
// defined function no-realign-stack. arm64 Go already keeps a 16-byte SP,
// so only the attribute is added there.
func FixIR(path, arch string) error {
	b, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	ll := string(b)
	if arch == "" {
		arch = "amd64"
	}
	if arch != "arm64" && !strings.Contains(ll, "override-stack-alignment") {
		if strings.Contains(ll, "!llvm.module.flags = !{") {
			ll = strings.Replace(ll, "!llvm.module.flags = !{", "!llvm.module.flags = !{!9000, ", 1)
		} else {
			ll += "\n!llvm.module.flags = !{!9000}\n"
		}
		ll += "\n!9000 = !{i32 1, !\"override-stack-alignment\", i32 8}\n"
		fmt.Println("realbody: override-stack-alignment=8 (no aligned SSE on the C stack)")
	}
	defRE := regexp.MustCompile(`(?m)^(define [^\n]*)\{$`)
	ll = defRE.ReplaceAllStringFunc(ll, func(s string) string {
		if strings.Contains(s, `"no-realign-stack"`) {
			return strings.TrimSuffix(s, "{")
		}
		return strings.TrimSuffix(s, "{") + `"no-realign-stack" {`
	})
	return os.WriteFile(path, []byte(ll), 0o644)
}

// RenameIR rewrites @old to @new for each "old:new" pair in spec.
func RenameIR(path, spec string) error {
	b, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	text := string(b)
	for _, pair := range strings.Fields(spec) {
		old, neu, ok := strings.Cut(pair, ":")
		if !ok {
			return fmt.Errorf("realbody: IR rename %q is not old:new", pair)
		}
		text = replaceAtName(text, old, neu)
	}
	return os.WriteFile(path, []byte(text), 0o644)
}

var alignOK = regexp.MustCompile(`^(movs[sd]|mov[lh]p[sd]|movu|movq|movd|cvt|ucomis|comis|cmp[a-z]*s[sd]$|(add|sub|mul|div|sqrt|min|max)s[sd]$|pinsr|pextr)`)

// AlignASM rewrites aligned SSE memory moves to their unaligned forms and
// refuses any other aligned XMM memory operand.
func AlignASM(path string) error {
	b, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	text := string(b)
	for _, pair := range [][2]string{{"movaps", "movups"}, {"movapd", "movupd"}, {"movdqa", "movdqu"}} {
		re := regexp.MustCompile(`(?m)^(\s*)` + pair[0] + `(\s[^\n]*\()`)
		text = re.ReplaceAllString(text, "${1}"+pair[1]+"${2}")
	}
	for _, line := range strings.Split(text, "\n") {
		s := strings.TrimSpace(line)
		if strings.Contains(s, "%xmm") && strings.Contains(s, "(") && !strings.Contains(s, "(%rip)") && !strings.HasPrefix(s, "#") && !strings.HasPrefix(s, ".") {
			op := strings.Fields(s)[0]
			if !alignOK.MatchString(op) {
				return fmt.Errorf("realbody: FATAL alignment-requiring SSE memory operand on an 8-byte-aligned Go stack: %s", s)
			}
		}
	}
	return os.WriteFile(path, []byte(text), 0o644)
}

var fnNameRE = regexp.MustCompile(`define [^@]*@([A-Za-z_][A-Za-z0-9_]*)`)

// FirstFnName is the first LLVM define name, using the same pattern cmd/goc
// used to ask ripgrep for.
func FirstFnName(path string) (string, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return "", err
	}
	m := fnNameRE.FindSubmatch(b)
	if m == nil {
		return "", nil
	}
	return string(m[1]), nil
}

// HasDefine reports whether IR contains `define … @name`, same as the old
// ripgrep check on the single-function path.
func HasDefine(path, name string) (bool, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return false, err
	}
	re := regexp.MustCompile(`define .* @` + regexp.QuoteMeta(name))
	return re.Match(b), nil
}

var magicRE = regexp.MustCompile(`28C0DE42|0x28c0de42|683728450`)

// IRHasMagic reports the P28 source-body sentinel in LLVM IR.
func IRHasMagic(path string) (bool, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return false, err
	}
	return magicRE.Match(b), nil
}

// ObjectHasMagic requires the sentinel as a little-endian u32 in the goobj.
func ObjectHasMagic(path string) error {
	b, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	pat := make([]byte, 4)
	binary.LittleEndian.PutUint32(pat, 0x28C0DE42)
	if !bytes.Contains(b, pat) {
		return fmt.Errorf("FAIL: magic 0x28C0DE42 not in goobj TEXT — body not from source")
	}
	fmt.Println("P28-proof: magic 0x28C0DE42 present in goobj (real ISel body)")
	return nil
}

// CheckMeta rejects a sidecar that is not the real-ISel producer.
func CheckMeta(path string) error {
	b, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	if !bytes.Contains(b, []byte(`"encoding": "clang-real-isel"`)) {
		return fmt.Errorf("realbody: meta is missing clang-real-isel encoding")
	}
	if !bytes.Contains(b, []byte("not_source")) {
		return fmt.Errorf("realbody: meta is missing not_source")
	}
	if bytes.Contains(b, []byte("attrs→seedMIR")) || bytes.Contains(b, []byte("seedMIR→Spill")) {
		return fmt.Errorf("FATAL: meta looks like P21 seedMIR pipeline")
	}
	return nil
}

// SymABIs prints one Go symabis line per TEXT in the sidecar.
func SymABIs(path string) error {
	b, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	var m struct {
		ABI       string `json:"abi"`
		Functions []struct {
			GoSym string `json:"go_sym"`
		} `json:"functions"`
	}
	if err := json.Unmarshal(b, &m); err != nil {
		return err
	}
	abi := m.ABI
	if abi == "" {
		abi = "ABIInternal"
	}
	for _, f := range m.Functions {
		fmt.Printf("def %s %s\n", f.GoSym, abi)
	}
	return nil
}

// TextCount is the number of defined TEXT symbols in `go tool nm` output.
func TextCount(nm string) int {
	n := 0
	for _, line := range strings.Split(nm, "\n") {
		if strings.Contains(line, " T ") {
			n++
		}
	}
	return n
}

// HasExternalHook reports the P28 indirect-call fixture in nm or objdump text.
func HasExternalHook(nm, dis string) bool {
	return strings.Contains(nm, "U p28_external_hook") || strings.Contains(dis, "p28_external_hook")
}
