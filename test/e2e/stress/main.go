// Stack-growth and GC stress for goc's backend stack check and tables.
//
// Many goroutines start on the minimum stack and recurse through C frames
// of all three check tiers, so every call path goes through morestack and
// the stack is copied many times while C frames hold pointers into it.
// Meanwhile other goroutines churn the heap with a tiny GC target, so GCs
// scan those frames and shrink stacks. The C side checks its frames after
// every return; the Go side checks the result against a Go model.
package main

import (
	"fmt"
	"os"
	"runtime"
	"runtime/debug"
	"sync"
	"sync/atomic"
)

func stress_small(seed int64, n int32) int64
func stress_big(seed int64, n int32) int64
func stress_huge(seed int64, n int32) int64

//go:noescape
func stress_through(p *int64, n int32) int64

// model mirrors the C recursion: level n of a chain entered with value v
// returns model(v+4n+1, n-1) + v+4n+3.
func model(v int64, n int32) int64 {
	var r int64
	for ; n >= 0; n-- {
		r += v + 4*int64(n) + 3
		v += 4*int64(n) + 1
	}
	return r
}

var sink atomic.Pointer[[]byte]

func main() {
	debug.SetGCPercent(1)
	stop := make(chan struct{})
	var churn sync.WaitGroup
	for i := 0; i < 2; i++ {
		churn.Add(1)
		go func() {
			defer churn.Done()
			for {
				select {
				case <-stop:
					return
				default:
				}
				b := make([]byte, 64<<10)
				sink.Store(&b)
				runtime.GC()
			}
		}()
	}

	type tier struct {
		name  string
		f     func(int64, int32) int64
		depth int32
	}
	tiers := []tier{
		{"small", stress_small, 3000},
		{"big", stress_big, 300},
		{"huge", stress_huge, 40},
	}
	var failed atomic.Int32
	var wg sync.WaitGroup
	for round := 0; round < 20; round++ {
		for _, t := range tiers {
			for g := 0; g < 8; g++ {
				wg.Add(1)
				seed := int64(round*1000 + g)
				depth := t.depth - int32(g)*(t.depth/10)
				go func(t tier) {
					defer wg.Done()
					if got, want := t.f(seed, depth), model(seed, depth); got != want {
						fmt.Printf("FAIL stress-%s: seed %d depth %d: got %d want %d\n", t.name, seed, depth, got, want)
						failed.Add(1)
					}
				}(t)
			}
		}
		wg.Add(1)
		go func(seed int64) {
			defer wg.Done()
			v := seed
			if got, want := stress_through(&v, 200), model(seed, 200)+seed; got != want {
				fmt.Printf("FAIL stress-through: got %d want %d\n", got, want)
				failed.Add(1)
			}
		}(int64(round))
		wg.Wait()
	}
	close(stop)
	churn.Wait()
	var ms runtime.MemStats
	runtime.ReadMemStats(&ms)
	if failed.Load() != 0 {
		os.Exit(1)
	}
	fmt.Printf("PASS stress (%d GCs)\n", ms.NumGC)
}
