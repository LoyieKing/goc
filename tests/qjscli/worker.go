package main

import "time"

// Called from the worker event loop while no QuickJS runtime is executing.
// The C scheduler supplies a monotonic deadline; Go owns the actual wait.
//go:noinline
func gocQjsCliWorkerSleep(nanoseconds int64) {
	if nanoseconds > 0 {
		time.Sleep(time.Duration(nanoseconds))
	}
}
