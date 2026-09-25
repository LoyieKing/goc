# Build libGocMachinePasses.so + goc-pass-driver (LLVM 19)
CXX      := clang++-19
LLVM_CFG ?= llvm-config-19

# Local LLVM 19.1.7 Target/X86 headers + generated X86Gen*.inc (not in distro).
GOC_DOCS_ROOT ?= $(abspath ../..)
LLVM_X86_SRC  ?= $(GOC_DOCS_ROOT)/_deps/llvm-project-19.1.7/llvm/lib/Target/X86
LLVM_X86_BUILD?= $(GOC_DOCS_ROOT)/_deps/llvm-19.1.7-build/lib/Target/X86

# Recorded for docs / consumers:
LLVM_X86_SRC_INCLUDE   := $(LLVM_X86_SRC)
LLVM_X86_BUILD_INCLUDE := $(LLVM_X86_BUILD)

HAVE_X86_HDR := $(shell test -f "$(LLVM_X86_SRC)/X86InstrInfo.h" && test -f "$(LLVM_X86_BUILD)/X86GenInstrInfo.inc" && echo 1 || echo 0)
ifeq ($(HAVE_X86_HDR),0)
  $(error real X86InstrInfo headers required (X86InstrInfoLite fallback removed): missing $(LLVM_X86_SRC)/X86InstrInfo.h or $(LLVM_X86_BUILD)/X86GenInstrInfo.inc — point GOC_DOCS_ROOT/LLVM_X86_SRC/LLVM_X86_BUILD at a local llvm-project 19.1.7 source + build tree)
endif

CXXFLAGS := $(shell $(LLVM_CFG) --cxxflags) -std=c++17 -fPIC -I. -I$(LLVM_X86_SRC_INCLUDE) -I$(LLVM_X86_BUILD_INCLUDE)
$(info goc-pass: real X86InstrInfo.h via $(LLVM_X86_SRC_INCLUDE) + $(LLVM_X86_BUILD_INCLUDE))

LDFLAGS  := $(shell $(LLVM_CFG) --ldflags)
LIBS     := -lLLVM-19

OBJDIR := ../build/pass
OUTDIR := ../build/pass-out

# IR pass plugin (loaded by opt -load-pass-plugin): asks LLVM to report where
# sptr-colored values live at each call site (see GocStackMap.cpp).
STACKMAP_SO := $(OUTDIR)/GocStackMap.so

OBJS := $(OBJDIR)/GocInsertStackCheck.o \
        $(OBJDIR)/GocExpandStoreGptr.o \
        $(OBJDIR)/GocSpillGptrsAtSafepoints.o \
        $(OBJDIR)/GocRebuildLISAfterStackCheck.o \
        $(OBJDIR)/GocEmitPointerMaps.o \
        $(OBJDIR)/goc_pass_driver.o \
        $(OBJDIR)/goc_mir_export.o

.PHONY: all clean run show-x86-paths

all: $(OBJDIR)/goc-pass-driver $(OBJDIR)/libGocMachinePasses.so $(STACKMAP_SO)

$(OBJDIR)/GocStackMap.o: GocStackMap.cpp
	@mkdir -p $(OBJDIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# No -lLLVM: the plugin resolves LLVM symbols from the loading process, otherwise
# its static initializers clash with the host opt/llc and dlopen crashes.
$(STACKMAP_SO): $(OBJDIR)/GocStackMap.o
	@mkdir -p $(OUTDIR)
	$(CXX) -shared -o $@ $< $(LDFLAGS)

show-x86-paths:
	@echo "HAVE_X86_HDR=$(HAVE_X86_HDR)"
	@echo "LLVM_X86_SRC_INCLUDE=$(LLVM_X86_SRC_INCLUDE)"
	@echo "LLVM_X86_BUILD_INCLUDE=$(LLVM_X86_BUILD_INCLUDE)"

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(OBJDIR)/%.o: %.cpp goc_passes.h goc_x86.h | $(OBJDIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(OBJDIR)/libGocMachinePasses.so: $(OBJDIR)/GocInsertStackCheck.o \
    $(OBJDIR)/GocExpandStoreGptr.o $(OBJDIR)/GocSpillGptrsAtSafepoints.o \
    $(OBJDIR)/GocRebuildLISAfterStackCheck.o \
    $(OBJDIR)/GocEmitPointerMaps.o
	$(CXX) -shared -o $@ $^ $(LDFLAGS) $(LIBS)

$(OBJDIR)/goc-pass-driver: $(OBJS)
	$(CXX) -o $@ $^ $(LDFLAGS) $(LIBS)

run: all
	mkdir -p $(OUTDIR)
	$(OBJDIR)/goc-pass-driver --outdir $(OUTDIR) --wb

clean:
	rm -rf $(OBJDIR)
