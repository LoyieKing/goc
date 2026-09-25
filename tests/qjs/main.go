// P29 north-star smoke: quickjs-ng (colored, compiled by goc) executing on a
// goroutine stack, called from Go through goc ABIInternal entry thunks.
//
// The QJS bodies live in the packed goobj (build/qjs/qjs.o): TEXT symbols are
// Go-ABI thunks (main.JS_*) + SysV bodies (main.JS_*.impl). stubs_amd64.s only
// satisfies Go's "bodyless func needs assembly in the package" rule.
package main

import (
	"fmt"
	"math"
	"os"
	"runtime"
	"strconv"
	"time"
	"unsafe"
)

// cgo import forces external linking so the packed quickjs-ng object resolves
// its libc symbols (malloc/memcpy/printf/...). QJS keeps using its own C heap.

/*
#include <stdlib.h>
*/
import "C"

// quickjs-ng entry points (Go-ABI thunk over the clang-compiled body):
//
//	const char *JS_GetVersion(void)
//	JSRuntime *JS_NewRuntime(void)
//	void JS_FreeRuntime(JSRuntime *rt)
//
//go:linkname JS_GetVersion main.JS_GetVersion
//go:linkname JS_NewRuntime main.JS_NewRuntime
//go:linkname JS_FreeRuntime main.JS_FreeRuntime
//go:linkname JS_NewContext main.JS_NewContext
//go:linkname JS_FreeContext main.JS_FreeContext
//go:linkname JS_Eval main.JS_Eval
//go:linkname JS_EvalFunction main.JS_EvalFunction
//go:linkname JS_GetException main.JS_GetException
//go:linkname JS_ToCStringLen2 main.JS_ToCStringLen2
//go:linkname JS_FreeCString main.JS_FreeCString
//go:linkname JS_FreeValue main.JS_FreeValue
//go:linkname goc_qjs_install_promise_hook main.goc_qjs_install_promise_hook
//go:linkname goc_qjs_promise_parent_hooks main.goc_qjs_promise_parent_hooks
//go:linkname goc_qjs_link_copy_probe main.goc_qjs_link_copy_probe
//go:linkname goc_qjs_frame_chain_probe main.goc_qjs_frame_chain_probe
func JS_GetVersion() *byte
func JS_NewRuntime() unsafe.Pointer
func JS_FreeRuntime(rt unsafe.Pointer)
func JS_NewContext(rt unsafe.Pointer) unsafe.Pointer
func JS_FreeContext(ctx unsafe.Pointer)
func JS_Eval(ctx unsafe.Pointer, input *byte, inputLen uint64, filename *byte, flags int32) jsValue
func JS_EvalFunction(ctx unsafe.Pointer, code jsValue) jsValue
func JS_GetException(ctx unsafe.Pointer) jsValue
func JS_ToCStringLen2(ctx unsafe.Pointer, length *uint64, val jsValue, cesu8 bool) *byte
func JS_FreeCString(ctx unsafe.Pointer, str *byte)
func JS_FreeValue(ctx unsafe.Pointer, val jsValue)
func goc_qjs_install_promise_hook(rt unsafe.Pointer)
func goc_qjs_promise_parent_hooks() int32
func goc_qjs_link_copy_probe() int32
func goc_qjs_frame_chain_probe(heapFrame int32) int32

// JS_NAN_BOXING=0 returns the pointer/scalar word and tag in a 16-byte value.
// The goc ABIInternal thunk maps SysV RAX:RDX to Go's AX:BX struct result.
type jsValue struct {
	payload uint64
	tag     int64
}

const (
	jsEvalGlobal  = 0
	jsCompileOnly = 1 << 5
)

func cstr(p *byte) string {
	if p == nil {
		return "<nil>"
	}
	b := make([]byte, 0, 32)
	for q := p; *q != 0 && len(b) < 64; q = (*byte)(unsafe.Add(unsafe.Pointer(q), 1)) {
		b = append(b, *q)
	}
	return string(b)
}

var failed bool

//go:noinline
func gocGoMathPow(x, y float64) float64 { return math.Pow(x, y) }

//go:noinline
func gocGoMathSqrt(x float64) float64 { return math.Sqrt(x) }

