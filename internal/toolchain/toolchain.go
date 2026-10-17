// Package toolchain finds the tools goc runs: the patched clang and its
// opt and llvm-mc, goc-llc, and libGocPasses.so. They are found from the
// location of the goc binary only; there is no flag or environment
// variable that points at other tools.
//
// A release unpacks to
//
//	bin/{goc,clang,opt,llvm-mc}  passes/{goc-llc,libGocPasses.so}  lib/  include/
//
// and a checkout keeps bin/goc (go build -o bin/goc ./cmd/goc) next to
//
//	third_party/llvm-19.1.7-clang-build/  build/llvm/{goc-llc,libGocPasses.so}  include/
package toolchain

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
)

// Tools are the absolute paths of one goc tree.
type Tools struct {
	Root    string // the tree: a release or a checkout
	Include string // goc.h, goc_uptr.h
	Clang   string
	LibDir  string // the clang tree's lib/ (libLLVM, the clang resource dir)
	Opt     string
	MC      string
	LLC     string // goc-llc
	Passes  string // libGocPasses.so
}

// Root is the tree the running goc binary belongs to: the parent of the
// directory that holds it.
func Root() (string, error) {
	exe, err := os.Executable()
	if err != nil {
		return "", err
	}
	if exe, err = filepath.EvalSymlinks(exe); err != nil {
		return "", err
	}
	return filepath.Dir(filepath.Dir(exe)), nil
}

// IsRelease reports whether root is an unpacked release (it ships the
// passes in passes/), not a checkout.
func IsRelease(root string) bool {
	_, err := os.Stat(filepath.Join(root, "passes", "goc-llc"))
	return err == nil
}

// Locate returns the tools of root. It does not check that they exist;
// Require does.
func Locate(root string) (*Tools, error) {
	t := &Tools{Root: root, Include: filepath.Join(root, "include")}
	for _, c := range []string{
		filepath.Join(root, "bin", "clang"),
		filepath.Join(root, "third_party", "llvm-19.1.7-clang-build", "bin", "clang"),
	} {
		if isExec(c) {
			t.Clang = c
			break
		}
	}
	if t.Clang == "" {
		return nil, fmt.Errorf("no patched clang next to goc.\n" +
			"Download a release: https://github.com/LoyieKing/goc/releases\n" +
			"Or build the package: docs/build-from-source.md")
	}
	real, err := filepath.EvalSymlinks(t.Clang)
	if err != nil {
		return nil, err
	}
	bin := filepath.Dir(real)
	t.LibDir = filepath.Join(filepath.Dir(bin), "lib")
	t.Opt = filepath.Join(bin, "opt")
	t.MC = filepath.Join(bin, "llvm-mc")
	if IsRelease(root) {
		t.LLC = filepath.Join(root, "passes", "goc-llc")
		t.Passes = filepath.Join(root, "passes", "libGocPasses.so")
	} else {
		t.LLC = filepath.Join(root, "build", "llvm", "goc-llc")
		t.Passes = filepath.Join(root, "build", "llvm", "libGocPasses.so")
	}
	return t, nil
}

// Require is Locate plus a check that every tool is present and that
// goc-llc finds the LLVM libraries of the clang tree.
func Require(root string) (*Tools, error) {
	t, err := Locate(root)
	if err != nil {
		return nil, err
	}
	for _, c := range []struct{ path, what, fix string }{
		{t.Opt, "opt", "See docs/build-from-source.md."},
		{t.MC, "llvm-mc", "See docs/build-from-source.md."},
		{t.LLC, "goc-llc", "Run scripts/build-passes.sh."},
	} {
		if !isExec(c.path) {
			return nil, fmt.Errorf("%s missing (%s). %s", c.what, c.path, c.fix)
		}
	}
	if _, err := os.Stat(t.Passes); err != nil {
		return nil, fmt.Errorf("libGocPasses.so missing (%s). Run scripts/build-passes.sh.", t.Passes)
	}
	cmd := exec.Command("ldd", t.LLC)
	cmd.Env = t.Env(os.Environ())
	if out, err := cmd.Output(); err == nil {
		var missing []string
		for _, l := range strings.Split(string(out), "\n") {
			if strings.Contains(l, "not found") {
				missing = append(missing, strings.Fields(l)[0])
			}
		}
		if len(missing) > 0 {
			sort.Strings(missing)
			return nil, fmt.Errorf("LLVM libraries not loaded (%s). The clang lib dir is on LD_LIBRARY_PATH; libLLVM does not match these passes.", strings.Join(missing, " "))
		}
	}
	return t, nil
}

// Env returns env with the clang tree's lib/ first on LD_LIBRARY_PATH:
// goc-llc and libGocPasses.so load that libLLVM.
func (t *Tools) Env(env []string) []string {
	if st, err := os.Stat(t.LibDir); err != nil || !st.IsDir() {
		return env
	}
	out := make([]string, 0, len(env)+1)
	cur := ""
	for _, kv := range env {
		if v, ok := strings.CutPrefix(kv, "LD_LIBRARY_PATH="); ok {
			cur = v
			continue
		}
		out = append(out, kv)
	}
	for _, d := range strings.Split(cur, ":") {
		if d == t.LibDir {
			return append(out, "LD_LIBRARY_PATH="+cur)
		}
	}
	if cur == "" {
		return append(out, "LD_LIBRARY_PATH="+t.LibDir)
	}
	return append(out, "LD_LIBRARY_PATH="+t.LibDir+":"+cur)
}

func isExec(p string) bool {
	st, err := os.Stat(p)
	return err == nil && !st.IsDir() && st.Mode()&0o111 != 0
}
