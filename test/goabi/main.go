// P29 golden: Go calls clang-compiled C through goc ABIInternal entry thunks.
//
// The C bodies live in a packed goobj: TEXT symbols are the Go-ABI entry thunk
// (<name>) plus the SysV body (<name>.impl). stubs_amd64.s only satisfies Go's
// "bodyless func needs assembly in the package" rule.
package main

import (
	"fmt"
	"os"
)

func GoabiMarker() uint64

// C: int goabi_add2(int a, int b)
func goabi_add2(a, b int32) int32

// C: int goabi_stack(int n) — internal call + stack locals
func goabi_stack(n int32) int32

// C: int goabi_store(sptr(int) out, int v)
//go:noescape
func goabi_store(out *int32, v int32) int32

// C: long goabi_addl(long, long, long, long, long, long)
func goabi_addl(a, b, c, d, e, f int64) int64

func goabi_add7(a, b, c, d, e, f, g int64) int64

func goabi_add10(a, b, c, d, e, f, g, h, i, j int64) int64

func goabi_float_mix(a float64, b int64, c float32, d float64) float64

func goabi_float9(a, b, c, d, e, f, g, h, i float64) float64

func goabi_mixed_stack(a, b, c, d, e, f, g int64,
	h, i, j, k, l, m, n, o, p float64) int64

type goabiPair struct {
	lo uint64
	hi uint64
}

func goabi_pair(a, b uint64) goabiPair

type goabiDoubleTag struct {
	value float64
	tag   int64
}

func goabi_double_tag(value float64, tag int64) goabiDoubleTag

type goabiTagDouble struct {
	tag   int64
	value float64
}

func goabi_tag_double(tag int64, value float64) goabiTagDouble

type goabiTwoDoubles struct {
	lo float64
	hi float64
}

func goabi_two_doubles(a, b float64) goabiTwoDoubles

func goabi_movable_local() int32

func goabi_stack_pointer_argument() int32

func goabi_register_pointer_argument() int32

var failed bool

func check(name string, got, want int64) {
	if got != want {
		fmt.Printf("FAIL %s: got %d want %d\n", name, got, want)
		failed = true
		return
	}
	fmt.Printf("PASS %s: %d\n", name, got)
}

func main() {
	if m := GoabiMarker(); m != 0x670ab1 {
		fmt.Printf("FAIL goabi-marker: 0x%x\n", m)
		failed = true
	}
	check("goabi-add2", int64(goabi_add2(3, 4)), 7)
	// buf[i]=i; buf[5]=5; buf[0]+buf[1] (=1) + goabi_add2(1,1) (=2)
	check("goabi-stack", int64(goabi_stack(5)), 3)
	var x int32
	check("goabi-store", int64(goabi_store(&x, 42)), 84)
	check("goabi-store-value", int64(x), 42)
	storeDone := make(chan int64, 1)
	go func() {
		var local int32
		result := goabi_store(&local, 42)
		storeDone <- int64(result) + int64(local)
	}()
	check("goabi-stack-argument-after-growth", <-storeDone, 126)
	check("goabi-addl6", goabi_addl(1, 2, 3, 4, 5, 6), 21)
	check("goabi-add7", goabi_add7(1, 2, 3, 4, 5, 6, 7), 140)
	check("goabi-add10", goabi_add10(1, 2, 3, 4, 5, 6, 7, 8, 9, 10), 385)
	if got := goabi_float_mix(1.5, 2, 3, 4.5); got != 32 {
		fmt.Printf("FAIL goabi-float-mix: got %g want 32\n", got)
		failed = true
	} else {
		fmt.Printf("PASS goabi-float-mix: %g\n", got)
	}
	if got := goabi_float9(1, 2, 3, 4, 5, 6, 7, 8, 9); got != 285 {
		fmt.Printf("FAIL goabi-float9: got %g want 285\n", got)
		failed = true
	} else {
		fmt.Printf("PASS goabi-float9: %g\n", got)
	}
	check("goabi-mixed-stack", goabi_mixed_stack(1, 2, 3, 4, 5, 6, 7,
		1, 2, 3, 4, 5, 6, 7, 8, 9), 425)
	pair := goabi_pair(7, 3)
	check("goabi-pair-lo", int64(pair.lo), 10)
	check("goabi-pair-hi", int64(pair.hi), 13)
	if r := goabi_double_tag(1.25, 30); r.value != 1.75 || r.tag != 37 {
		fmt.Printf("FAIL goabi-double-tag: got {%g, %d} want {1.75, 37}\n", r.value, r.tag)
		failed = true
	} else {
		fmt.Println("PASS goabi-double-tag")
	}
	if r := goabi_tag_double(40, 4.25); r.tag != 42 || r.value != 7.25 {
		fmt.Printf("FAIL goabi-tag-double: got {%d, %g} want {42, 7.25}\n", r.tag, r.value)
		failed = true
	} else {
		fmt.Println("PASS goabi-tag-double")
	}
	if r := goabi_two_doubles(2.5, 4); r.lo != 6.5 || r.hi != 10 {
		fmt.Printf("FAIL goabi-two-doubles: got {%g, %g} want {6.5, 10}\n", r.lo, r.hi)
		failed = true
	} else {
		fmt.Println("PASS goabi-two-doubles")
	}
	moved := make(chan int32, 1)
	go func() { moved <- goabi_movable_local() }()
	check("goabi-sptr-after-stack-copy", int64(<-moved), 3)
	go func() { moved <- goabi_stack_pointer_argument() }()
	check("goabi-stack-passed-pointer-after-copy", int64(<-moved), 3)
	go func() { moved <- goabi_register_pointer_argument() }()
	check("goabi-register-pointer-after-copy", int64(<-moved), 3)
	if failed {
		fmt.Println("FAIL p29-goabi")
		os.Exit(1)
	}
	fmt.Println("PASS p29-goabi (Go ABIInternal scalar and aggregate thunks)")
}
