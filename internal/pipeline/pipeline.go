// Package pipeline compiles one C file into a Go object: the patched clang,
// the goc IR passes in opt, the lowering to Go ABI thunks, goc-llc,
// llvm-mc, and the converter. It is the only compile pipeline; `goc build`,
// `goc go` and the tests all run it.
//
//	clang -emit-llvm          a.ll
//	goc-color-escape          a.color.ll
//	goc-uptr-inline           a.gate.ll
//	O1-3 (-O>0 only)          a.opt.ll    default<On> minus argpromotion, globalopt
//	goc-stackmap              a.sm.ll     after inlining, on the merged frames
//	goc-reanchor              a.reanchor.ll
//	fix-ir, rename-ir         body.ll
//	Go ABI thunks             body.impl.ll, thunks.s, thunks.goc.s
//	goc-stackmap (-O0 only)   body.sm.ll
//	goc-llc                   body.s, body.goc.s
//	llvm-mc                   body.llc.o  (the .gocobj)
//	convert                   OUT, OUT.symabis
//
// Beside OUT (foo.goobj) it leaves foo.gocobj, foo.symabis and foo.ll (the
// IR goc-llc compiled).
package pipeline

import (
	"bytes"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"

	"github.com/LoyieKing/goc/internal/convert"
	"github.com/LoyieKing/goc/internal/lower"
	"github.com/LoyieKing/goc/internal/toolchain"
)

// Options of one compile. The zero value is `goc build`'s default: -O0,
// linux/amd64, package main, no default pointer color.
type Options struct {
	Src, Out        string
	OptLevel        int    // 0..3
	DefaultPtrColor string // cptr, sptr, uptr, gptr or "" (inferred)
	FastStackAlloca bool
	ColorReport     bool
	Arch            string // amd64 (default) or arm64
	KeepTmp         string // keep the work files in this directory
	OptExtra        []string
	LLCExtra        []string
	Pkg             string // Go symbol prefix (default main)
	IRRenames       string // space-separated old:new
	LibcallImpl     string // space-separated libcall names
	ABI0            bool   // C-side platform entries (a libc shim)
	ClangArgs       []string

	Stdout, Stderr io.Writer
}

// Build runs the pipeline with the tools t.
func Build(t *toolchain.Tools, o Options) error {
	b := &builder{t: t, o: o, env: t.Env(os.Environ())}
	if b.o.Arch == "" {
		b.o.Arch = "amd64"
	}
	if b.o.Pkg == "" {
		b.o.Pkg = "main"
	}
	if b.o.Stdout == nil {
		b.o.Stdout = os.Stdout
	}
	if b.o.Stderr == nil {
		b.o.Stderr = os.Stderr
	}
	return b.run()
}

type builder struct {
	t   *toolchain.Tools
	o   Options
	env []string
	tmp string
}

var amd64Host = regexp.MustCompile(`movq[[:space:]]+%%fs:-8|"syscall"`)

func (b *builder) run() error {
	o := &b.o
	switch o.Arch {
	case "amd64", "arm64":
	default:
		return fmt.Errorf("arch %s (want amd64 or arm64)", o.Arch)
	}
	if o.OptLevel < 0 || o.OptLevel > 3 {
		return fmt.Errorf("-O%d: want 0, 1, 2 or 3", o.OptLevel)
	}
	if o.Out == "" {
		switch {
		case strings.HasSuffix(o.Src, ".c"):
			o.Out = strings.TrimSuffix(o.Src, ".c") + ".goobj"
		case strings.HasSuffix(o.Src, ".goc"):
			o.Out = strings.TrimSuffix(o.Src, ".goc") + ".goobj"
		default:
			o.Out = o.Src + ".goobj"
		}
	}
	if o.Arch == "arm64" {
		// A file that still contains the amd64 host sequence is refused
		// rather than compiled into a binary that reads FS or traps in
		// syscall.
		src, err := os.ReadFile(o.Src)
		if err != nil {
			return err
		}
		if amd64Host.Match(src) {
			return fmt.Errorf("%s is a linux/amd64 host (movq %%%%fs:-8 or syscall) and is not ported to arm64", o.Src)
		}
	}
	if o.KeepTmp != "" {
		if err := os.MkdirAll(o.KeepTmp, 0o755); err != nil {
			return err
		}
		b.tmp = o.KeepTmp
		defer fmt.Fprintf(o.Stderr, "goc build: kept %s\n", b.tmp)
	} else {
		d, err := os.MkdirTemp("", "goc-build")
		if err != nil {
			return err
		}
		b.tmp = d
		defer os.RemoveAll(d)
	}
	body, prepared, err := b.frontend()
	if err != nil {
		return err
	}
	name, _ := lower.FirstFnName(body)
	if name == "" {
		name, _ = lower.FirstFnName(b.path("a.ll"))
	}
	if name == "" {
		return fmt.Errorf("no defined function in %s; refusing a non-goobj LLVM object", o.Src)
	}
	if err := b.backend(body, prepared); err != nil {
		return fmt.Errorf("%v\ngoc build: real-body lower failed; no object emitted", err)
	}
	fmt.Fprintf(o.Stdout, "goc build: wrote %s\n", o.Out)
	return nil
}

