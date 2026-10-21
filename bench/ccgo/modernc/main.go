// Command modernc runs one script file on modernc.org/quickjs, the upstream
// ccgo-translated Bellard QuickJS (pure Go, no cgo), for bench/v8multi.sh.
// print and console.log join their arguments with spaces, as qjs does.
package main

import (
	"fmt"
	"os"

	quickjs "modernc.org/quickjs"
)

func main() {
	src, err := os.ReadFile(os.Args[1])
	if err != nil {
		panic(err)
	}
	vm, err := quickjs.NewVM()
	if err != nil {
		panic(err)
	}
	defer vm.Close()
	vm.SetMaxStackSize(16 << 20)
	if err := vm.RegisterFunc("__p", func(s string) { fmt.Println(s) }, false); err != nil {
		panic(err)
	}
	if _, err := vm.Eval("globalThis.console={log:(...a)=>__p(a.join(' '))};globalThis.print=console.log;", quickjs.EvalGlobal); err != nil {
		panic(err)
	}
	if _, err := vm.Eval(string(src), quickjs.EvalGlobal); err != nil {
		fmt.Fprintln(os.Stderr, "error:", err)
		os.Exit(1)
	}
}
