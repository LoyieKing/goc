package abi

// Stubs for compiler intrinsics (normally provided by the Go toolchain).
// Enough for cmd/internal/obj encode path when vendored outside GOROOT.

func FuncPCABI0(f interface{}) uintptr { return 0 }

func FuncPCABIInternal(f interface{}) uintptr { return 0 }
