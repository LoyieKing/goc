package main

import (
	"errors"
	"fmt"
	"os"
	"os/exec"
	"runtime"
	"strings"
	"sync"
	"syscall"
	"unsafe"
)

// Layout is shared with GocQjsCliProcessRequest in _qjs_cli_std_os.c.
type gocQjsCliProcessRequest struct {
	op, pid, block, options, usePath, stdinFD, stdoutFD, stderrFD int32
	argc                                                        uint32
	_pad0                                                       uint32
	argv                                                        uintptr
	envc                                                        uint32
	_pad1                                                       uint32
	envp                                                        uintptr
	file, cwd                                                   uintptr
	uid, gid, groupsLen                                         uint32
	uidSet, gidSet                                              int32
	groupsSet                                                   int32
	groups                                                      uintptr
	result, status                                              int32
}

type gocQjsCliFloatRequest struct {
	format   uintptr
	value    float64
	output   uintptr
	capacity uint64
	length   int64
}

type gocQjsCliStrerrorRequest struct {
	code     int32
	_pad     uint32
	output   uintptr
	capacity uint64
	length   int64
}

type gocQjsCliChild struct {
	cmd   *exec.Cmd
	done  chan struct{}
	files []*os.File
	// Written by the sole Wait goroutine before closing done.
	status int32
	exit   int32
}

var gocQjsCliChildren = struct {
	sync.Mutex
	byPID map[int32]*gocQjsCliChild
}{byPID: make(map[int32]*gocQjsCliChild)}

func gocQjsCliCString(address uintptr) string {
	if address == 0 {
		return ""
	}
	p := (*byte)(unsafe.Pointer(address))
	length := 0
	for *(*byte)(unsafe.Add(unsafe.Pointer(p), length)) != 0 {
		length++
	}
	return strings.Clone(unsafe.String(p, length))
}

func gocQjsCliCStringArray(address uintptr, count uint32) []string {
	if count == 0 {
		return []string{}
	}
	pointers := unsafe.Slice((**byte)(unsafe.Pointer(address)), int(count))
	result := make([]string, len(pointers))
	for i, pointer := range pointers {
		result[i] = gocQjsCliCString(uintptr(unsafe.Pointer(pointer)))
	}
	return result
}

func gocQjsCliOSFile(fd int32, name string) (*os.File, error) {
	if fd < 0 {
		return nil, syscall.EBADF
	}
	file := os.NewFile(uintptr(fd), name)
	if file == nil {
		return nil, syscall.EBADF
	}
	// C owns every descriptor. The wrapper exists only for exec.Cmd's
	// pass-through child stdio handling and must never close that descriptor.
	runtime.SetFinalizer(file, nil)
	return file, nil
}

func gocQjsCliExitResult(status syscall.WaitStatus) int32 {
	if status.Exited() {
		return int32(status.ExitStatus())
	}
	if status.Signaled() {
		return -int32(status.Signal())
	}
	return 127
}

func gocQjsCliErrno(err error) syscall.Errno {
	if errors.Is(err, exec.ErrNotFound) {
		return syscall.ENOENT
	}
	var errno syscall.Errno
	if errors.As(err, &errno) {
		return errno
	}
	return syscall.EIO
}

func gocQjsCliWaitChild(child *gocQjsCliChild) {
	err := child.cmd.Wait()
	if child.cmd.ProcessState != nil {
		if status, ok := child.cmd.ProcessState.Sys().(syscall.WaitStatus); ok {
			child.status = int32(status)
			child.exit = gocQjsCliExitResult(status)
		} else {
			child.exit = 127
		}
	} else if err != nil {
		child.exit = 127
	}
	close(child.done)
}

func gocQjsCliCredential(request *gocQjsCliProcessRequest) *syscall.Credential {
	if request.uidSet == 0 && request.gidSet == 0 && request.groupsSet == 0 {
		return nil
	}
	uid, gid := uint32(os.Getuid()), uint32(os.Getgid())
	if request.uidSet != 0 {
		uid = request.uid
	}
	if request.gidSet != 0 {
		gid = request.gid
	}
	groups := make([]uint32, int(request.groupsLen))
	if request.groupsLen != 0 {
		provided := unsafe.Slice((*uint32)(unsafe.Pointer(request.groups)), int(request.groupsLen))
		copy(groups, provided)
	}
	return &syscall.Credential{
		Uid: uid, Gid: gid, Groups: groups, NoSetGroups: request.groupsSet == 0,
	}
}

