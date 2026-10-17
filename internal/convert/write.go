package convert

import (
	"bufio"
	"debug/elf"
	"fmt"
	"math"
	"os"

	"github.com/LoyieKing/goc/internal/gocobj"
	"github.com/LoyieKing/goc/internal/gometa"
	"github.com/LoyieKing/goc/internal/goobj"
)

func (c *converter) write(out string, meta *gometa.Meta, header string) error {
	goarch := "amd64"
	if c.arm64 {
		goarch = "arm64"
	}
	ctxt, err := goobj.NewLink(meta, goarch, c.pkg)
	if err != nil {
		return err
	}
	c.ctxt = ctxt
	exps := headerExperiments(header)
	ctxt.Dwarf5 = exps["dwarf5"]
	ctxt.RegabiWrappers = exps["regabiwrappers"]
	funcdataArgs := meta.ABI.Funcdata["ArgsPointerMaps"]
	funcdataLocals := meta.ABI.Funcdata["LocalsPointerMaps"]
	pcUnsafe := meta.ABI.Pcdata["UnsafePoint"]
	pcStackMap := meta.ABI.Pcdata["StackMapIndex"]

	// File table: .goc.files in order (a repeated name keeps its first
	// index). Every TEXT symbol starts at line 1 of file 0.
	file := "goc"
	if len(c.f.Files) > 0 && c.f.Files[0] != "" {
		file = c.f.Files[0]
	}
	fileIdx := []int64{int64(ctxt.AddFile(file))}
	for _, f := range c.f.Files[min(1, len(c.f.Files)):] {
		fileIdx = append(fileIdx, int64(ctxt.AddFile(f)))
	}
	goFile := func(i int64) int64 {
		if i < 0 || i >= int64(len(fileIdx)) {
			return 0
		}
		return fileIdx[i]
	}

	for _, x := range c.fns {
		x.sym = ctxt.LookupABI(c.goName[x.name], x.abi)
	}
	// Every function and thunk is a TEXT symbol; an overestimate only widens
	// the DWARF index fields (go1.25+).
	ctxt.DwTextCount = len(c.fns) + len(c.goName)
	if err := c.emitData(); err != nil {
		return err
	}

	// TEXT symbols with their FUNCDATA; the bytes and tables are filled in
	// after Flush, as the assembler path did.
	stackIdx := map[*fn][]gocobj.PCEntry{}
	for _, x := range c.fns {
		t := x.tab
		flags := 0
		if t.Flags&gocobj.FuncNosplit != 0 {
			flags |= goobj.TextNoSplit
		}
		if err := ctxt.InitText(x.sym, flags); err != nil {
			return err
		}
		if t.GC != nil {
			args, locals, idx := stackmaps(t.GC)
			stackIdx[x] = idx
			for _, m := range []struct {
				kind int
				name string
				blob []byte
			}{
				{funcdataArgs, "gclocals.goc." + x.sym.Name + ".args", args},
				{funcdataLocals, "gclocals.goc." + x.sym.Name + ".locals", locals},
			} {
				ms := ctxt.Lookup(m.name)
				if err := ctxt.Globl(ms, int64(len(m.blob)), goobj.RODATA|goobj.DUPOK); err != nil {
					return err
				}
				ms.WriteBytes(0, m.blob)
				ctxt.Funcdata(x.sym, m.kind, ms)
			}
		}
		ctxt.PCDATA(x.sym)
	}
	// 1.4: Go's linker reads the FuncID and start line of every inlined
	// function from its FuncInfo. A static C function inlined everywhere
	// has no body here, so it gets a one-byte stub (never referenced, so
	// the linker leaves it out of the binary).
	type stub struct {
		sym  *goobj.Sym
		line uint64
	}
	var stubs []stub
	for _, x := range c.fns {
		for _, n := range x.tab.Inl {
			if _, ok := c.goName[n.Func]; ok {
				continue
			}
			ls := c.inlSym(n.Func)
			if ls.Func != nil {
				continue
			}
			if err := ctxt.InitText(ls, goobj.TextNoSplit|goobj.TextDupok); err != nil {
				return err
			}
			stubs = append(stubs, stub{ls, n.StartLine})
		}
	}
	if err := ctxt.Flush(); err != nil {
		return err
	}
	for _, st := range stubs {
		st.sym.P = []byte{0xcc} // int3
		if c.arm64 {
			st.sym.P = []byte{0, 0, 0x20, 0xd4} // brk #0
		}
		st.sym.Size = int64(len(st.sym.P))
		fi := st.sym.Func
		fi.StartLine = int32(st.line)
		fi.Pcdata = make([]*goobj.Sym, pcStackMap+1)
		for i := range fi.Pcdata {
			fi.Pcdata[i] = pctab(ctxt, 0, nil)
		}
		fi.Pcsp = pctab(ctxt, st.sym.Size, []gocobj.PCEntry{{PC: 0, Value: 0}})
		fi.Pcfile = pctab(ctxt, st.sym.Size, []gocobj.PCEntry{{PC: 0, Value: 0}})
		fi.Pcline = pctab(ctxt, st.sym.Size, []gocobj.PCEntry{{PC: 0, Value: int64(st.line)}})
		fi.UsedFiles = map[uint32]bool{0: true}
	}

	// Calls, by function and offset of their relocated field: the tables
	// list every call, so R_CALL vs R_PCREL is read from them.
	callAt := map[string]map[uint64]bool{}
	for _, r := range c.f.Relocs {
		if r.Kind == gocobj.RCall {
			if callAt[r.Func] == nil {
				callAt[r.Func] = map[uint64]bool{}
			}
			callAt[r.Func][r.Offset] = true
		}
	}
	for _, sec := range c.textSecs() {
		l := c.bySec[sec]
		rs, err := c.relas(sec)
		if err != nil {
			return err
		}
		d, err := c.ef.Sections[sec].Data()
		if err != nil {
			return err
		}
		for _, x := range l {
			t := x.tab
			s := x.sym
			s.P = append([]byte(nil), d[x.off:x.off+t.TextSize]...)
			s.Size = int64(len(s.P))
			s.R = nil
			if c.arm64 {
				if err := c.arm64TextRelocs(x, rs); err != nil {
					return err
				}
			}
			for _, r := range rs {
				if c.arm64 {
					break
				}
				if r.Off < x.off || r.Off >= x.off+t.TextSize {
					continue
				}
				off := r.Off - x.off
				target, goAdd, ename := c.resolveTarget(r)
				if target == nil {
					return fmt.Errorf("reloc at %s+%#x: unresolved symbol %q", s.Name, off, ename)
				}
				ty := elf.R_X86_64(r.Type)
				if ty == elf.R_X86_64_GOTPCREL || ty == elf.R_X86_64_GOTPCRELX || ty == elf.R_X86_64_REX_GOTPCRELX {
					target, goAdd, ty = c.gotSlot(target, goAdd), 0, elf.R_X86_64_PC32
				}
				typ, ok := gv.X86Text[ty]
				if !ok {
					return fmt.Errorf("unsupported text reloc type %d in %s at %#x", r.Type, s.Name, off)
				}
				if (ty == elf.R_X86_64_PLT32 || ty == elf.R_X86_64_PC32) && callAt[x.name][off] {
					typ = gv.X86Call
					if goAdd != 0 {
						return fmt.Errorf("call at %s+%#x has addend %d", s.Name, off, goAdd)
					}
				}
				if r.Add < math.MinInt32 || r.Add > math.MaxInt32 {
					return fmt.Errorf("reloc at %s+%#x: addend %d out of range", s.Name, off, r.Add)
				}
				s.AddRel(goobj.Reloc{Off: int32(off), Siz: 4, Type: typ, Add: goAdd, Sym: target})
			}

			fi := s.Func
			fi.Locals = int32(t.FrameSize)
			fi.Args = int32(t.ArgSize)
			s.NoSplit = t.Flags&gocobj.FuncNosplit != 0
			if need := pcStackMap + 1; len(fi.Pcdata) < need {
				n := make([]*goobj.Sym, need)
				copy(n, fi.Pcdata)
				fi.Pcdata = n
			}
			for i := range fi.Pcdata {
				if fi.Pcdata[i] == nil {
					fi.Pcdata[i] = pctab(ctxt, 0, nil)
				}
			}
			if sp := pcTable(t, gocobj.PCSPDelta); sp != nil {
				fi.Pcsp = pctab(ctxt, s.Size, sp)
			} else {
				fi.Pcsp = pctab(ctxt, s.Size, []gocobj.PCEntry{{PC: 0, Value: int64(t.FrameSize)}})
			}
			if up := pcTable(t, gocobj.PCUnsafePoint); up != nil {
				fi.Pcdata[pcUnsafe] = pctab(ctxt, s.Size, up)
			}
			fi.Pcdata[pcStackMap] = pctab(ctxt, s.Size, stackIdx[x])
			fi.Pcfile = pctab(ctxt, s.Size, []gocobj.PCEntry{{PC: 0, Value: 1}})
			fi.Pcline = pctab(ctxt, s.Size, []gocobj.PCEntry{{PC: 0, Value: 1}})
			if t.StartLine != 0 {
				fi.StartLine = int32(t.StartLine)
			}
			if ft, lt := pcTable(t, gocobj.PCFile), pcTable(t, gocobj.PCLine); ft != nil && lt != nil {
				// 1.4: real C files and lines from the debug locations.
				fi.UsedFiles = map[uint32]bool{}
				gf := make([]gocobj.PCEntry, len(ft))
				for i, e := range ft {
					gf[i] = gocobj.PCEntry{PC: e.PC, Value: goFile(e.Value)}
					fi.UsedFiles[uint32(gf[i].Value)] = true
				}
				fi.Pcfile = pctab(ctxt, s.Size, gf)
				fi.Pcline = pctab(ctxt, s.Size, lt)
				fi.InlTree = nil
				for _, n := range t.Inl {
					fi.InlTree = append(fi.InlTree, goobj.InlNode{
						Parent:   int32(n.Parent),
						File:     uint32(fileIdx[min(int(n.File), len(fileIdx)-1)]),
						Line:     int32(n.Line),
						Func:     c.inlSym(n.Func),
						ParentPC: int32(n.ParentPC),
					})
				}
				if it := pcTable(t, gocobj.PCInlIndex); it != nil && len(t.Inl) > 0 {
					fi.Pcinline = pctab(ctxt, s.Size, it)
				}
			}
		}
	}

	f, err := os.Create(out)
	if err != nil {
		return err
	}
	bw := bufio.NewWriter(f)
	if err := ctxt.Write(bw, header); err != nil {
		f.Close()
		return err
	}
	if err := bw.Flush(); err != nil {
		f.Close()
		return err
	}
	return f.Close()
}

// headerExperiments returns the experiments an object header's X: field
// enables.
func headerExperiments(h string) map[string]bool {
	out := map[string]bool{}
	for _, f := range splitFields(h) {
		if len(f) > 2 && f[:2] == "X:" {
			for _, e := range splitComma(f[2:]) {
				out[e] = true
			}
		}
	}
	return out
}
