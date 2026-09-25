// qjscli is a Go entry point for the goc-built QuickJS engine. The engine and
// host callbacks are linked from goc goobj files; this program owns file I/O,
// argument parsing, evaluation, error reporting, and the pending-job loop.
package main

/*
#include <stdlib.h>
*/
import "C"

import (
	"errors"
	"fmt"
	"io"
	"math"
	"os"
	"runtime"
	"strconv"
	"strings"
	"time"
	"unsafe"
)

//go:linkname JS_NewRuntime main.JS_NewRuntime
//go:linkname JS_SetMaxStackSize main.JS_SetMaxStackSize
//go:linkname JS_FreeRuntime main.JS_FreeRuntime
//go:linkname JS_NewContext main.JS_NewContext
//go:linkname JS_FreeContext main.JS_FreeContext
//go:linkname JS_Eval main.JS_Eval
//go:linkname JS_EvalFunction main.JS_EvalFunction
//go:linkname JS_GetException main.JS_GetException
//go:linkname JS_ToCStringLen2 main.JS_ToCStringLen2
//go:linkname JS_FreeCString main.JS_FreeCString
//go:linkname JS_FreeValue main.JS_FreeValue
//go:linkname JS_IsJobPending main.JS_IsJobPending
//go:linkname JS_ExecutePendingJob main.JS_ExecutePendingJob
//go:linkname JS_PromiseState main.JS_PromiseState
//go:linkname JS_PromiseResult main.JS_PromiseResult
//go:linkname goc_qjs_cli_install main.goc_qjs_cli_install
//go:linkname goc_qjs_cli_unhandled_rejections main.goc_qjs_cli_unhandled_rejections
//go:linkname goc_qjs_cli_rejection_reason main.goc_qjs_cli_rejection_reason
//go:linkname goc_qjs_cli_clear_rejections main.goc_qjs_cli_clear_rejections
//go:linkname goc_qjs_cli_enable_interrupt main.goc_qjs_cli_enable_interrupt
//go:linkname goc_qjs_cli_dispatch_workers main.goc_qjs_cli_dispatch_workers
//go:linkname goc_qjs_cli_worker_cleanup main.goc_qjs_cli_worker_cleanup
func JS_NewRuntime() unsafe.Pointer
func JS_SetMaxStackSize(rt unsafe.Pointer, size uint64)
func JS_FreeRuntime(rt unsafe.Pointer)
func JS_NewContext(rt unsafe.Pointer) unsafe.Pointer
func JS_FreeContext(ctx unsafe.Pointer)
func JS_Eval(ctx unsafe.Pointer, input *byte, length uint64, filename *byte, flags int32) jsValue
func JS_EvalFunction(ctx unsafe.Pointer, code jsValue) jsValue
func JS_GetException(ctx unsafe.Pointer) jsValue
func JS_ToCStringLen2(ctx unsafe.Pointer, length *uint64, val jsValue, cesu8 bool) *byte
func JS_FreeCString(ctx unsafe.Pointer, p *byte)
func JS_FreeValue(ctx unsafe.Pointer, val jsValue)
func JS_IsJobPending(rt unsafe.Pointer) bool
func JS_ExecutePendingJob(rt unsafe.Pointer, ctx **byte) int32
func JS_PromiseState(ctx unsafe.Pointer, promise jsValue) int32
func JS_PromiseResult(ctx unsafe.Pointer, promise jsValue) jsValue
func goc_qjs_cli_install(ctx unsafe.Pointer) int32
func goc_qjs_cli_unhandled_rejections(ctx unsafe.Pointer) int32
func goc_qjs_cli_rejection_reason(ctx unsafe.Pointer) jsValue
func goc_qjs_cli_clear_rejections(ctx unsafe.Pointer)
func goc_qjs_cli_enable_interrupt(rt unsafe.Pointer, countdown int32)
func goc_qjs_cli_dispatch_workers(ctx unsafe.Pointer) int32
func goc_qjs_cli_worker_cleanup(ctx unsafe.Pointer)

type jsValue struct {
	payload uint64
	tag     int64
}

// Called from the SysV C math shim after it restores g in R14.
//
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

const (
	jsTagException = 6
	jsEvalGlobal   = 0
	jsEvalModule   = 1
	jsCompileOnly  = 1 << 5
)

func jsString(ctx unsafe.Pointer, val jsValue) (string, error) {
	var length uint64
	p := JS_ToCStringLen2(ctx, &length, val, false)
	if p == nil {
		return "", errors.New("QuickJS could not convert value to string")
	}
	defer JS_FreeCString(ctx, p)
	if length > uint64(^uint(0)>>1) {
		return "", errors.New("QuickJS string exceeds Go address space")
	}
	return string(unsafe.Slice(p, int(length))), nil
}

func jsException(ctx unsafe.Pointer) error {
	val := JS_GetException(ctx)
	defer JS_FreeValue(ctx, val)
	text, err := jsString(ctx, val)
	if err != nil {
		return fmt.Errorf("QuickJS exception (%v)", err)
	}
	return errors.New(text)
}