func (b *builder) path(name string) string { return filepath.Join(b.tmp, name) }

// tool runs one tool with the clang tree's libLLVM on LD_LIBRARY_PATH.
func (b *builder) tool(bin string, args ...string) error {
	cmd := exec.Command(bin, args...)
	cmd.Env = b.env
	cmd.Stdout = b.o.Stderr
	cmd.Stderr = b.o.Stderr
	if err := cmd.Run(); err != nil {
		return fmt.Errorf("%s: %v", filepath.Base(bin), err)
	}
	return nil
}

func (b *builder) opt(passes string, extra []string, in, out string) error {
	args := []string{"-load-pass-plugin=" + b.t.Passes, "-passes=" + passes}
	args = append(args, extra...)
	return b.tool(b.t.Opt, append(args, "-S", in, "-o", out)...)
}

// frontend: clang, then the IR passes up to the stack maps. It returns the
// IR to lower and whether goc-stackmap already ran on it.
func (b *builder) frontend() (string, bool, error) {
	o := &b.o
	// Go-stack compatible codegen: no red zone, no stack protector, no
	// unwind tables. IR value names do not matter: the passes and goc-llc
	// mark the stack slots they track with metadata (llvm/include/Goc/SlotMD.h).
	args := []string{"-mno-red-zone", "-fno-stack-protector", "-fno-asynchronous-unwind-tables",
		"-I", b.t.Include, "-emit-llvm", "-S", fmt.Sprintf("-O%d", o.OptLevel)}
	if o.ColorReport {
		// Names and declaration lines for bare T* live in debug info.
		args = append(args, "-g")
	} else {
		// File and line rows of the .goc tables (and the inline tree) come
		// from line-table debug locations; they do not change the code.
		args = append(args, "-gline-tables-only")
	}
	if o.OptLevel == 0 {
		args = append(args, "-Xclang", "-disable-O0-optnone")
	} else {
		// Color/escape analysis must see the original pointer stores before
		// SROA and inlining. -disable-llvm-passes keeps this IR uninlined
		// without stamping noinline (which would block the inliner below).
		args = append(args, "-fwrapv", "-fno-strict-aliasing",
			"-fno-omit-frame-pointer", "-mno-omit-leaf-frame-pointer",
			"-fno-optimize-sibling-calls", "-Xclang", "-disable-llvm-passes")
	}
	fmt.Fprintf(o.Stderr, "goc build: clang=%s\n", b.t.Clang)
	if o.Arch == "arm64" {
		// IR datalayout must be aarch64. -ffixed-x28 matches llc's
		// reservation of g. -fno-pic: declarations are dso_local, so llc
		// reaches them with ADRP pairs, not GOT loads.
		args = append(args, "-target", "aarch64-unknown-linux-gnu", "-ffixed-x28", "-fno-pic")
		if st, err := os.Stat("/usr/aarch64-linux-gnu/include"); err == nil && st.IsDir() {
			args = append(args, "-isystem", "/usr/aarch64-linux-gnu/include")
		}
	}
	args = append(args, o.ClangArgs...)
	args = append(args, "-o", b.path("a.ll"), o.Src)
	if err := b.tool(b.t.Clang, args...); err != nil {
		return "", false, fmt.Errorf("Clang Sema/frontend failed for %s", o.Src)
	}

	passes := "goc-color-escape"
	if o.ColorReport {
		passes = "print<goc-color>,goc-color-escape"
	}
	var colorArgs []string
	if o.DefaultPtrColor != "" {
		colorArgs = append(colorArgs, "-default-ptr-color="+o.DefaultPtrColor)
	}
	if err := b.opt(passes, colorArgs, b.path("a.ll"), b.path("a.color.ll")); err != nil {
		return "", false, fmt.Errorf("pointer-color/escape analysis failed for %s", o.Src)
	}
	body := b.path("a.color.ll")
	if o.ColorReport {
		// print<goc-color> already ran. Drop debug info only (no-realign-stack
		// would land after a !dbg attachment, which llc rejects).
		if err := b.tool(b.t.Opt, "--strip-debug", "-S", body, "-o", b.path("a.color.nodbg.ll")); err != nil {
			return "", false, fmt.Errorf("failed to strip debug info after color report for %s", o.Src)
		}
		body = b.path("a.color.nodbg.ll")
	}
	// Every goc_uptr_* / goc_stack_* / goc_dynalloc call gets an internal
	// alwaysinline body (there is no runtime library), at every -O.
	var uptrArgs []string
	if o.FastStackAlloca {
		uptrArgs = append(uptrArgs, "-goc-fast-stack-alloca")
	}
	if err := b.opt("goc-uptr-inline", uptrArgs, body, b.path("a.gate.ll")); err != nil {
		return "", false, err
	}
	body = b.path("a.gate.ll")
	if o.OptLevel == 0 {
		return body, false, nil
	}
	// Maps are recorded after On on the merged frames. Stack maps model
	// SysV outgoing areas; argpromotion and globalopt rewrite internal calls
	// (fastcc, fewer args) and would make those offsets lie. The textual
	// pipeline does not carry default<O3>'s inliner threshold (250; a
	// textual "inline" uses 225), see docs/research/perf-gap.md.
	cmd := exec.Command(b.t.Opt, fmt.Sprintf("-passes=default<O%d>", o.OptLevel),
		"-print-pipeline-passes", "-disable-output", "/dev/null")
	cmd.Env = b.env
	cmd.Stderr = o.Stderr
	out, err := cmd.Output()
	if err != nil {
		return "", false, fmt.Errorf("opt -print-pipeline-passes: %v", err)
	}
	pipe := strings.TrimRight(string(out), "\n")
	pipe = strings.ReplaceAll(pipe, ",argpromotion", "")
	pipe = strings.ReplaceAll(pipe, ",globalopt", "")
	// goc-name-blocks: LLVM 19's .ll reader can merge a numbered and a named
	// blockaddress target; name them before printing.
	pipe += ",function(goc-name-blocks)"
	if err := b.opt(pipe, o.OptExtra, body, b.path("a.opt.ll")); err != nil {
		return "", false, err
	}
	fmt.Fprintf(o.Stderr, "goc build: O%d inlined before stack maps\n", o.OptLevel)
	// Inlining is done. Record maps on the surviving functions, then spill
	// frame addresses On left in SSA across calls. Nothing after this may
	// inline: a caller cannot retarget a callee's frame-word offsets.
	if err := b.opt("goc-stackmap", nil, b.path("a.opt.ll"), b.path("a.sm.ll")); err != nil {
		return "", false, err
	}
	if err := b.opt("goc-reanchor", nil, b.path("a.sm.ll"), b.path("a.reanchor.ll")); err != nil {
		return "", false, err
	}
	return b.path("a.reanchor.ll"), true, nil
}

