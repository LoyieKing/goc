// Stack pointers returned through calls stay valid across stack growth.
// Every call runs on a fresh goroutine (minimum stack), so grow() inside
// the C function forces morestack and a copy while the pointer is live.
package main

import (
	"fmt"
	"os"
	"sync"
)

func retptr_pick(k, depth int32) int64
func retptr_identity(k, depth int32) int64

func growModel(n int32) int64 {
	var r int64
	for i := int32(0); i <= n; i++ {
		r += int64(i%600) + 0
	}
	return r
}

// pregrow deepens the Go stack first, so the C call below never moves it:
// the control that separates a stale pointer from a plain miscompile.
//
//go:noinline
func pregrow(n int, f func()) {
	var pad [1024]byte
	if n > 0 {
		pregrow(n-1, f)
	} else {
		f()
	}
	pad[n%1024]++
}

func main() {
	for k := int32(0); k < 8; k++ {
		pregrow(400, func() {
			if got, want := retptr_pick(k, 3), int64(100*8+28+1000)+growModel(3); got != want {
				fmt.Printf("FAIL control (no stack copy) retptr_pick k=%d: got %d want %d\n", k, got, want)
				os.Exit(1)
			}
		})
	}
	fail := false
	var mu sync.Mutex
	var wg sync.WaitGroup
	for k := int32(0); k < 8; k++ {
		for _, depth := range []int32{0, 3, 20, 60} {
			wg.Add(1)
			go func(k, depth int32) {
				defer wg.Done()
				g := growModel(depth)
				if got, want := retptr_pick(k, depth), int64(100*8+28+1000)+g; got != want {
					mu.Lock()
					fmt.Printf("FAIL retptr_pick k=%d depth=%d: got %d want %d\n", k, depth, got, want)
					fail = true
					mu.Unlock()
				}
				if got, want := retptr_identity(k, depth), 2*g; got != want {
					mu.Lock()
					fmt.Printf("FAIL retptr_identity k=%d depth=%d: got %d want %d\n", k, depth, got, want)
					fail = true
					mu.Unlock()
				}
			}(k, depth)
		}
	}
	wg.Wait()
	if fail {
		os.Exit(1)
	}
	fmt.Println("PASS retptr")
}
