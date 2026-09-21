package main

import (
	"fmt"
	"os"
	"runtime"
	"runtime/debug"
	"sync/atomic"
	"unsafe"
)

// goc TEXT from binary goobj (binwriter WriteObjFile); stubs in stubs_amd64.s (not goc_lower.py).
func GetSP() uintptr
func MorestackHits() uint64
func ForceMorestackOnce() (spBefore, spAfter uintptr)
func HugeFrame(n int) int
func HugeFrameVoid()
func GocCheckedAdd(a, b int64) int64
func StoreGptrWB(slot *uintptr, new uintptr)
func WBPathHits() uint64
func WBEnabled() bool
func GocHoldLive(p *int) int
func GocHoldArg(p *int) int
func GocHoldTwo(p, q *int) int
func GocHoldRegOnly(p *int) int
func GocFadd64(a, b float64) float64
func GocFadd32(a, b float32) float32

//go:linkname writeBarrier runtime.writeBarrier
var writeBarrier struct {
	enabled bool
	pad     [3]byte
	alignme uint64
}

type Box struct {
	P *int
}

func grow(n int) int {
	v := HugeFrame(n)
	sum := GocCheckedAdd(40, 2)
	if sum != 42 {
		panic(fmt.Sprintf("checked add=%d", sum))
	}
	if n > 0 {
		v += grow(n - 1)
	}
	return v
}

//go:noinline
func deep(depth int, p *int, sps *[]uintptr) int {
	*sps = append(*sps, GetSP())
	local := p
	v := GocHoldLive(local)
	if depth > 0 {
		HugeFrameVoid()
		return deep(depth-1, local, sps) + v
	}
	return v
}

//go:noinline
func deepArg(depth int, p *int) int {
	v := GocHoldArg(p)
	if depth > 0 {
		HugeFrameVoid()
		return deepArg(depth-1, p) + v
	}
	return v
}