// backend lowers the IR into the Go object: Go ABI thunks, goc-llc,
// llvm-mc, convert.
func (b *builder) backend(in string, prepared bool) error {
	o := &b.o
	ll := b.path("body.ll")
	if err := copyFile(in, ll); err != nil {
		return err
	}
	// Go's ABI only guarantees 8-byte stack alignment at a call on amd64;
	// fix-ir tells the backend (override-stack-alignment) and gives every
	// defined function "no-realign-stack", so the frame geometry stays fixed
	// and pcsp stays exact (docs/design/codegen.md).
	if err := lower.FixIR(ll, o.Arch); err != nil {
		return err
	}
	src, err := os.ReadFile(ll)
	if err != nil {
		return err
	}
	magic := regexp.MustCompile(`28C0DE42|0x28c0de42|683728450`).Match(src)
	// IR-level redirects for compiler-emitted libc calls that -D macros
	// cannot reach (e.g. @memcpy from struct copies).
	if o.IRRenames != "" {
		if err := lower.RenameIR(ll, o.IRRenames); err != nil {
			return err
		}
	}
	goabi := b.path("goabi.json")
	if err := lower.WriteGoABI(ll, b.path("body.impl.ll"), b.path("thunks.s"), goabi, b.path("thunks.goc.s"), o.Arch); err != nil {
		return err
	}
	if err := copyFile(b.path("body.impl.ll"), ll); err != nil {
		return err
	}
	// Go symbol ABI of the C functions: ABI0 when Go ABI thunks wrap them
	// (the thunk owns the ABIInternal name) or for a C-side object.
	symABI := 1
	if js, _ := os.ReadFile(goabi); o.ABI0 || bytes.Contains(js, []byte(`"name"`)) {
		symABI = 0
	}
	if !prepared {
		// -O0: record the stack-map roots on the IR goc-llc compiles.
		if err := b.opt("goc-stackmap", nil, ll, b.path("body.sm.ll")); err != nil {
			return err
		}
		if st, err := os.Stat(b.path("body.sm.ll")); err != nil || st.Size() == 0 {
			return fmt.Errorf("empty stackmap IR")
		}
		ll = b.path("body.sm.ll")
	}
	// The converter names the ABI profile of the Go in PATH (from that
	// release's facts, internal/gometa/data); goc-llc records it.
	profile, err := convert.Profile(o.Arch)
	if err != nil {
		return err
	}
	args := llcArgs(o.OptLevel, o.Arch)
	args = append(args, o.LLCExtra...)
	// One section per function: every reference between functions then
	// carries an ELF relocation, so the converter never patches code.
	args = append(args, "-function-sections", fmt.Sprintf("-goc-sym-abi=%d", symABI), "-goc-profile="+profile)
	args = append(args, "-goc-tables", b.path("body.goc.s"), "-filetype=asm", "-o", b.path("body.s"), ll)
	if err := b.tool(b.t.LLC, args...); err != nil {
		return err
	}
	triple := "x86_64-unknown-linux-gnu"
	if o.Arch == "arm64" {
		triple = "aarch64-unknown-linux-gnu"
	} else {
		// Without realignment an amd64 frame is only 8-byte aligned, so
		// aligned SSE memory moves would #GP.
		if err := lower.AlignASM(b.path("body.s")); err != nil {
			return err
		}
	}
	// The Go thunks and both table files go into the same assembly, so one
	// relocatable object holds every .text contribution (no linker).
	var asm []byte
	for _, f := range []string{"body.s", "thunks.s", "body.goc.s", "thunks.goc.s"} {
		c, err := os.ReadFile(b.path(f))
		if err != nil {
			return err
		}
		asm = append(asm, c...)
	}
	if err := os.WriteFile(b.path("body.thunks.s"), asm, 0o644); err != nil {
		return err
	}
	elf := b.path("body.llc.o")
	if err := b.tool(b.t.MC, "-filetype=obj", "-triple="+triple, "-o", elf, b.path("body.thunks.s")); err != nil {
		return err
	}
	side := Side(o.Out)
	cargs := []string{"-in", elf, "-o", o.Out, "-p", o.Pkg, "-symabis", side + ".symabis", "-goarch", o.Arch}
	if o.LibcallImpl != "" {
		cargs = append(cargs, "-libcall-impl", o.LibcallImpl)
	}
	if rc := convert.Main(cargs); rc != 0 {
		return fmt.Errorf("convert %s failed", elf)
	}
	if magic {
		if err := lower.ObjectHasMagic(o.Out); err != nil {
			return err
		}
	}
	if err := copyFile(elf, side+".gocobj"); err != nil {
		return err
	}
	if err := copyFile(ll, side+".ll"); err != nil {
		return err
	}
	return nil
}

