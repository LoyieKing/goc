# goc's LLVM-side code, built against an installed LLVM 19:
#   libGocPasses.so   opt plugin: goc-color-escape, print<goc-color>,
#                     goc-inline-gate, goc-stackmap, goc-reanchor
#   goc-llc           llc plus GocFrameAddrFix / GocStackmapPlacement, the Go
#                     stack check (X86 and AArch64 hooks) and the .goc.* tables
#
#   cmake -S llvm -B build/llvm -G Ninja -DLLVM_DIR=$(llvm-config --cmakedir) \
#         -DGOC_X86_SRC=<llvm-project>/llvm/lib/Target/X86 \
#         -DGOC_X86_BUILD=<llvm-build>/lib/Target/X86
# scripts/build-passes.sh fills these in.
cmake_minimum_required(VERSION 3.20)
project(goc-llvm LANGUAGES CXX)

find_package(LLVM REQUIRED CONFIG)
if(NOT LLVM_VERSION_MAJOR EQUAL 19)
  message(FATAL_ERROR "goc: needs LLVM 19, found ${LLVM_PACKAGE_VERSION}")
endif()
message(STATUS "goc: LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")
if(DEFINED GOC_EXPECT_LLVM_DIR AND NOT LLVM_DIR STREQUAL GOC_EXPECT_LLVM_DIR)
  message(FATAL_ERROR "goc: found LLVM in ${LLVM_DIR}, wanted ${GOC_EXPECT_LLVM_DIR}")
endif()

# goc-llc subclasses X86 machine passes, which needs LLVM's private
# Target/X86 headers and the generated X86Gen*.inc files.
set(GOC_X86_SRC "" CACHE PATH "llvm-project/llvm/lib/Target/X86")
set(GOC_X86_BUILD "" CACHE PATH "<llvm build>/lib/Target/X86")
if(NOT EXISTS "${GOC_X86_SRC}/X86InstrInfo.h" OR
   NOT EXISTS "${GOC_X86_BUILD}/X86GenInstrInfo.inc")
  message(FATAL_ERROR "goc: set GOC_X86_SRC / GOC_X86_BUILD to LLVM 19 X86 headers "
                      "(got '${GOC_X86_SRC}' / '${GOC_X86_BUILD}')")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
if(NOT LLVM_ENABLE_RTTI)
  add_compile_options(-fno-rtti)
endif()

separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_definitions(${LLVM_DEFINITIONS_LIST})
include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
link_directories(${LLVM_LIBRARY_DIRS})

# Link tools against the LLVM dylib when the install has one, as the old
# Makefiles did with -lLLVM-19.
if(LLVM_LINK_LLVM_DYLIB OR TARGET LLVM)
  set(GOC_LLVM_LIBS LLVM)
else()
  llvm_map_components_to_libnames(GOC_LLVM_LIBS
    core irreader support passes codegen mc target x86 all-targets asmparser)
endif()

# The IR passes, all linked into the plugin.
#   lib/Analysis    read-only: pointer colors, safepoints, stack addresses
#   lib/Transforms  the IR rewrites built on them
set(GOC_COLOR_SRCS
  lib/Analysis/GocColor/ColorAnalysis.cpp lib/Analysis/GocColor/ColorPrinter.cpp
  lib/Transforms/ColorEscape/GocColorEscape.cpp
  lib/Transforms/ColorEscape/CERewrite.cpp lib/Transforms/ColorEscape/CECheck.cpp)
add_library(GocCore OBJECT ${GOC_COLOR_SRCS}
  lib/Analysis/StackMap/SafepointAnalysis.cpp
  lib/Transforms/StackMap/GocStackMap.cpp lib/Transforms/StackMap/SMRoots.cpp
  lib/Transforms/StackMap/SMStackMap.cpp lib/Transforms/StackMap/SMReanchor.cpp
  lib/Transforms/StackMap/SMUptrInline.cpp)

# No LLVM libraries: the plugin resolves LLVM symbols from the loading opt.
# Linking libLLVM in would run its static initializers a second time and
# crash dlopen.
add_library(GocPasses MODULE lib/Plugin/GocPasses.cpp $<TARGET_OBJECTS:GocCore>)
set_target_properties(GocPasses PROPERTIES PREFIX "lib" OUTPUT_NAME GocPasses)

# goc-llc: llc plus goc's machine passes (Goc/MachinePasses.h).
add_executable(goc-llc tools/goc-llc/goc_llc.cpp
  lib/CodeGen/GocStackCheck.cpp lib/CodeGen/GocStackmapPlacement.cpp
  lib/Object/GocObjTables.cpp lib/Object/GocObjTablesWriter.cpp
  lib/Target/X86/GocX86StackCheck.cpp
  lib/Target/X86/GocX86FrameAddrFix.cpp lib/Target/X86/GocX86FrameAddrLattice.cpp
  lib/Target/AArch64/GocAArch64StackCheck.cpp lib/Target/AArch64/GocAArch64Frame.cpp)
target_include_directories(goc-llc PRIVATE ${GOC_X86_SRC} ${GOC_X86_BUILD})
target_link_libraries(goc-llc PRIVATE ${GOC_LLVM_LIBS})
