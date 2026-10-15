// gojacli: minimal Goja runner for docs/benchmark.md.
// Usage: gojacli [--micro] file.js
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
	micro := false
	if len(args) > 0 && args[0] == "--micro" {
		micro = true
		args = args[1:]
	}
	if len(args) < 1 {
		fmt.Fprintln(os.Stderr, "usage: gojacli [--micro] file.js")
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