// llcArgs: per-call Direct locations are the roots, so RBP stays valid at
// every call. Spill slots share (root allocas are not spill slots).
// Call-frame opt would turn a reserved outgoing area into PUSH/POP, so SP
// would not be the constant pcsp claims. Tail merging would hoist a CALL
// shared by two blocks into a common tail and give it another path's
// roots. Tail duplication stays on (QuickJS dispatch blocks are shared
// tails).
func llcArgs(optLevel int, arch string) []string {
	o := fmt.Sprintf("-O%d", optLevel)
	if arch == "arm64" {
		// +reserve-x28 keeps g live. Jump tables stay 4-byte: goc-llc inserts
		// code after the compression pass sized them. Static model: extern
		// data is reached with ADRP+ADD/LDR, not a GOT pair llc may split.
		return []string{o, "-relocation-model=static",
			"-mtriple=aarch64-unknown-linux-gnu", "-mattr=+reserve-x28",
			"-aarch64-enable-compress-jump-tables=false",
			"-frame-pointer=all", "-enable-shrink-wrap=false", "-disable-tail-calls",
			"-enable-tail-merge=false",
			"-tail-dup-pred-size=1000", "-tail-dup-succ-size=1000"}
	}
	return []string{o, "-relocation-model=pic", "-march=x86-64",
		"-frame-pointer=all", "-enable-shrink-wrap=false", "-disable-tail-calls",
		"-no-x86-call-frame-opt", "-enable-tail-merge=false",
		"-tail-dup-pred-size=1000", "-tail-dup-succ-size=1000"}
}

// Side is the stem of an object path: foo.goobj (or foo.o) -> foo. The
// .gocobj, .symabis and .ll of foo.goobj are foo.gocobj, foo.symabis, foo.ll.
func Side(out string) string {
	switch {
	case strings.HasSuffix(out, ".goobj"):
		return strings.TrimSuffix(out, ".goobj")
	case strings.HasSuffix(out, ".o"):
		return strings.TrimSuffix(out, ".o")
	}
	return out
}

func copyFile(src, dst string) error {
	b, err := os.ReadFile(src)
	if err != nil {
		return err
	}
	return os.WriteFile(dst, b, 0o644)
}
