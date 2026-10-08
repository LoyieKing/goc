package main

import (
	"syscall"
	"time"
	"unsafe"
)

// Called from the worker event loop while no QuickJS runtime is executing.
// The C scheduler supplies a monotonic deadline; Go owns the actual wait.
//
//go:noinline
func gocQjsCliWorkerSleep(nanoseconds int64) {
	if nanoseconds > 0 {
		time.Sleep(time.Duration(nanoseconds))
	}
}

// Layout matches GocQjsCliPollRequest in _qjs_cli_worker.c. fds points at
// count pollfd values (int32 fd, int16 events, int16 revents). timeoutMs -1
// waits indefinitely. result is the number of ready fds, or -errno.
type gocQjsCliPollRequest struct {
	fds       unsafe.Pointer
	count     int32
	timeoutMs int32
	result    int32
	_         int32
}

// Called from the os.setReadHandler / os.setWriteHandler loop. The goroutine
// parks in Go instead of blocking inside a goc-compiled syscall.
//
//go:noinline
func gocQjsCliPoll(request *gocQjsCliPollRequest) {
	if request == nil || request.count < 0 {
		return
	}
	if request.count == 0 {
		if request.timeoutMs > 0 {
			time.Sleep(time.Duration(request.timeoutMs) * time.Millisecond)
		}
		request.result = 0
		return
	}
	// Go 1.24 dropped syscall.Poll. SYS_POLL is poll(2); a negative timeout
	// is sign-extended so -1 still means "wait forever".
	n, _, errno := syscall.Syscall(syscall.SYS_POLL,
		uintptr(request.fds),
		uintptr(request.count),
		uintptr(request.timeoutMs))
	if errno != 0 {
		if errno == syscall.EINTR {
			request.result = 0
			return
		}
		request.result = -int32(errno)
		return
	}
	request.result = int32(n)
}
