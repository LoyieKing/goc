// Golden: C → Go via __go_extern stubs, including a Go callee that grows the stack.
package main

import (
	"fmt"
	"os"
)

func GoExternMarker() uint64

// C probes that call Go through stubs.
func go_extern_probe(a, b int64) int64

// Go callees imported by C with __go_extern.
func go_extern_add(a, b int64) int64 { return a + b }

func go_extern_pair(a, b int64) (int64, int64) { return a + 1, b + 2 }

// go_extern_grow forces stack growth so the stub's pcsp is exercised.
//
//go:noinline
func go_extern_grow(n int64) int64 {
	if n <= 0 {
		return 1
	}
	var buf [256]byte
	buf[n&255] = byte(n)
	return int64(buf[n&255]) + go_extern_grow(n-1)
}

func main() {
	got := go_extern_probe(10, 20)
	// sum=30, pair=(11,22), grow(32)=32+31+...+1 = 32*33/2 = 528 → total 30+11+22+528=591
	want := int64(592)
	if got != want {
		fmt.Printf("FAIL go_extern: got %d want %d\n", got, want)
		os.Exit(1)
	}
	if GoExternMarker() == 0 {
		fmt.Printf("FAIL go_extern: marker\n")
		os.Exit(1)
	}
	fmt.Println("PASS go_extern")
}