func evaluate(rt, ctx unsafe.Pointer, source []byte, filename string,
	module, showResult, allowUnhandled bool) error {
	// QuickJS requires input[input_len] == '\\0'. Keep both Go buffers live
	// through the synchronous C call; the JS source owns no Go-heap pointer.
	length := len(source)
	source = append(source, 0)
	name := append([]byte(filename), 0)
	defer runtime.KeepAlive(source)
	defer runtime.KeepAlive(name)
	flags := int32(jsEvalGlobal)
	if module {
		flags = jsEvalModule
	}
	compiled := JS_Eval(ctx, &source[0], uint64(length), &name[0], flags|jsCompileOnly)
	if compiled.tag == jsTagException {
		return fmt.Errorf("qjscli:parse: %s: %w", filename, jsException(ctx))
	}
	result := JS_EvalFunction(ctx, compiled) // consumes compiled
	if result.tag == jsTagException {
		return fmt.Errorf("qjscli:runtime: %s: %w", filename, jsException(ctx))
	}
	defer JS_FreeValue(ctx, result)
	if showResult {
		text, err := jsString(ctx, result)
		if err != nil {
			return fmt.Errorf("%s: %w", filename, err)
		}
		fmt.Println(text)
	}
	for {
		for JS_IsJobPending(rt) {
			var jobCtx *byte
			if JS_ExecutePendingJob(rt, &jobCtx) < 0 {
				failedCtx := ctx
				if jobCtx != nil {
					failedCtx = unsafe.Pointer(jobCtx)
				}
				return fmt.Errorf("qjscli:runtime: %s: pending job: %w", filename, jsException(failedCtx))
			}
		}
		work := goc_qjs_cli_dispatch_workers(ctx)
		if work < 0 {
			return fmt.Errorf("qjscli:runtime: %s: worker callback: %w", filename, jsException(ctx))
		}
		if work == 0 {
			break
		}
	}
	if n := goc_qjs_cli_unhandled_rejections(ctx); n != 0 && !allowUnhandled {
		reason := goc_qjs_cli_rejection_reason(ctx)
		defer JS_FreeValue(ctx, reason)
		if text, err := jsString(ctx, reason); err == nil {
			return fmt.Errorf("qjscli:runtime: %s: %d unhandled Promise rejection(s): %s", filename, n, text)
		}
		return fmt.Errorf("qjscli:runtime: %s: %d unhandled Promise rejection(s)", filename, n)
	}
	if module {
		switch JS_PromiseState(ctx, result) {
		case 0:
			return fmt.Errorf("qjscli:runtime: %s: module evaluation remains pending without jobs", filename)
		case 2:
			reason := JS_PromiseResult(ctx, result)
			defer JS_FreeValue(ctx, reason)
			text, err := jsString(ctx, reason)
			if err != nil {
				return fmt.Errorf("qjscli:runtime: %s: rejected module (%v)", filename, err)
			}
			return fmt.Errorf("qjscli:runtime: %s: rejected module: %s", filename, text)
		}
	}
	return nil
}

//go:noinline
func forceGrow(n int) {
	var pad [32 << 10]byte
	pad[0] = byte(n)
	pad[len(pad)-1] = byte(n + 1)
	if n > 1 && pad[0] != 0 {
		forceGrow(n - 1)
	}
	if pad[0]+pad[len(pad)-1] == 255 {
		os.Exit(2)
	}
}

func run(args []string) error {
	var filename, expression string
	var module bool
	var interruptAfter int
	var allowUnhandled bool
	var stackSizeKB int
	for len(args) > 0 {
		if args[0] == "--allow-unhandled-rejections" {
			allowUnhandled = true
			args = args[1:]
			continue
		}
		if args[0] == "--interrupt-after" && len(args) >= 2 {
			n, err := strconv.Atoi(args[1])
			if err != nil || n <= 0 || n > 0x7fffffff {
				return errors.New("--interrupt-after requires a positive 32-bit count")
			}
			interruptAfter = n
			args = args[2:]
			continue
		}
		if args[0] == "--stack-size" && len(args) >= 2 {
			n, err := strconv.Atoi(args[1])
			if err != nil || n <= 0 || n > int(^uint(0)>>1)/1024 {
				return errors.New("--stack-size requires a positive KiB count")
			}
			stackSizeKB = n
			args = args[2:]
			continue
		}
		break
	}
	switch {
	case len(args) == 2 && args[0] == "-e":
		expression = args[1]
		filename = "<eval>"
	case len(args) == 2 && args[0] == "-m":
		module = true
		filename = args[1]
	case len(args) == 1 && args[0] != "-e" && args[0] != "-m":
		filename = args[0]
	default:
		return errors.New("usage: qjscli [--stack-size KiB] [--interrupt-after count] [--allow-unhandled-rejections] [-e expression | [-m] script.js | -]")
	}

	var source []byte
	if filename == "<eval>" {
		source = []byte(expression)
	} else if filename == "-" {
		filename = "<stdin>"
		var err error
		source, err = io.ReadAll(os.Stdin)
		if err != nil {
			return err
		}
	} else {
		var err error
		source, err = os.ReadFile(filename)
		if err != nil {
			return err
		}
	}
	if strings.HasSuffix(filename, ".mjs") {
		module = true
	}
	rt := JS_NewRuntime()
	if rt == nil {
		return errors.New("JS_NewRuntime failed")
	}
	forceGrow(40)
	defer JS_FreeRuntime(rt)
	if stackSizeKB > 0 {
		JS_SetMaxStackSize(rt, uint64(stackSizeKB)*1024)
	}
	ctx := JS_NewContext(rt)
	if ctx == nil {
		return errors.New("JS_NewContext failed")
	}
	defer JS_FreeContext(ctx)
	if goc_qjs_cli_install(ctx) < 0 {
		return fmt.Errorf("QuickJS host initialization: %w", jsException(ctx))
	}
	defer goc_qjs_cli_clear_rejections(ctx)
	defer goc_qjs_cli_worker_cleanup(ctx)
	if interruptAfter > 0 {
		goc_qjs_cli_enable_interrupt(rt, int32(interruptAfter))
	}
	return evaluate(rt, ctx, source, filename, module, filename == "<eval>", allowUnhandled)
}

func main() {
	if err := run(os.Args[1:]); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
