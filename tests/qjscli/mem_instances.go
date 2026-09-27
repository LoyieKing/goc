//go:build qjsmem

// Multi-instance memory probe, built only with -tags qjsmem
// (scripts/bench-mem.sh runs QJSCLI_TAGS=qjsmem QJSCLI_OUT=build/qjs/qjsmem
// scripts/qjs-cli-build.sh). The default qjscli binary does not contain it.
//
//	qjsmem --mem-instances N
//
// Starts N goroutines. Each one owns a QuickJS runtime and context, evaluates
// tests/qjsmem/instance.js, then stays blocked with the runtime alive. The
// runtimes are created one after another: the goc libc shim heap has no
// locks, so two goroutines must never be inside QuickJS at the same time.
// Once all N are parked it prints one JSON line with process RSS
// (/proc/self/status) and runtime.MemStats, then the same again after
// runtime.GC() + debug.FreeOSMemory().
package main

import (
	_ "embed"
	"encoding/json"
	"fmt"
	"os"
	"runtime"
	"runtime/debug"
	"strconv"
	"strings"
)

//go:embed instance.js
var memInstanceScript string

// qjsFlavor is set by scripts/qjs-cli-build.sh for QJS_FLAVOR=bellard
// (-X main.qjsFlavor=bellard); the ng build keeps the default.
var qjsFlavor = "ng"

func init() {
	if len(os.Args) == 3 && os.Args[1] == "--mem-instances" {
		n, err := strconv.Atoi(os.Args[2])
		if err != nil || n < 0 {
			fmt.Fprintln(os.Stderr, "--mem-instances needs a count >= 0")
			os.Exit(2)
		}
		os.Exit(memInstances(n))
	}
}

type memSnap struct {
	VmRSSKB, VmHWMKB                         int64
	HeapSys, HeapInuse, StackSys, StackInuse uint64
	Sys, NumGC                               uint64
	Goroutines                               int
}

func procStatusKB(field string) int64 {
	b, _ := os.ReadFile("/proc/self/status")
	for _, l := range strings.Split(string(b), "\n") {
		if strings.HasPrefix(l, field+":") {
			f := strings.Fields(l)
			v, _ := strconv.ParseInt(f[1], 10, 64)
			return v
		}
	}
	return -1
}

func snap() memSnap {
	var ms runtime.MemStats
	runtime.ReadMemStats(&ms)
	return memSnap{procStatusKB("VmRSS"), procStatusKB("VmHWM"), ms.HeapSys, ms.HeapInuse,
		ms.StackSys, ms.StackInuse, ms.Sys, uint64(ms.NumGC), runtime.NumGoroutine()}
}

func memInstances(n int) int {
	base := snap()
	ready := make(chan string)
	release := make(chan struct{})
	for i := 0; i < n; i++ {
		go func() {
			rt := JS_NewRuntime()
			if rt == nil {
				ready <- "JS_NewRuntime failed"
				return
			}
			ctx := JS_NewContext(rt)
			if ctx == nil {
				ready <- "JS_NewContext failed"
				return
			}
			src := append([]byte(memInstanceScript), 0)
			name := []byte("instance.js\x00")
			v := JS_Eval(ctx, &src[0], uint64(len(src)-1), &name[0], jsEvalGlobal)
			if v.tag == jsTagException {
				ready <- "eval: " + jsException(ctx).Error()
				return
			}
			text, _ := jsString(ctx, v)
			JS_FreeValue(ctx, v)
			runtime.KeepAlive(src)
			ready <- "ok " + text
			<-release // runtime stays alive; the process exits without freeing it
		}()
		if msg := <-ready; !strings.HasPrefix(msg, "ok ") {
			fmt.Fprintln(os.Stderr, "instance", i, msg)
			return 1
		} else if i == 0 {
			fmt.Fprintln(os.Stderr, "instance 0 result:", msg[3:])
		}
	}
	live := snap()
	runtime.GC()
	debug.FreeOSMemory()
	gc := snap()
	engine := "goc"
	if qjsFlavor == "bellard" {
		engine = "goc-bellard"
	}
	out, _ := json.Marshal(map[string]any{"engine": engine, "n": n, "gogc": os.Getenv("GOGC"),
		"base": base, "live": live, "after_gc": gc})
	fmt.Println("MEMINST " + string(out))
	close(release)
	return 0
}
