// perf-gap mechanism microbenchmarks: Go driver for the goc build of mech.c.
// Same table and output format as mech_native_main.c.
package main

import (
	"fmt"
	"os"
	"strconv"
	"time"
)

func mech_leafcalls(n int64) int64
func mech_fib(n int64) int64
func mech_localaddr(n int64) int64
func mech_interp(n int64) int64
func mech_chase(n int64) int64
func mech_indirect(n int64) int64

func main() {
	rounds := 5
	if len(os.Args) > 1 {
		rounds, _ = strconv.Atoi(os.Args[1])
	}
	bs := []struct {
		name string
		f    func(int64) int64
		n    int64
		div  float64
	}{
		{"leafcalls", mech_leafcalls, 200000000, 200000000},
		{"fib", mech_fib, 32, 7049155},
		{"localaddr", mech_localaddr, 200000000, 200000000},
		{"interp", mech_interp, 1600000, 409600000},
		{"chase", mech_chase, 200000000, 200000000},
		{"indirect", mech_indirect, 200000000, 200000000},
	}
	for _, b := range bs {
		if len(os.Args) > 2 && os.Args[2] != b.name {
			continue
		}
		best := time.Duration(1 << 62)
		var r int64
		for k := 0; k < rounds; k++ {
			t0 := time.Now()
			r = b.f(b.n)
			if t := time.Since(t0); t < best {
				best = t
			}
		}
		fmt.Printf("%s ns/op=%.4f result=%d\n", b.name, float64(best.Nanoseconds())/b.div, r)
	}
}
