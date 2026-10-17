// The multi-instance memory probe for Goja (docs/benchmark, 内存占用).
// Usage: goja -mem N script.js
// Starts N goroutines; each owns one goja.Runtime, runs the script, then
// blocks with the runtime alive. Runtimes are created one after another, the
// same order as the goc and C harnesses. Prints one JSON line with process
// RSS (/proc/self/status) and runtime.MemStats, then again after
// runtime.GC() + debug.FreeOSMemory().
package main

import (
	"encoding/json"
	"fmt"
	"os"
	"runtime"
	"runtime/debug"
	"strconv"
	"strings"

	"github.com/dop251/goja"
)

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
			v, _ := strconv.ParseInt(strings.Fields(l)[1], 10, 64)
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

func memMain(args []string) {
	if len(args) != 2 {
		fmt.Fprintln(os.Stderr, "usage: goja -mem N script.js")
		os.Exit(2)
	}
	n, err := strconv.Atoi(args[0])
	if err != nil || n < 0 {
		fmt.Fprintln(os.Stderr, "N must be >= 0")
		os.Exit(2)
	}
	src, err := os.ReadFile(args[1])
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}
	base := snap()
	ready := make(chan string)
	release := make(chan struct{})
	for i := 0; i < n; i++ {
		go func() {
			vm := goja.New()
			v, err := vm.RunScript("instance.js", string(src))
			if err != nil {
				ready <- err.Error()
				return
			}
			ready <- "ok " + v.String()
			<-release
			runtime.KeepAlive(vm)
		}()
		if msg := <-ready; !strings.HasPrefix(msg, "ok ") {
			fmt.Fprintln(os.Stderr, "instance", i, msg)
			os.Exit(1)
		} else if i == 0 {
			fmt.Fprintln(os.Stderr, "instance 0 result:", msg[3:])
		}
	}
	live := snap()
	runtime.GC()
	debug.FreeOSMemory()
	gc := snap()
	out, _ := json.Marshal(map[string]any{"engine": "goja", "n": n, "gogc": os.Getenv("GOGC"),
		"base": base, "live": live, "after_gc": gc})
	fmt.Println("MEMINST " + string(out))
	close(release)
}
