# P18: goc-color-bridge + goc-p18-driver (links P5 Machine passes)
CXX      := clang++-19
LLVM_CFG ?= llvm-config-19

GOC_DOCS_ROOT ?= $(abspath ../..)
P5_PASS      := $(GOC_DOCS_ROOT)/p5-machinepass-goobj/pass
P5_OBJ       := $(GOC_DOCS_ROOT)/p5-machinepass-goobj/build/pass
LLVM_X86_SRC  ?= $(GOC_DOCS_ROOT)/_deps/llvm-project-19.1.7/llvm/lib/Target/X86
LLVM_X86_BUILD?= $(GOC_DOCS_ROOT)/_deps/llvm-19.1.7-build/lib/Target/X86

HAVE_X86_HDR := $(shell test -f "$(LLVM_X86_SRC)/X86InstrInfo.h" && test -f "$(LLVM_X86_BUILD)/X86GenInstrInfo.inc" && echo 1 || echo 0)
ifeq ($(HAVE_X86_HDR),0)
  $(error real X86InstrInfo headers required (X86InstrInfoLite fallback removed): missing $(LLVM_X86_SRC)/X86InstrInfo.h or $(LLVM_X86_BUILD)/X86GenInstrInfo.inc — point GOC_DOCS_ROOT/LLVM_X86_SRC/LLVM_X86_BUILD at a local llvm-project 19.1.7 source + build tree)
endif

CXXFLAGS := $(shell $(LLVM_CFG) --cxxflags) -std=c++17 -fPIC -I. -I$(P5_PASS) -I$(LLVM_X86_SRC) -I$(LLVM_X86_BUILD)

LDFLAGS  := $(shell $(LLVM_CFG) --ldflags)
LIBS     := -lLLVM-19

OBJDIR := ../build
BRIDGE := $(OBJDIR)/goc-color-bridge
DRIVER := $(OBJDIR)/goc-p18-driver

P5_PASS_OBJS := \
  $(P5_OBJ)/GocInsertStackCheck.o \
  $(P5_OBJ)/GocExpandStoreGptr.o \
  $(P5_OBJ)/GocSpillGptrsAtSafepoints.o \
  $(P5_OBJ)/GocRebuildLISAfterStackCheck.o \
  $(P5_OBJ)/GocEmitPointerMaps.o

.PHONY: all clean p5-objs

all: $(BRIDGE) $(DRIVER)

$(OBJDIR):
	mkdir -p $(OBJDIR)

p5-objs:
	$(MAKE) -C $(P5_PASS) all

$(BRIDGE): GocColorBridge.cpp | $(OBJDIR)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS) $(LIBS)

$(DRIVER): goc_p18_driver.cpp p5-objs | $(OBJDIR)
	$(CXX) $(CXXFLAGS) -o $@ goc_p18_driver.cpp $(P5_PASS_OBJS) $(LDFLAGS) $(LIBS)

clean:
	rm -f $(BRIDGE) $(DRIVER)
