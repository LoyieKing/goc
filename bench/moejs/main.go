// Command moejs runs one classic script on github.com/Calcium-Ion/moejs, a
// JavaScript engine written in pure Go (its own parser, bytecode compiler and
// interpreter), for bench/v8multi.sh and the test262 sample.
//
//	moejs file.js
//
// Globals: print and console.log (arguments converted with ToString and
// joined with spaces, as qjs does) and performance.now (Go monotonic clock,
// milliseconds), the same set the Goja runner has.
//
// The runtime gets its own mutable copy of the builtins
// (Options.MutableIntrinsics): by default moejs shares deeply frozen builtins
// between runtimes, and a benchmark or test harness that assigns to a builtin
// (V8's base.js replaces Math.random) would throw a TypeError. Dynamic code
// (eval, Function) has no length limit, as in the other engines' shells.
package main

import (
	"bufio"
	"errors"
	"fmt"
	"os"
	"strings"
	"time"

	"github.com/Calcium-Ion/moejs"
)

func main() {
	if len(os.Args) != 2 {
		fmt.Fprintln(os.Stderr, "usage: moejs file.js")
		os.Exit(2)
	}
	src, err := os.ReadFile(os.Args[1])
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}
	out := bufio.NewWriter(os.Stdout)
	defer out.Flush()

	rt := moejs.NewRuntime(moejs.Options{MutableIntrinsics: true, MaxDynamicSource: -1})
	printFn := moejs.NativeFunc(func(r *moejs.Realm, _ moejs.Value, args []moejs.Value) (moejs.Value, error) {
		parts := make([]string, len(args))
		for i, a := range args {
			s, err := r.ToString(a)
			if err != nil {
				return moejs.Undefined(), err
			}
			parts[i] = s.GoString()
		}
		out.WriteString(strings.Join(parts, " "))
		out.WriteByte('\n')
		return moejs.Undefined(), out.Flush()
	})
	t0 := time.Now()
	globals := map[string]any{
		"print":   printFn,
		"console": map[string]any{"log": printFn},
		"performance": map[string]any{
			"now": moejs.NativeFunc(func(*moejs.Realm, moejs.Value, []moejs.Value) (moejs.Value, error) {
				return moejs.Number(float64(time.Since(t0).Nanoseconds()) / 1e6), nil
			}),
		},
	}
	for name, v := range globals {
		if err := rt.SetGlobal(name, v); err != nil {
			fail(rt, err)
		}
	}
	script, err := moejs.CompileScript(os.Args[1], string(src))
	if err != nil {
		fail(rt, err)
	}
	if _, err := rt.RunScript(script); err != nil {
		fail(rt, err)
	}
}

func fail(rt *moejs.Runtime, err error) {
	var exc *moejs.Exception
	if errors.As(err, &exc) {
		fmt.Fprintln(os.Stderr, "error:", rt.StackTrace(exc))
	} else {
		fmt.Fprintln(os.Stderr, "error:", err)
	}
	os.Stdout.Sync()
	os.Exit(1)
}