func main() {
	debug.SetMaxStack(64 << 20)
	fail := 0

	// ========== L ==========
	spBefore, spAfter := ForceMorestackOnce()
	fmt.Printf("L1 ForceMorestackOnce: spBefore=%#x spAfter=%#x hits=%d\n",
		spBefore, spAfter, MorestackHits())
	if MorestackHits() < 1 {
		fmt.Println("FAIL L: ForceMorestackOnce did not hit morestack")
		fail++
	} else {
		fmt.Println("L1 PASS: CALL runtime.morestack_noctxt + JMP reentry")
	}
	_ = grow(20)
	leaf := GocCheckedAdd(40, 2)
	fmt.Printf("L2 GocCheckedAdd after growth: %d hits=%d\n", leaf, MorestackHits())
	if leaf != 42 {
		fmt.Println("FAIL L: leaf != 42")
		fail++
	} else {
		fmt.Println("PASS L: checked entry + MIR/goobj leaf after growth = 42")
	}

	// ========== W before S/A (WB stress independent of hold maps) ==========
	runW := os.Getenv("GOC_P5_SKIP_W") == ""
	if runW {
		a := new(int)
		*a = 11
		b := new(int)
		*b = 22
		box := &Box{P: a}
		slot := (*uintptr)(unsafe.Pointer(&box.P))
		StoreGptrWB(slot, uintptr(unsafe.Pointer(b)))
		runtime.KeepAlive(b)
		if box.P != b || *box.P != 22 {
			fmt.Println("FAIL W: store")
			fail++
		} else {
			fmt.Println("W1 PASS: StoreGptrWB (disabled path)")
		}
		var stop atomic.Uint32
		go func() {
			for stop.Load() == 0 {
				_ = make([]byte, 1<<20)
			}
		}()
		for i := 0; i < 200 && WBPathHits() == 0; i++ {
			runtime.GC()
			for tries := 0; tries < 1_000_000; tries++ {
				if writeBarrier.enabled {
					for j := 0; j < 200; j++ {
						x := new(int)
						*x = j
						StoreGptrWB(slot, uintptr(unsafe.Pointer(x)))
						runtime.KeepAlive(x)
					}
					break
				}
			}
		}
		stop.Store(1)
		runtime.GC()
		if WBPathHits() == 0 {
			fmt.Println("FAIL W: did not exercise gcWriteBarrier2 (optional; set GOC_P5_SKIP_W=1 to skip)")
			fail++
		} else {
			fmt.Printf("PASS W: store_gptr WB enabled path hits=%d\n", WBPathHits())
		}
	} else {
		fmt.Println("SKIP W: GOC_P5_SKIP_W set")
	}

	// ========== S ==========
	heapVal := new(int)
	*heapVal = 7
	var sps []uintptr
	got := deep(25, heapVal, &sps)
	want := 7 + 25*7
	fmt.Printf("S: depth=25 got=%d want=%d sps=%d\n", got, want, len(sps))
	if got != want || *heapVal != 7 {
		fmt.Println("FAIL S: live local gptr")
		fail++
	} else {
		runtime.GC()
		if *heapVal != 7 {
			fmt.Println("FAIL S: heap corrupted after GC")
			fail++
		} else {
			fmt.Println("PASS S: live *int across CALL+morestack with LocalsPointerMaps from pass")
		}
	}

	// ========== A ==========
	argVal := new(int)
	*argVal = 9
	agot := deepArg(20, argVal)
	awant := 9 + 20*9
	fmt.Printf("A: depth=20 got=%d want=%d\n", agot, awant)
	if agot != awant || *argVal != 9 {
		fmt.Println("FAIL A: ArgsPointerMaps")
		fail++
	} else {
		runtime.GC()
		if *argVal != 9 {
			fmt.Println("FAIL A: arg heap corrupted after GC")
			fail++
		} else {
			fmt.Println("PASS A: ArgsPointerMaps keep arg *int across CALL+morestack")
		}
	}

	// ========== S2: two live gptr slots (multi-FI Go SP layout) ==========
	{
		a := new(int)
		*a = 3
		b := new(int)
		*b = 5
		var sps []uintptr
		got := 0
		for depth := 0; depth < 15; depth++ {
			sps = append(sps, GetSP())
			got += GocHoldTwo(a, b)
			HugeFrameVoid()
		}
		want := 15 * (3 + 5)
		fmt.Printf("S2: depth=15 got=%d want=%d sps=%d\n", got, want, len(sps))
		if got != want || *a != 3 || *b != 5 {
			fmt.Println("FAIL S2: two live gptrs / multi-FI layout")
			fail++
		} else {
			runtime.GC()
			if *a != 3 || *b != 5 {
				fmt.Println("FAIL S2: heap corrupted after GC")
				fail++
			} else {
				fmt.Println("PASS S2: two live *int across CALL+morestack (Go SP layout, not FI*8)")
			}
		}
	}

	// ========== S3: register-only gptr (no prior stack store in MIR); needs spill ==========
	{
		heapVal := new(int)
		*heapVal = 11
		var sps []uintptr
		got := 0
		for depth := 0; depth < 20; depth++ {
			sps = append(sps, GetSP())
			got += GocHoldRegOnly(heapVal)
			HugeFrameVoid()
		}
		want := 20 * 11
		fmt.Printf("S3: depth=20 got=%d want=%d sps=%d\n", got, want, len(sps))
		if got != want || *heapVal != 11 {
			fmt.Println("FAIL S3: register-only gptr without spill/maps")
			fail++
		} else {
			runtime.GC()
			if *heapVal != 11 {
				fmt.Println("FAIL S3: heap corrupted after GC")
				fail++
			} else {
				fmt.Println("PASS S3: register-only *int across CALL+morestack (LiveIntervals spill→Locals)")
			}
		}
	}

	// ========== F: float64/float32 via Go ABIInternal XMM (X0,X1 → X0) ==========
	{
		got64 := GocFadd64(1.5, 2.25)
		want64 := 3.75
		got32 := GocFadd32(1.5, 2.25)
		want32 := float32(3.75)
		fmt.Printf("F: fadd64=%v want=%v fadd32=%v want=%v\n", got64, want64, got32, want32)
		if got64 != want64 {
			fmt.Println("FAIL F: GocFadd64 ABIInternal X0/X1")
			fail++
		} else if got32 != want32 {
			fmt.Println("FAIL F: GocFadd32 ABIInternal X0/X1")
			fail++
		} else {
			fmt.Println("PASS F: float64+float32 via llc→elfpack→goobj→Go (X0,X1→X0)")
		}
	}

	if fail > 0 {
		fmt.Printf("FAIL p5-machinepass-goobj (%d)\n", fail)
		os.Exit(1)
	}
	fmt.Println("PASS p5-machinepass-goobj (L+S+S2+S3+A+F[+W])")
}
