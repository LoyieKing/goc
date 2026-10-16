package main

import (
	"encoding/binary"

	"github.com/LoyieKing/goc/gocobj"
	"github.com/LoyieKing/goc/goobj/enc/cmd/objlib/obj"
	"github.com/LoyieKing/goc/goobj/enc/cmd/objlib/objabi"
)

// pctab encodes plain (pc, value) entries as a Go pc-value table. Values
// hold from their pc to the next entry; the runtime starts at -1.
func pctab(ctxt *obj.Link, size int64, ents []gocobj.PCEntry) *obj.LSym {
	sym := &obj.LSym{Type: objabi.SRODATA, Attribute: obj.AttrContentAddressable | obj.AttrPcdata}
	// Align 1, as cmd/internal/obj.funcpctab does; go1.27 rejects a
	// content-addressable symbol without one.
	sym.GocSetAlign(1)
	if size <= 0 {
		return sym
	}
	type seg struct {
		val   int64
		start int64
	}
	segs := []seg{{-1, 0}}
	for _, e := range ents {
		pc := int64(e.PC)
		if pc < 0 || pc >= size {
			continue
		}
		last := &segs[len(segs)-1]
		if pc == last.start {
			last.val = e.Value
			continue
		}
		if e.Value == last.val {
			continue
		}
		segs = append(segs, seg{e.Value, pc})
	}
	minLC := int64(ctxt.Arch.MinLC)
	var dst []byte
	prev := int64(-1)
	for i, s := range segs {
		end := size
		if i+1 < len(segs) {
			end = segs[i+1].start
		}
		if end <= s.start {
			continue
		}
		dst = binary.AppendVarint(dst, s.val-prev)
		dst = binary.AppendUvarint(dst, uint64((end-s.start)/minLC))
		prev = s.val
	}
	dst = append(dst, 0)
	sym.P = dst
	sym.Size = int64(len(dst))
	return sym
}

// inlSym names an inlined C function in the inline tree: the Go symbol of
// its body in this object if it has one, else (a static function inlined
// everywhere) the name a body of it would have, which gets a stub.
func (c *converter) inlSym(name string) *obj.LSym {
	if gn, ok := c.goName[name]; ok {
		return c.ctxt.LookupABI(gn, c.abiByGoSym[gn])
	}
	return c.ctxt.Lookup(c.pkg + "." + c.tu + "." + name)
}

func pcTable(t *gocobj.Func, k gocobj.PCKind) []gocobj.PCEntry {
	for _, p := range t.PCValue {
		if p.Kind == k {
			return p.Entries
		}
	}
	return nil
}

// stackmaps turns a .goc.gcmaps record into the args and locals stack map
// blobs (n uint32, nbit uint32, n bitmaps) and the StackMapIndex entries:
// each safepoint's index holds from the byte before its return address.
func stackmaps(g *gocobj.GCMap) (args, locals []byte, idx []gocobj.PCEntry) {
	type pair struct{ a, l string }
	seen := map[pair]int{}
	var as, ls [][]byte
	for _, sp := range g.Safepoints {
		k := pair{string(sp.Args), string(sp.Locals)}
		i, ok := seen[k]
		if !ok {
			i = len(as)
			seen[k] = i
			as = append(as, sp.Args)
			ls = append(ls, sp.Locals)
		}
		idx = append(idx, gocobj.PCEntry{PC: sp.PC - 1, Value: int64(i)})
	}
	blob := func(nbit uint64, maps [][]byte) []byte {
		stride := int((nbit + 7) / 8)
		b := make([]byte, 8, 8+len(maps)*stride)
		n := len(maps)
		if n == 0 {
			n = 1 // the runtime wants n > 0 even when nothing is scanned
		}
		binary.LittleEndian.PutUint32(b[0:], uint32(n))
		binary.LittleEndian.PutUint32(b[4:], uint32(nbit))
		for _, m := range maps {
			b = append(b, m...)
		}
		if len(maps) == 0 {
			b = append(b, make([]byte, stride)...)
		}
		return b
	}
	return blob(g.NArgs, as), blob(g.NLocals, ls), idx
}