//go:noinline
func gocGoMathTrunc(x float64) float64 { return math.Trunc(x) }

//go:noinline
func gocGoMathFloor(x float64) float64 { return math.Floor(x) }

//go:noinline
func gocGoMathCeil(x float64) float64 { return math.Ceil(x) }

//go:noinline
func gocGoMathHypot(x, y float64) float64 { return math.Hypot(x, y) }

//go:noinline
func gocGoMathMod(x, y float64) float64 { return math.Mod(x, y) }

//go:noinline
func gocGoMathRound(x float64) float64 { return math.Round(x) }

//go:noinline
func gocGoMathAcos(x float64) float64 { return math.Acos(x) }

//go:noinline
func gocGoMathAcosh(x float64) float64 { return math.Acosh(x) }

//go:noinline
func gocGoMathAsin(x float64) float64 { return math.Asin(x) }

//go:noinline
func gocGoMathAsinh(x float64) float64 { return math.Asinh(x) }

//go:noinline
func gocGoMathAtan(x float64) float64 { return math.Atan(x) }

//go:noinline
func gocGoMathAtan2(y, x float64) float64 { return math.Atan2(y, x) }

//go:noinline
func gocGoMathAtanh(x float64) float64 { return math.Atanh(x) }

//go:noinline
func gocGoMathCbrt(x float64) float64 { return math.Cbrt(x) }

//go:noinline
func gocGoMathCos(x float64) float64 { return math.Cos(x) }

//go:noinline
func gocGoMathCosh(x float64) float64 { return math.Cosh(x) }

//go:noinline
func gocGoMathExp(x float64) float64 { return math.Exp(x) }

//go:noinline
func gocGoMathExpm1(x float64) float64 { return math.Expm1(x) }

//go:noinline
func gocGoMathLog(x float64) float64 { return math.Log(x) }

//go:noinline
func gocGoMathLog10(x float64) float64 { return math.Log10(x) }

//go:noinline
func gocGoMathLog1p(x float64) float64 { return math.Log1p(x) }

//go:noinline
func gocGoMathLog2(x float64) float64 { return math.Log2(x) }

//go:noinline
func gocGoMathSin(x float64) float64 { return math.Sin(x) }

//go:noinline
func gocGoMathSinh(x float64) float64 { return math.Sinh(x) }

//go:noinline
func gocGoMathTan(x float64) float64 { return math.Tan(x) }

//go:noinline
func gocGoMathTanh(x float64) float64 { return math.Tanh(x) }

//go:noinline
func gocGoMathRoundToEven(x float64) int64 { return int64(math.RoundToEven(x)) }

//go:noinline
func gocGoMathFrexp(x float64) (float64, int64) {
	fraction, exponent := math.Frexp(x)
	return fraction, int64(exponent)
}

//go:noinline
func gocGoMathLdexp(x float64, exponent int32) float64 { return math.Ldexp(x, int(exponent)) }

//go:noinline
func gocGoMathModf(x float64) (float64, float64) { return math.Modf(x) }

//go:noinline
func gocGoParseFloat(p *byte, length uint64) float64 {
	value, _ := strconv.ParseFloat(unsafe.String(p, int(length)), 64)
	return value
}

type gocTM struct {
	Sec, Min, Hour, Mday, Mon, Year, Wday, Yday, Isdst int32
	_                                                  int32
	GmtOff                                             int64
	Zone                                               *byte
}

//go:noinline
func gocGoLocaltime(seconds int64, out *gocTM, zone *byte) {
	t := time.Unix(seconds, 0).Local()
	name, offset := t.Zone()
	out.Sec, out.Min, out.Hour = int32(t.Second()), int32(t.Minute()), int32(t.Hour())
	out.Mday, out.Mon, out.Year = int32(t.Day()), int32(t.Month()-1), int32(t.Year()-1900)
	out.Wday, out.Yday = int32(t.Weekday()), int32(t.YearDay()-1)
	if t.IsDST() {
		out.Isdst = 1
	} else {
		out.Isdst = 0
	}
	out.GmtOff, out.Zone = int64(offset), zone
	for i := 0; i < 63; i++ {
		if i < len(name) {
			*(*byte)(unsafe.Add(unsafe.Pointer(zone), i)) = name[i]
		} else {
			*(*byte)(unsafe.Add(unsafe.Pointer(zone), i)) = 0
			break
		}
	}
	*(*byte)(unsafe.Add(unsafe.Pointer(zone), 63)) = 0
}

