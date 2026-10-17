// goja: the Goja runner of the benchmarks (docs/benchmark).
//
//	goja [--micro] file.js   run a script
//	goja -mem N script.js    multi-instance memory probe (mem.go)
//
// Globals: print, console.log, performance.now (Go monotonic clock, ms).
// --micro aliases Date.prototype.toGMTString to toUTCString (Goja lacks it and
// Bellard's microbench.js date_parse needs it).
package main

import (
	"fmt"
	"os"
	"strings"
	"time"

	"github.com/dop251/goja"
)

func main() {
	args := os.Args[1:]
	if len(args) > 0 && args[0] == "-mem" {
		memMain(args[1:])
		return
	}
	micro := false
	if len(args) > 0 && args[0] == "--micro" {
		micro = true
		args = args[1:]
	}
	if len(args) < 1 {
		fmt.Fprintln(os.Stderr, "usage: goja [--micro] file.js | goja -mem N script.js")
		os.Exit(2)
	}
	src, err := os.ReadFile(args[0])
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}
	vm := goja.New()
	out := func(call goja.FunctionCall) goja.Value {
		parts := make([]string, len(call.Arguments))
		for i, a := range call.Arguments {
			parts[i] = a.String()
		}
		fmt.Println(strings.Join(parts, " "))
		return goja.Undefined()
	}
	vm.Set("print", out)
	con := vm.NewObject()
	con.Set("log", out)
	vm.Set("console", con)
	t0 := time.Now()
	perf := vm.NewObject()
	perf.Set("now", func() float64 { return float64(time.Since(t0).Nanoseconds()) / 1e6 })
	vm.Set("performance", perf)
	vm.Set("scriptArgs", []interface{}{args[0]})
	if micro {
		if _, err := vm.RunString(`Date.prototype.toGMTString = Date.prototype.toUTCString;`); err != nil {
			panic(err)
		}
	}
	if _, err := vm.RunScript(args[0], string(src)); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
