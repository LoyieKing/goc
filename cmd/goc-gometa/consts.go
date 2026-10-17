package main

import (
	"fmt"
	"go/ast"
	"go/constant"
	"go/parser"
	"go/token"
	"math/big"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

// srcFile is one parsed GOROOT/src file.
type srcFile struct {
	rel  string // path under GOROOT/src
	fset *token.FileSet
	file *ast.File
	text []byte
}

func (r *resolver) parse(rel string) *srcFile {
	if f, ok := r.files[rel]; ok {
		return f
	}
	path := filepath.Join(r.goroot, "src", filepath.FromSlash(rel))
	text, err := os.ReadFile(path)
	if err != nil {
		r.fatalf("%v", err)
	}
	fset := token.NewFileSet()
	f, err := parser.ParseFile(fset, path, text, parser.ParseComments)
	if err != nil {
		r.fatalf("%v", err)
	}
	sf := &srcFile{rel: rel, fset: fset, file: f, text: text}
	r.files[rel] = sf
	r.used[rel] = true
	return sf
}

// constBlock is the constants of one const declaration, in order.
type constBlock struct {
	names  []string
	values map[string]int64
	strs   map[string]string
}

// consts evaluates every const declaration of a file. Each block is
// evaluated with iota and implicit repetition, as the compiler does; a
// name from an earlier block (or env, for other packages' constants)
// may be referenced.
func (r *resolver) consts(sf *srcFile, env map[string]int64) []constBlock {
	known := map[string]int64{}
	for k, v := range env {
		known[k] = v
	}
	var out []constBlock
	for _, d := range sf.file.Decls {
		gd, ok := d.(*ast.GenDecl)
		if !ok || gd.Tok != token.CONST {
			continue
		}
		blk := constBlock{values: map[string]int64{}, strs: map[string]string{}}
		var last []ast.Expr
		var lastBits int
		for iota, spec := range gd.Specs {
			vs := spec.(*ast.ValueSpec)
			exprs := vs.Values
			bits := typeBits(vs.Type)
			if len(exprs) == 0 {
				exprs, bits = last, lastBits
			} else {
				last, lastBits = exprs, bits
			}
			for i, n := range vs.Names {
				if i >= len(exprs) {
					continue
				}
				v, err := evalConst(exprs[i], known, int64(iota), bits)
				if err != nil {
					if s, ok := exprs[i].(*ast.BasicLit); ok && s.Kind == token.STRING {
						str, _ := strconv.Unquote(s.Value)
						blk.strs[n.Name] = str
						blk.names = append(blk.names, n.Name)
					}
					continue // not an integer constant (e.g. a string or float)
				}
				if n.Name == "_" {
					continue
				}
				blk.names = append(blk.names, n.Name)
				blk.values[n.Name] = v
				known[n.Name] = v
			}
		}
		out = append(out, blk)
	}
	return out
}

func typeBits(t ast.Expr) int {
	id, ok := t.(*ast.Ident)
	if !ok {
		return 0
	}
	switch id.Name {
	case "uint8", "int8":
		return 8
	case "uint16", "int16":
		return 16
	case "uint32", "int32":
		return 32
	case "uint64", "int64":
		return 64
	}
	return 0
}

// evalConst evaluates an integer constant expression.
func evalConst(e ast.Expr, known map[string]int64, iota int64, bits int) (int64, error) {
	v, err := eval(e, known, iota, bits)
	if err != nil {
		return 0, err
	}
	if v.Kind() != constant.Int {
		return 0, fmt.Errorf("not an integer")
	}
	i, ok := constant.Int64Val(v)
	if !ok {
		u, ok := constant.Uint64Val(v)
		if !ok {
			return 0, fmt.Errorf("overflow")
		}
		return int64(u), nil
	}
	return i, nil
}

func eval(e ast.Expr, known map[string]int64, iota int64, bits int) (constant.Value, error) {
	switch x := e.(type) {
	case *ast.BasicLit:
		if x.Kind != token.INT && x.Kind != token.CHAR {
			return nil, fmt.Errorf("literal %s", x.Value)
		}
		return constant.MakeFromLiteral(x.Value, x.Kind, 0), nil
	case *ast.Ident:
		if x.Name == "iota" {
			return constant.MakeInt64(iota), nil
		}
		if v, ok := known[x.Name]; ok {
			return constant.MakeInt64(v), nil
		}
		return nil, fmt.Errorf("unknown name %s", x.Name)
	case *ast.SelectorExpr: // pkg.Name: the caller puts qualified names in known
		if p, ok := x.X.(*ast.Ident); ok {
			if v, ok := known[p.Name+"."+x.Sel.Name]; ok {
				return constant.MakeInt64(v), nil
			}
		}
		return nil, fmt.Errorf("unknown selector")
	case *ast.ParenExpr:
		return eval(x.X, known, iota, bits)
	case *ast.CallExpr: // a conversion T(x)
		if len(x.Args) != 1 {
			return nil, fmt.Errorf("call")
		}
		b := bits
		if tb := typeBits(x.Fun); tb != 0 {
			b = tb
		}
		return eval(x.Args[0], known, iota, b)
	case *ast.UnaryExpr:
		v, err := eval(x.X, known, iota, bits)
		if err != nil {
			return nil, err
		}
		if x.Op == token.XOR && bits > 0 {
			mask := new(big.Int).Sub(new(big.Int).Lsh(big.NewInt(1), uint(bits)), big.NewInt(1))
			vi := new(big.Int)
			vi.SetString(v.ExactString(), 10)
			return constant.Make(new(big.Int).Xor(vi, mask)), nil
		}
		return constant.UnaryOp(x.Op, v, 0), nil
	case *ast.BinaryExpr:
		a, err := eval(x.X, known, iota, bits)
		if err != nil {
			return nil, err
		}
		b, err := eval(x.Y, known, iota, bits)
		if err != nil {
			return nil, err
		}
		switch x.Op {
		case token.SHL, token.SHR:
			s, _ := constant.Uint64Val(b)
			return constant.Shift(a, x.Op, uint(s)), nil
		case token.QUO:
			return constant.BinaryOp(a, token.QUO_ASSIGN, b), nil // integer division
		}
		return constant.BinaryOp(a, x.Op, b), nil
	}
	return nil, fmt.Errorf("unsupported expression %T", e)
}

// pick returns the constants of every block whose names all start with
// prefix, keeping the names whole.
func pick(blocks []constBlock, prefix string) map[string]int {
	out := map[string]int{}
	for _, b := range blocks {
		for _, n := range b.names {
			if v, ok := b.values[n]; ok && strings.HasPrefix(n, prefix) {
				out[n] = int(v)
			}
		}
	}
	return out
}

// pickTrim is pick with the prefix removed from the names.
func pickTrim(blocks []constBlock, prefix string) map[string]int {
	out := map[string]int{}
	for n, v := range pick(blocks, prefix) {
		out[strings.TrimPrefix(n, prefix)] = v
	}
	return out
}

// blockWith returns the block that defines name.
func blockWith(blocks []constBlock, name string) (constBlock, bool) {
	for _, b := range blocks {
		if _, ok := b.values[name]; ok {
			return b, true
		}
	}
	return constBlock{}, false
}

// compositeVar returns the composite literal a package-level var is
// initialized with.
func compositeVar(sf *srcFile, name string) *ast.CompositeLit {
	for _, d := range sf.file.Decls {
		gd, ok := d.(*ast.GenDecl)
		if !ok || gd.Tok != token.VAR {
			continue
		}
		for _, s := range gd.Specs {
			vs := s.(*ast.ValueSpec)
			for i, n := range vs.Names {
				if n.Name != name || i >= len(vs.Values) {
					continue
				}
				v := vs.Values[i]
				if u, ok := v.(*ast.UnaryExpr); ok && u.Op == token.AND {
					v = u.X
				}
				if cl, ok := v.(*ast.CompositeLit); ok {
					return cl
				}
			}
		}
	}
	return nil
}

func litString(e ast.Expr) (string, bool) {
	b, ok := e.(*ast.BasicLit)
	if !ok || b.Kind != token.STRING {
		return "", false
	}
	s, err := strconv.Unquote(b.Value)
	return s, err == nil
}

func litInt(e ast.Expr) (int, bool) {
	b, ok := e.(*ast.BasicLit)
	if !ok || b.Kind != token.INT {
		return 0, false
	}
	n, err := strconv.ParseInt(b.Value, 0, 64)
	return int(n), err == nil
}