func checkEval(ctx unsafe.Pointer, label string, result jsValue) {
	if result.tag == 0 && int32(result.payload) == 7 {
		fmt.Printf("PASS %s: 7\n", label)
		JS_FreeValue(ctx, result)
		return
	}
	fmt.Printf("FAIL %s: lo=%#x tag=%#x\n", label, result.payload, result.tag)
	if result.tag == 6 {
		exc := JS_GetException(ctx)
		text := JS_ToCStringLen2(ctx, nil, exc, false)
		fmt.Printf("%s exception: %s\n", label, cstr(text))
		if text != nil {
			JS_FreeCString(ctx, text)
		}
		JS_FreeValue(ctx, exc)
	} else {
		JS_FreeValue(ctx, result)
	}
	failed = true
}

func main() {
	fmt.Println("MARK start")
	if v := cstr(JS_GetVersion()); v == "" {
		fmt.Println("FAIL qjs-version: empty")
		failed = true
	} else {
		fmt.Printf("PASS qjs-version: %s\n", v)
	}

	rt := JS_NewRuntime()
	if rt == nil {
		fmt.Println("FAIL qjs-newruntime: nil")
		failed = true
	} else {
		fmt.Printf("PASS qjs-newruntime: %p\n", rt)
		moved := make(chan int32)
		go func() { moved <- goc_qjs_link_copy_probe() }()
		if got := <-moved; got != 3 {
			fmt.Printf("FAIL qjs-link-copy-after-growth: %d\n", got)
			failed = true
		} else {
			fmt.Println("PASS qjs-link-copy-after-growth: 3")
		}
		for _, heap := range []int32{0, 1} {
			go func(useHeap int32) { moved <- goc_qjs_frame_chain_probe(useHeap) }(heap)
			if got := <-moved; got != 3 {
				fmt.Printf("FAIL qjs-frame-chain-after-growth (heap=%d): %d\n", heap, got)
				failed = true
			} else {
				fmt.Printf("PASS qjs-frame-chain-after-growth (heap=%d): 3\n", heap)
			}
		}
		if os.Getenv("QJS_EVAL") == "1" {
			ctx := JS_NewContext(rt)
			if ctx == nil {
				fmt.Println("FAIL qjs-newcontext: nil")
				failed = true
			} else {
				fmt.Printf("qjs-context probe: %p\n", ctx)
				source := "1+2*3"
				script := append([]byte(source), 0) // QuickJS reads input[input_len]
				filename := []byte("<qjs-smoke>\x00")
				result := JS_Eval(ctx, &script[0], uint64(len(source)), &filename[0], jsEvalGlobal)
				runtime.KeepAlive(script)
				runtime.KeepAlive(filename)
				checkEval(ctx, "qjs-eval", result)
				if os.Getenv("QJS_PROMISE") == "1" {
					goc_qjs_install_promise_hook(rt)
					promiseSource := "Promise.resolve(1).then(x => x + 6); 1+2*3"
					promiseScript := append([]byte(promiseSource), 0)
					compiled := JS_Eval(ctx, &promiseScript[0], uint64(len(promiseSource)),
						&filename[0], jsEvalGlobal|jsCompileOnly)
					runtime.KeepAlive(promiseScript)
					if compiled.tag == 6 {
						checkEval(ctx, "qjs-promise-eval", compiled)
					} else {
						checkEval(ctx, "qjs-promise-eval", JS_EvalFunction(ctx, compiled))
					}
					if n := goc_qjs_promise_parent_hooks(); n == 0 {
						fmt.Println("FAIL qjs-promise-hook: no parent promise")
						failed = true
					} else {
						fmt.Printf("PASS qjs-promise-hook: %d parent promises\n", n)
					}
				}
				JS_FreeContext(ctx)
			}
		}
		JS_FreeRuntime(rt)
		fmt.Println("PASS qjs-freeruntime")
	}

	if failed {
		os.Exit(1)
	}
	fmt.Println("PASS p29-qjs (quickjs-ng on goroutine stack via goc)")
}
