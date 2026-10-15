package main

import (
	"bufio"
	"debug/elf"
	"fmt"
	"math"
	"os"

	"github.com/LoyieKing/goc/gocobj"
	"github.com/LoyieKing/goc/goobj/enc/cmd/objlib/bio"
	"github.com/LoyieKing/goc/goobj/enc/cmd/objlib/goobj"
	"github.com/LoyieKing/goc/goobj/enc/cmd/objlib/obj"
	"github.com/LoyieKing/goc/goobj/enc/cmd/objlib/obj/x86"
	"github.com/LoyieKing/goc/goobj/enc/cmd/objlib/objabi"
	"github.com/LoyieKing/goc/goobj/enc/cmd/objlib/src"
	"github.com/LoyieKing/goc/goobj/enc/stdinternal/abi"
)

func (c *converter) write(out string) error {
	arch := &x86.Linkamd64
	if c.arm64 {
		arch = &linkARM64
	}
	ctxt := obj.Linknew(arch)
	c.ctxt = ctxt
	ctxt.IsAsm = true
	ctxt.Pkgpath = c.pkg
	ctxt.DiagFunc = func(format string, args ...any) {
		fatalf("diag: "+format, args...)
	}
	ctxt.Bso = bufio.NewWriter(os.Stdout)
	defer ctxt.Bso.Flush()
	if ctxt.Arch.Init != nil {
		ctxt.Arch.Init(ctxt)
	}
	file := "goc"
	if len(c.f.Files) > 0 && c.f.Files[0] != "" {
		file = c.f.Files[0]
	}
	fileBase := src.NewFileBase(file, file)
	pos := ctxt.PosTable.XPos(src.MakePos(fileBase, 1, 0))
	// 1.4: every .goc.files entry gets a PosBase, so FILE values and
	// inline-tree call sites map to the object's file table.
	fileBases := []*src.PosBase{fileBase}
	for _, f := range c.f.Files[min(1, len(c.f.Files)):] {
		b := src.NewFileBase(f, f)
		ctxt.PosTable.XPos(src.MakePos(b, 1, 0))
		fileBases = append(fileBases, b)
	}
	goFile := func(i int64) int64 {
		if i < 0 || i >= int64(len(fileBases)) {
			return 0
		}
		return int64(fileBases[i].FileIndex())
	}

	for _, x := range c.fns {
		x.sym = ctxt.LookupABI(c.goName[x.name], x.abi)
	}
	if err := c.emitData(); err != nil {
		return err
	}

	// TEXT, FUNCDATA and PCDATA through the assembler's plist, which sets
	// up each symbol's FuncInfo; the bytes and tables are filled in after.
	var head, tail *obj.Prog
	add := func(p *obj.Prog) {
		p.Pos = pos
		if head == nil {
			head = p
		} else {
			tail.Link = p
		}
		tail = p
	}
	stackIdx := map[*fn][]gocobj.PCEntry{}
	for _, x := range c.fns {
		t := x.tab
		flags := 0
		if t.Flags&gocobj.FuncNosplit != 0 {
			flags |= obj.NOSPLIT
		}
		ctxt.InitTextSym(x.sym, flags, pos)
		tp := ctxt.NewProg()
		tp.As = obj.ATEXT
		tp.Ctxt = ctxt
		tp.From = obj.Addr{Type: obj.TYPE_MEM, Name: obj.NAME_EXTERN, Sym: x.sym}
		tp.To = obj.Addr{Type: obj.TYPE_TEXTSIZE, Offset: 0}
		tp.To.Val = int32(0)
		x.sym.Func().Text = tp
		add(tp)
		if t.GC != nil {
			args, locals, idx := stackmaps(t.GC)
			stackIdx[x] = idx
			for _, m := range []struct {
				kind int64
				name string
				blob []byte
			}{
				{gv.FuncdataArgs, "gclocals.goc." + x.sym.Name + ".args", args},
				{gv.FuncdataLocals, "gclocals.goc." + x.sym.Name + ".locals", locals},
			} {
				ms := ctxt.Lookup(m.name)
				ctxt.GloblPos(ms, int64(len(m.blob)), obj.RODATA|obj.DUPOK, src.NoXPos)
				writeBytes(ctxt, ms, m.blob)
				p := ctxt.NewProg()
				p.As = obj.AFUNCDATA
				p.From = obj.Addr{Type: obj.TYPE_CONST, Offset: m.kind}
				p.To = obj.Addr{Type: obj.TYPE_MEM, Name: obj.NAME_EXTERN, Sym: ms}
				add(p)
			}
		}
		p := ctxt.NewProg()
		p.As = obj.APCDATA
		p.From = obj.Addr{Type: obj.TYPE_CONST, Offset: gv.PcdataStackMapIndex}
		p.To = obj.Addr{Type: obj.TYPE_CONST, Offset: -1}
		add(p)
	}
	// 1.4: Go's linker reads the FuncID and start line of every inlined
	// function from its FuncInfo. A static C function inlined everywhere
	// has no body here, so it gets a one-byte stub (never referenced, so
	// the linker leaves it out of the binary).
	type stub struct {
		sym  *obj.LSym
		line uint64
	}
	var stubs []stub
	for _, x := range c.fns {
		for _, n := range x.tab.Inl {
			if _, ok := c.goName[n.Func]; ok {
				continue
			}
			ls := c.inlSym(n.Func)
			if ls.Func() != nil && ls.Func().Text != nil {
				continue
			}
			ctxt.InitTextSym(ls, obj.NOSPLIT|obj.DUPOK, pos)
			tp := ctxt.NewProg()
			tp.As = obj.ATEXT
			tp.Ctxt = ctxt
			tp.From = obj.Addr{Type: obj.TYPE_MEM, Name: obj.NAME_EXTERN, Sym: ls}
			tp.To = obj.Addr{Type: obj.TYPE_TEXTSIZE, Offset: 0}
			tp.To.Val = int32(0)
			ls.Func().Text = tp
			add(tp)
			stubs = append(stubs, stub{ls, n.StartLine})
		}
	}
	obj.Flushplist(ctxt, &obj.Plist{Firstpc: head}, nil)
	for _, st := range stubs {
		st.sym.P = []byte{0xcc} // int3
		if c.arm64 {
			st.sym.P = []byte{0, 0, 0x20, 0xd4} // brk #0
		}
		st.sym.Size = int64(len(st.sym.P))
		st.sym.Set(obj.AttrNoFrame, true)
		fi := st.sym.Func()
		fi.StartLine = int32(st.line)
		fi.Pcln.Pcdata = make([]*obj.LSym, gv.PcdataStackMapIndex+1)
		for i := range fi.Pcln.Pcdata {
			fi.Pcln.Pcdata[i] = pctab(ctxt, 0, nil)
		}
		fi.Pcln.Pcsp = pctab(ctxt, st.sym.Size, []gocobj.PCEntry{{PC: 0, Value: 0}})
		fi.Pcln.Pcfile = pctab(ctxt, st.sym.Size, []gocobj.PCEntry{{PC: 0, Value: 0}})
		fi.Pcln.Pcline = pctab(ctxt, st.sym.Size, []gocobj.PCEntry{{PC: 0, Value: int64(st.line)}})
		fi.Pcln.UsedFiles = map[goobj.CUFileIndex]struct{}{0: {}}
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
				s.AddRel(ctxt, obj.Reloc{Off: int32(off), Siz: 4, Type: typ, Add: goAdd, Sym: target})
			}

			fi := s.Func()
			fi.Locals = int32(t.FrameSize)
			fi.Args = int32(t.ArgSize)
			s.Set(obj.AttrNoFrame, t.FrameSize == 0)
			s.Set(obj.AttrNoSplit, t.Flags&gocobj.FuncNosplit != 0)
			need := abi.PCDATA_StackMapIndex + 1
			if len(fi.Pcln.Pcdata) < need {
				n := make([]*obj.LSym, need)
				copy(n, fi.Pcln.Pcdata)
				fi.Pcln.Pcdata = n
			}
			for i := range fi.Pcln.Pcdata {
				if fi.Pcln.Pcdata[i] == nil {
					fi.Pcln.Pcdata[i] = pctab(ctxt, 0, nil)
				}
			}
			if sp := pcTable(t, gocobj.PCSPDelta); sp != nil {
				fi.Pcln.Pcsp = pctab(ctxt, s.Size, sp)
			} else {
				fi.Pcln.Pcsp = pctab(ctxt, s.Size, []gocobj.PCEntry{{PC: 0, Value: int64(t.FrameSize)}})
			}
			if up := pcTable(t, gocobj.PCUnsafePoint); up != nil {
				fi.Pcln.Pcdata[abi.PCDATA_UnsafePoint] = pctab(ctxt, s.Size, up)
			}
			fi.Pcln.Pcdata[abi.PCDATA_StackMapIndex] = pctab(ctxt, s.Size, stackIdx[x])
			fi.Pcln.Pcfile = pctab(ctxt, s.Size, []gocobj.PCEntry{{PC: 0, Value: 1}})
			fi.Pcln.Pcline = pctab(ctxt, s.Size, []gocobj.PCEntry{{PC: 0, Value: 1}})
			if t.StartLine != 0 {
				fi.StartLine = int32(t.StartLine)
			}
			if ft, lt := pcTable(t, gocobj.PCFile), pcTable(t, gocobj.PCLine); ft != nil && lt != nil {
				// 1.4: real C files and lines from the debug locations.
				fi.Pcln.UsedFiles = map[goobj.CUFileIndex]struct{}{}
				gf := make([]gocobj.PCEntry, len(ft))
				for i, e := range ft {
					gf[i] = gocobj.PCEntry{PC: e.PC, Value: goFile(e.Value)}
					fi.Pcln.UsedFiles[goobj.CUFileIndex(gf[i].Value)] = struct{}{}
				}
				fi.Pcln.Pcfile = pctab(ctxt, s.Size, gf)
				fi.Pcln.Pcline = pctab(ctxt, s.Size, lt)
				fi.Pcln.InlTree = obj.InlTree{}
				for _, n := range t.Inl {
					p := ctxt.PosTable.XPos(src.MakePos(fileBases[min(int(n.File), len(fileBases)-1)], uint(n.Line), 0))
					k := fi.Pcln.InlTree.Add(int(n.Parent), p, c.inlSym(n.Func), n.Func)
					fi.Pcln.InlTree.SetParentPC(k, int32(n.ParentPC))
				}
				if it := pcTable(t, gocobj.PCInlIndex); it != nil && len(t.Inl) > 0 {
					fi.Pcln.Pcinline = pctab(ctxt, s.Size, it)
				}
			}
		}
	}

	ctxt.NumberSyms()
	buf, err := bio.Create(out)
	if err != nil {
		return err
	}
	buf.WriteString(objabi.HeaderString())
	fmt.Fprintf(buf, "!\n")
	obj.WriteObjFile(ctxt, buf)
	return buf.Close()
}
