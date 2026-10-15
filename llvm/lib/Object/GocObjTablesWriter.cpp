//===- GocObjTablesWriter.cpp - .goc.* sections as assembler text ---------===//
//
// GocObjTables::doFinalization: after the last function, writes every
// collected table (see docs/gocobj-format.md) to the -goc-tables file.
//===----------------------------------------------------------------------===//
#include "GocObjTablesImpl.h"

namespace goc::objtab {

// Assembler text for the .goc.* sections.
struct Writer {
  raw_ostream &OS;
  StringMap<unsigned> Strs;
  std::vector<std::string> StrList;
  unsigned Rec = 0;

  unsigned str(StringRef S) {
    auto [It, New] = Strs.try_emplace(S, StrList.size());
    if (New)
      StrList.push_back(S.str());
    return It->second;
  }
  std::string ref(StringRef S) {
    return ".Lgoc_s" + std::to_string(str(S)) + "-.Lgoc_strtab";
  }
  void section(StringRef Name) { OS << "\t.section " << Name << ",\"\",%progbits\n"; }
  // A record: uleb length, then the body between two labels.
  std::string begin() {
    std::string L = ".Lgoc_r" + std::to_string(Rec++);
    OS << L << ":\n\t.uleb128 " << L << "e-" << L << "b\n" << L << "b:\n";
    return L;
  }
  void end(const std::string &L) { OS << L << "e:\n"; }
  void uleb(const Twine &E) { OS << "\t.uleb128 " << E << "\n"; }
  void sleb(const Twine &E) { OS << "\t.sleb128 " << E << "\n"; }
  void u8(unsigned V) { OS << "\t.byte " << V << "\n"; }
};

bool GocObjTables::doFinalization(Module &M) {
  std::error_code EC;
  raw_fd_ostream OS(Path, EC, sys::fs::OF_Text);
  if (EC) {
    errs() << "goc-llc: " << Path << ": " << EC.message() << "\n";
    return false;
  }
  Writer W{OS};
  OS << "# .goc.* tables written by goc-llc (docs/gocobj-format.md)\n";

  // gcmaps first: funcs records point at them.
  std::vector<std::string> GC(Funcs.size());
  W.section(".goc.gcmaps");
  OS << ".Lgoc_gcbase:\n";
  for (size_t I = 0; I < Funcs.size(); ++I) {
    // 1.3: one safepoint per call, with the locals bitmap the frame has
    // there: bit i is the word at SP+8*i, SP = BP-(frame-8). Set are the
    // sptr slots, and the roots of the STACKMAPs whose site this call is
    // (Direct BP-relative slots and Constant SP offsets of stack-passed
    // pointers).
    const FuncInfo &F = Funcs[I];
    if (F.Calls.empty())
      continue;
    int64_t Words = F.FrameSize > 8 ? (int64_t)(F.FrameSize - 8) / 8 : 0;
    // At the morestack call the frame is the entry one plus the cold
    // block's saves; the locals bitmap must reach the deepest stub slot.
    for (const goc::StubSlot &S : F.Stack.StubSlots)
      Words = std::max<int64_t>(Words, S.WordBelowVarp);
    std::vector<uint8_t> Base((Words + 7) / 8);
    auto Set = [&](std::vector<uint8_t> &B, int64_t Off) {
      if (Off < 8 || Off % 8 != 0 || Off / 8 > Words)
        return;
      int64_t Bit = Words - Off / 8;
      if (Bit >= 0 && Bit < Words)
        B[Bit / 8] |= 1u << (Bit % 8);
    };
    for (uint64_t O : F.SptrSlots)
      Set(Base, (int64_t)O);
    std::string L = W.begin();
    GC[I] = L;
    W.uleb("0");
    W.uleb(Twine(Words));
    W.uleb(Twine(F.Calls.size()));
    for (const CallSite &C : F.Calls) {
      std::vector<uint8_t> B = Base;
      if (C.Morestack) {
        // Only the spilled pointer arguments are live; the body's slots
        // are not set up yet.
        B.assign(B.size(), 0);
        for (const goc::StubSlot &S : F.Stack.StubSlots)
          if (F.SysvPtrRegs & (1ull << S.ArgReg))
            Set(B, 8 * (int64_t)S.WordBelowVarp);
      }
      for (const SMLoc &Lc : C.Roots) {
        if (F.A64) {
          // Bytes below varp (entry SP - 8). X29 = entry SP + A64FPSave
          // (the frame record), SP = entry SP - FrameSize.
          if (Lc.Type == 2 && Lc.Reg == 29 && F.A64HasFP && Lc.Size == 8)
            Set(B, -(F.A64FPSave + (int64_t)Lc.Off) - 8);
          else if (Lc.Type == 2 && Lc.Reg == 31 && Lc.Size == 8)
            Set(B, (int64_t)F.FrameSize - 8 - Lc.Off);
          else if (Lc.Type == 4 && Lc.Off >= 0 && Lc.Off % 8 == 0)
            Set(B, (int64_t)F.FrameSize - 8 - Lc.Off);
          continue;
        }
        if (Lc.Type == 2 && Lc.Reg == 6 && Lc.Off < 0 && Lc.Size == 8)
          Set(B, -(int64_t)Lc.Off);
        else if (Lc.Type == 4 && Lc.Off >= 0 && Lc.Off % 8 == 0)
          Set(B, (int64_t)F.FrameSize - 8 - Lc.Off);
      }
      W.uleb(C.Ret + "-" + F.Name);
      for (uint8_t V : B)
        W.u8(V);
      W.uleb("0"); // ncolor
    }
    W.end(L);
  }

  std::vector<std::string> PCV(Funcs.size());
  W.section(".goc.pcvalue");
  OS << ".Lgoc_pcvbase:\n";
  for (size_t I = 0; I < Funcs.size(); ++I) {
    const FuncInfo &F = Funcs[I];
    std::string L = W.begin();
    PCV[I] = L;
    auto Entry = [&](const std::string &PC, int64_t V) {
      W.uleb(PC.empty() ? std::string("0") : PC + "-" + F.Name);
      W.sleb(Twine(V));
    };
    int64_t Frame = (int64_t)F.FrameSize;
    unsigned NLine = F.Lines.empty() ? 0 : (F.Inl.empty() ? 2 : 3);
    if (!F.Stack.HasCheck) {
      W.uleb(Twine(1 + NLine));
      W.u8(1); // SP_DELTA: the body frame from the first instruction
      W.uleb("1");
      Entry("", Frame);
    } else {
      W.uleb(Twine(2 + NLine));
      W.u8(1); // SP_DELTA: entry, body, then the cold block's pushes
      W.uleb(Twine(3 + F.Stack.SPChanges.size()));
      Entry("", 0);
      Entry(F.Stack.BodyStart, Frame);
      Entry(F.Stack.StubStart, 0);
      for (const auto &C : F.Stack.SPChanges)
        Entry(C.first, C.second);
      W.u8(4); // UNSAFE_POINT: the check and the cold block
      W.uleb("3");
      Entry("", -2);
      Entry(F.Stack.BodyStart, -1);
      Entry(F.Stack.StubStart, -2);
    }
    if (NLine) {
      // 1.4: FILE, LINE, INL_INDEX. A row whose label lands on the same pc
      // as the next one (an instruction that emits no bytes) is replaced
      // by it.
      for (unsigned K : {2u, 3u, 5u}) {
        if (K == 5 && F.Inl.empty())
          continue;
        W.u8(K);
        W.uleb(Twine(F.Lines.size()));
        for (const LineRow &R : F.Lines)
          Entry(R.PC, K == 2 ? (int64_t)R.File : K == 3 ? (int64_t)R.Line : (int64_t)R.Inl);
      }
    }
    W.end(L);
  }

  std::vector<std::string> INL(Funcs.size());
  W.section(".goc.inltree");
  OS << ".Lgoc_inlbase:\n";
  for (size_t I = 0; I < Funcs.size(); ++I) {
    const FuncInfo &F = Funcs[I];
    if (F.Inl.empty())
      continue;
    std::string L = W.begin();
    INL[I] = L;
    W.uleb(Twine(F.Inl.size()));
    for (const InlNode &N : F.Inl) {
      W.sleb(Twine(N.Parent));
      W.uleb(Twine(N.File));
      W.uleb(Twine(N.Line));
      W.uleb(W.ref(N.Func));
      W.uleb(N.ParentPC.empty() ? std::string("0") : N.ParentPC + "-" + F.Name);
      W.uleb(Twine(N.StartLine));
    }
    W.end(L);
  }

  W.section(".goc.files");
  for (const std::string &P : FileList) {
    std::string L = W.begin();
    W.uleb(W.ref(P));
    W.end(L);
  }

  W.section(".goc.funcs");
  for (size_t I = 0; I < Funcs.size(); ++I) {
    const FuncInfo &F = Funcs[I];
    std::string L = W.begin();
    W.uleb(W.ref(F.Name));
    W.uleb(F.End + "-" + F.Name);
    W.uleb(Twine(F.FrameSize));
    W.uleb("0");          // arg_size: still in meta.json
    W.u8(SymABI);
    W.u8(0); // kind normal
    uint64_t Flags = F.Flags & ~uint64_t(7);
    if (F.Stack.HasCheck)
      Flags |= 1;
    if (F.Stack.NoSplit)
      Flags |= 4;
    if (F.Calls.empty())
      Flags |= 2;
    W.uleb(Twine(Flags | 8));
    W.u8(0);              // func_id
    W.uleb(Twine(F.File));
    W.uleb(Twine(F.StartLine));
    W.uleb(PCV[I] + "-.Lgoc_pcvbase+1");
    W.uleb(GC[I].empty() ? std::string("0") : GC[I] + "-.Lgoc_gcbase+1");
    W.uleb(INL[I].empty() ? std::string("0") : INL[I] + "-.Lgoc_inlbase+1"); // 1.4: inl
    W.uleb(Twine(F.SysvPtrRegs)); // 1.1: sysv_ptr_regs
    W.uleb(Twine(F.SptrSlots.size())); // 1.2: sptr_slots
    for (uint64_t O : F.SptrSlots)
      W.uleb(Twine(O));
    W.end(L);
  }

  W.section(".goc.relocs");
  for (const FuncInfo &F : Funcs) {
    for (const FuncAddr &A : F.Addrs) {
      std::string L = W.begin();
      W.uleb(W.ref(F.Name));
      W.uleb(A.Ret + "-4-" + F.Name);
      W.u8(4);
      W.u8(5); // FUNC_ADDR
      W.uleb(W.ref(A.Target));
      W.sleb(Twine(A.Offset - 4));
      W.u8(0);
      W.end(L);
    }
    for (const CallSite &C : F.Calls) {
      if (C.Target.empty()) {
        // CALL_INDIRECT_MARK: offset is the instruction, width its length.
        std::string L = W.begin();
        W.uleb(W.ref(F.Name));
        W.uleb(C.Pre + "-" + F.Name);
        OS << "\t.byte " << C.Ret << "-" << C.Pre << "\n";
        W.u8(2);
        W.uleb("0");
        W.sleb("0");
        W.u8(C.Safepoint ? 1 : 0);
        W.end(L);
        continue;
      }
      std::string L = W.begin();
      W.uleb(W.ref(F.Name));
      if (F.A64) {
        // BL: CALL26 on the instruction itself, no PC bias.
        W.uleb(C.Pre + "-" + F.Name);
        W.u8(4);
        W.u8(11); // ARM64_CALL26
        W.uleb(W.ref(C.Target));
        W.sleb("0");
        W.u8(C.Safepoint ? 1 : 0);
        W.end(L);
        continue;
      }
      W.uleb(C.Ret + "-4-" + F.Name);
      W.u8(4);
      W.u8(1); // CALL
      W.uleb(W.ref(C.Target));
      W.sleb("-4");
      W.u8(C.Safepoint ? 1 : 0);
      W.end(L);
    }
  }

  W.section(".goc.stackmaps");
  for (const FuncInfo &F : Funcs)
    for (const SMRec &R : F.Stackmaps) {
      std::string L = W.begin();
      W.uleb(W.ref(F.Name));
      W.uleb(R.Label + "-" + F.Name);
      W.uleb(Twine(R.ID));
      W.uleb(Twine(R.Locs.size()));
      for (const SMLoc &Lc : R.Locs) {
        W.u8(Lc.Type);
        W.uleb(Twine(Lc.Size));
        W.uleb(Twine(Lc.Reg));
        W.sleb(Twine(Lc.Off));
      }
      W.end(L);
    }

  const DataLayout &DL = M.getDataLayout();
  W.section(".goc.data");
  for (const GlobalVariable &G : M.globals()) {
    if (G.isDeclaration() || G.hasPrivateLinkage() || G.getName().starts_with("llvm."))
      continue;
    std::string Name = G.getName().str();
    std::string L = W.begin();
    W.uleb(W.ref(Name));
    W.uleb(Twine(DL.getTypeAllocSize(G.getValueType())));
    W.u8(Log2(DL.getPreferredAlign(&G)));
    uint64_t Flags = (G.isConstant() ? 1 : 0) | (G.hasLocalLinkage() ? 4 : 0);
    W.uleb(Twine(Flags));
    W.uleb("0"); // pointer bitmap: not yet
    W.end(L);
  }

  unsigned Prof = W.str(Profile);
  unsigned Prod = W.str("goc-llc (LLVM " LLVM_VERSION_STRING ")");
  W.section(".goc.header");
  OS << "\t.byte 0x47,0x4f,0x43,0x4f,0x42,0x4a,0,0\n"
     << "\t.short 1\n\t.short 4\n"
     << "\t.byte " << (A64 ? 2 : 1) << "\n\t.byte " << (goc::stackInfo().empty() ? 0 : 1) << "\n\t.short 0\n"
     << "\t.long .Lgoc_s" << Prof << "-.Lgoc_strtab\n"
     << "\t.long .Lgoc_s" << Prod << "-.Lgoc_strtab\n"
     << "\t.long 0\n";

  W.section(".goc.strtab");
  OS << ".Lgoc_strtab:\n\t.byte 0\n";
  for (size_t I = 0; I < W.StrList.size(); ++I) {
    OS << ".Lgoc_s" << I << ":\n\t.asciz \"";
    OS.write_escaped(W.StrList[I]);
    OS << "\"\n";
  }
  return false;
}

} // namespace goc::objtab
