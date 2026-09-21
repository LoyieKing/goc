# P17: build goc-color-escape (LLVM 19 IR color/escape driver)
CXX      := clang++-19
LLVM_CFG ?= llvm-config-19

CXXFLAGS := $(shell $(LLVM_CFG) --cxxflags) -std=c++17 -fno-rtti
LDFLAGS  := $(shell $(LLVM_CFG) --ldflags)
LIBS     := -lLLVM-19

OBJDIR := ../build
TARGET := $(OBJDIR)/goc-color-escape

.PHONY: all clean

all: $(TARGET)

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(TARGET): GocColorEscape.cpp | $(OBJDIR)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS) $(LIBS)

clean:
	rm -f $(TARGET)
