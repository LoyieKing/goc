package gocobj

import (
	"encoding/binary"
	"fmt"
	"io"
	"sort"
	"strings"
)

// Builder encodes the .goc.* sections. The C++ emitter in goc-llc is the
// product writer; this one exists for tests and golden files.
type Builder struct {
	Header    Header
	Files     []string
	Funcs     []Func
	Relocs    []Reloc
	Stackmaps []Stackmap
	Data      []Data
}

// Sections returns the payload of every .goc.* section.
func (bd *Builder) Sections() map[string][]byte {
	strs := []byte{0}
	idx := map[string]uint64{"": 0}
	s := func(v string) uint64 {
		if o, ok := idx[v]; ok {
			return o
		}
		o := uint64(len(strs))
		strs = append(append(strs, v...), 0)
		idx[v] = o
		return o
	}
	out := map[string][]byte{}

	h := make([]byte, HeaderSize)
	copy(h, Magic)
	le := binary.LittleEndian
	le.PutUint16(h[8:], Major)
	le.PutUint16(h[10:], bd.Header.Minor)
	h[12] = bd.Header.Arch
	h[13] = bd.Header.Flags
	le.PutUint32(h[16:], uint32(s(bd.Header.Profile)))
	le.PutUint32(h[20:], uint32(s(bd.Header.Producer)))
	out[SecHeader] = h

	var files, funcs, relocs, stackmaps, pcv, gcm, data enc
	for _, f := range bd.Files {
		var r enc
		r.uleb(s(f))
		files.record(r.b)
	}
	for _, fn := range bd.Funcs {
		var pcvOff, gcOff uint64
		if fn.PCValue != nil {
			var r enc
			r.uleb(uint64(len(fn.PCValue)))
			for _, t := range fn.PCValue {
				r.u8(uint8(t.Kind))
				r.uleb(uint64(len(t.Entries)))
				for _, e := range t.Entries {
					r.uleb(e.PC)
					r.sleb(e.Value)
				}
			}
			pcvOff = pcv.record(r.b) + 1
		}
		if fn.GC != nil {
			var r enc
			r.uleb(fn.GC.NArgs)
			r.uleb(fn.GC.NLocals)
			r.uleb(uint64(len(fn.GC.Safepoints)))
			for _, sp := range fn.GC.Safepoints {
				r.uleb(sp.PC)
				r.raw(sp.Args)
				r.raw(sp.Locals)
				r.uleb(uint64(len(sp.Color)))
				r.raw(sp.Color)
			}
			gcOff = gcm.record(r.b) + 1
		}
		var r enc
		r.uleb(s(fn.Name))
		r.uleb(fn.TextSize)
		r.uleb(fn.FrameSize)
		r.uleb(fn.ArgSize)
		r.u8(fn.ABI)
		r.u8(fn.Kind)
		r.uleb(fn.Flags)
		r.u8(fn.FuncID)
		r.uleb(fn.File)
		r.uleb(fn.StartLine)
		r.uleb(pcvOff)
		r.uleb(gcOff)
		r.uleb(0)
		if bd.Header.Minor >= 1 {
			r.uleb(fn.SysvPtrRegs)
		}
		if bd.Header.Minor >= 2 {
			r.uleb(uint64(len(fn.SptrSlots)))
			for _, s := range fn.SptrSlots {
				r.uleb(s)
			}
		}
		funcs.record(r.b)
	}
	for _, rl := range bd.Relocs {
		var r enc
		r.uleb(s(rl.Func))
		r.uleb(rl.Offset)
		r.u8(rl.Width)
		r.u8(uint8(rl.Kind))
		r.uleb(s(rl.Target))
		r.sleb(rl.Addend)
		r.u8(rl.Flags)
		relocs.record(r.b)
	}
	for _, sm := range bd.Stackmaps {
		var r enc
		r.uleb(s(sm.Func))
		r.uleb(sm.InsnOff)
		r.uleb(sm.ID)
		r.uleb(uint64(len(sm.Locs)))
		for _, l := range sm.Locs {
			r.u8(l.Type)
			r.uleb(l.Size)
			r.uleb(l.Reg)
			r.sleb(l.Off)
		}
		stackmaps.record(r.b)
	}
	for _, d := range bd.Data {
		var r enc
		r.uleb(s(d.Name))
		r.uleb(d.Size)
		r.u8(d.Log2Align)
		r.uleb(d.Flags)
		r.uleb(d.NBits)
		r.raw(d.Bitmap)
		data.record(r.b)
	}
	for name, e := range map[string]*enc{SecFiles: &files, SecFuncs: &funcs, SecRelocs: &relocs, SecStackmaps: &stackmaps,
		SecPCValue: &pcv, SecGCMaps: &gcm, SecData: &data} {
		if len(e.b) > 0 {
			out[name] = e.b
		}
	}
	out[SecStrtab] = strs
	return out
}

// WriteAsm writes the sections as GNU assembler directives, for appending
// to a hand-written .s file that llvm-mc turns into a golden .gocobj.
func (bd *Builder) WriteAsm(w io.Writer) error {
	secs := bd.Sections()
	names := make([]string, 0, len(secs))
	for n := range secs {
		names = append(names, n)
	}
	sort.Strings(names)
	for _, n := range names {
		if _, err := fmt.Fprintf(w, "\t.section %s,\"\",@progbits\n", n); err != nil {
			return err
		}
		b := secs[n]
		for len(b) > 0 {
			k := min(16, len(b))
			parts := make([]string, k)
			for i := range parts {
				parts[i] = fmt.Sprintf("0x%02x", b[i])
			}
			fmt.Fprintf(w, "\t.byte %s\n", strings.Join(parts, ","))
			b = b[k:]
		}
	}
	return nil
}
