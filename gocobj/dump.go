package gocobj

import (
	"fmt"
	"io"
)

// Dump prints every .goc.* table as text.
func Dump(w io.Writer, f *File) {
	h := f.Header
	fmt.Fprintf(w, "gocobj %d.%d arch=%d flags=%#x profile=%q producer=%q\n",
		h.Major, h.Minor, h.Arch, h.Flags, h.Profile, h.Producer)
	for i, p := range f.Files {
		fmt.Fprintf(w, "file %d %s\n", i, p)
	}
	for _, fn := range f.Funcs {
		fmt.Fprintf(w, "func %s size=%d frame=%d args=%d abi=%d kind=%d flags=%#x id=%d file=%d line=%d sysvptr=%#x\n",
			fn.Name, fn.TextSize, fn.FrameSize, fn.ArgSize, fn.ABI, fn.Kind, fn.Flags, fn.FuncID, fn.File, fn.StartLine, fn.SysvPtrRegs)
		for _, t := range fn.PCValue {
			fmt.Fprintf(w, "  pcvalue %s:", t.Kind)
			for _, e := range t.Entries {
				fmt.Fprintf(w, " %d=%d", e.PC, e.Value)
			}
			fmt.Fprintln(w)
		}
		if g := fn.GC; g != nil {
			fmt.Fprintf(w, "  gcmaps args=%d locals=%d\n", g.NArgs, g.NLocals)
			for _, sp := range g.Safepoints {
				fmt.Fprintf(w, "    pc=%d args=%x locals=%x", sp.PC, sp.Args, sp.Locals)
				if len(sp.Color) > 0 {
					fmt.Fprintf(w, " color=%x", sp.Color)
				}
				fmt.Fprintln(w)
			}
		}
		for _, r := range f.Relocs {
			if r.Func == fn.Name {
				fmt.Fprintf(w, "  reloc +%d w=%d %s %s%+d flags=%#x\n", r.Offset, r.Width, r.Kind, r.Target, r.Addend, r.Flags)
			}
		}
	}
	for _, d := range f.Data {
		fmt.Fprintf(w, "data %s size=%d align=%d flags=%#x ptrbits=%d bitmap=%x\n",
			d.Name, d.Size, 1<<d.Log2Align, d.Flags, d.NBits, d.Bitmap)
	}
}
