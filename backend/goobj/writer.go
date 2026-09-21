// Legacy entrypoint. P5b primary path is binwriter (Prog → WriteObjFile).
// This wrapper forwards to run_binwriter.sh so old `go run ./goobj/writer.go`
// invocations still produce a binary goobj (not .s + go tool asm).
package main

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
)

func main() {
	root, err := filepath.Abs(filepath.Join(filepath.Dir(os.Args[0]), ".."))
	if err != nil {
		// when go run, Args[0] is in build cache — locate via cwd
		cwd, _ := os.Getwd()
		root = cwd
		if filepath.Base(cwd) == "goobj" {
			root = filepath.Dir(cwd)
		}
	}
	// Prefer script next to this file when go run ./goobj/writer.go from module root
	script := filepath.Join("goobj", "run_binwriter.sh")
	if _, err := os.Stat(script); err != nil {
		script = filepath.Join(root, "goobj", "run_binwriter.sh")
	}
	// Translate common flags: -out-o -maps -recipe -p; ignore -out-s (no longer primary)
	args := make([]string, 0, len(os.Args))
	skipNext := false
	for i, a := range os.Args[1:] {
		if skipNext {
			skipNext = false
			continue
		}
		if a == "-out-s" {
			skipNext = true
			fmt.Println("writer: ignoring -out-s (binary goobj is primary; no .s serialization)")
			_ = i
			continue
		}
		args = append(args, a)
	}
	cmd := exec.Command(script, args...)
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	if err := cmd.Run(); err != nil {
		os.Exit(1)
	}
}
