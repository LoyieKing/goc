# P27: build libGocClang.so (Clang plugin — attrs + Sema escape)
CXX      := clang++-19
LLVM_CFG ?= llvm-config-19

CXXFLAGS := $(shell $(LLVM_CFG) --cxxflags) -std=c++17 -fno-rtti -fPIC
LDFLAGS  := -shared

# Default: repo-root build/ when invoked from goc driver
REPO_ROOT ?= $(abspath ../..)
OBJDIR ?= $(REPO_ROOT)/build
TARGET ?= $(OBJDIR)/libGocClang.so

.PHONY: all clean

all: $(TARGET)

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(TARGET): GocClangPlugin.cpp | $(OBJDIR)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $<
	@echo "built $@"

clean:
	rm -f $(TARGET)
