// goc addition, not part of the Go tree (tools/goc-goenc).

package abi

// FuncPCABI0 and FuncPCABIInternal are compiler intrinsics inside GOROOT.
// The object writer never calls them; these stubs only let the copy build.

func FuncPCABI0(f interface{}) uintptr { return 0 }

func FuncPCABIInternal(f interface{}) uintptr { return 0 }