func gocQjsCliStart(request *gocQjsCliProcessRequest) {
	arguments := gocQjsCliCStringArray(request.argv, request.argc)
	if len(arguments) == 0 {
		request.result = -int32(syscall.EINVAL)
		return
	}
	path := gocQjsCliCString(request.file)
	if path == "" {
		path = arguments[0]
	}
	if request.usePath != 0 {
		resolved, err := exec.LookPath(path)
		if err != nil {
			if request.block != 0 {
				request.result = 127
			} else {
				request.result = -int32(gocQjsCliErrno(err))
			}
			return
		}
		path = resolved
	}

	cmd := &exec.Cmd{
		Path: path,
		Args: arguments,
		Dir:  gocQjsCliCString(request.cwd),
	}
	if request.envp != 0 {
		environment := gocQjsCliCStringArray(request.envp, request.envc)
		cmd.Env = environment
	}
	child := &gocQjsCliChild{cmd: cmd, done: make(chan struct{})}
	var err error
	stdin, err := gocQjsCliOSFile(request.stdinFD, "qjs-stdin")
	if err != nil {
		request.result = -int32(syscall.EBADF)
		return
	}
	cmd.Stdin = stdin
	child.files = append(child.files, stdin)
	stdout, err := gocQjsCliOSFile(request.stdoutFD, "qjs-stdout")
	if err != nil {
		request.result = -int32(syscall.EBADF)
		return
	}
	cmd.Stdout = stdout
	child.files = append(child.files, stdout)
	stderr, err := gocQjsCliOSFile(request.stderrFD, "qjs-stderr")
	if err != nil {
		request.result = -int32(syscall.EBADF)
		return
	}
	cmd.Stderr = stderr
	child.files = append(child.files, stderr)
	if credential := gocQjsCliCredential(request); credential != nil {
		cmd.SysProcAttr = &syscall.SysProcAttr{Credential: credential}
	}
	if err := cmd.Start(); err != nil {
		if request.block != 0 {
			request.result = 127
		} else {
			request.result = -int32(gocQjsCliErrno(err))
		}
		return
	}
	pid := int32(cmd.Process.Pid)
	gocQjsCliChildren.Lock()
	gocQjsCliChildren.byPID[pid] = child
	gocQjsCliChildren.Unlock()
	go gocQjsCliWaitChild(child)
	request.result = pid
	if request.block != 0 {
		<-child.done
		request.result = child.exit
		request.status = child.status
		gocQjsCliChildren.Lock()
		delete(gocQjsCliChildren.byPID, pid)
		gocQjsCliChildren.Unlock()
	}
}

func gocQjsCliWait(request *gocQjsCliProcessRequest) {
	pid := request.pid
	gocQjsCliChildren.Lock()
	child := gocQjsCliChildren.byPID[pid]
	gocQjsCliChildren.Unlock()
	if child == nil {
		request.result = -int32(syscall.ECHILD)
		request.status = 0
		return
	}
	if request.block != 0 {
		<-child.done
	} else {
		select {
		case <-child.done:
		default:
			request.result = 0
			request.status = 0
			return
		}
	}
	gocQjsCliChildren.Lock()
	if gocQjsCliChildren.byPID[pid] != child {
		gocQjsCliChildren.Unlock()
		request.result = -int32(syscall.ECHILD)
		request.status = 0
		return
	}
	delete(gocQjsCliChildren.byPID, pid)
	gocQjsCliChildren.Unlock()
	request.result = pid
	request.status = child.status
}

// Called from C by the explicit goabi entry; no libc/cgo calls are made from
// the QuickJS C stack. The goroutine starts os/exec only after this bridge has
// restored the Go runtime's g register.
//go:noinline
func gocGoQjsCliProcess(request *gocQjsCliProcessRequest) {
	if request == nil {
		return
	}
	switch request.op {
	case 1:
		gocQjsCliStart(request)
	case 2:
		gocQjsCliWait(request)
	default:
		request.result = -int32(syscall.EINVAL)
	}
}

//go:noinline
func gocGoQjsCliFloat(request *gocQjsCliFloatRequest) {
	if request == nil {
		return
	}
	format := gocQjsCliCString(request.format)
	text := fmt.Sprintf(format, request.value)
	request.length = -1
	if request.capacity > uint64(len(text)) {
		output := unsafe.Slice((*byte)(unsafe.Pointer(request.output)), int(request.capacity))
		copy(output, text)
		request.length = int64(len(text))
	}
}

//go:noinline
func gocGoQjsCliStrerror(request *gocQjsCliStrerrorRequest) {
	if request == nil {
		return
	}
	text := syscall.Errno(request.code).Error()
	request.length = -1
	if request.capacity > uint64(len(text)) {
		output := unsafe.Slice((*byte)(unsafe.Pointer(request.output)), int(request.capacity))
		copy(output, text)
		request.length = int64(len(text))
	}
}
