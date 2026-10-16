module github.com/LoyieKing/goc

go 1.23

require github.com/LoyieKing/goc/goobj/enc v0.0.0

// The Go object writer, copied from a GOROOT by tools/goc-goenc. The tree
// copy is go1.24.4's; tools/goc/goenc.sh builds gocobj2goobj against a copy
// of the Go release in use instead (docs/multi-go-version.md).
replace github.com/LoyieKing/goc/goobj/enc => ./goobj/enc
