# goc's LLVM-side code, built against an installed LLVM 19:
#   libGocPasses.so   opt plugin: goc-color-escape, goc-inline-gate,
#                     goc-stackmap, goc-reanchor, goc-pin-i64
#   goc-color-escape  standalone driver for the color/escape transform
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

# Shared core, compiled once and linked into both the plugin and the tool.
add_library(GocCore OBJECT
  lib/Transforms/ColorEscape/GocColorEscape.cpp
  lib/Transforms/StackMap/GocStackMap.cpp)

# No LLVM libraries: the plugin resolves LLVM symbols from the loading opt.
# Linking libLLVM in would run its static initializers a second time and
# crash dlopen.
add_library(GocPasses MODULE lib/Plugin/GocPasses.cpp $<TARGET_OBJECTS:GocCore>)
set_target_properties(GocPasses PROPERTIES PREFIX "lib" OUTPUT_NAME GocPasses)

add_executable(goc-color-escape tools/goc-color-escape/goc_color_escape.cpp
  lib/Transforms/ColorEscape/GocColorEscape.cpp)
target_link_libraries(goc-color-escape PRIVATE ${GOC_LLVM_LIBS})

add_executable(goc-llc tools/goc-llc/goc_llc.cpp tools/goc-llc/GocObjTables.cpp
  lib/CodeGen/GocStackCheck.cpp lib/Target/X86/GocX86StackCheck.cpp
  lib/Target/AArch64/GocAArch64StackCheck.cpp lib/Target/AArch64/GocAArch64Frame.cpp)
target_include_directories(goc-llc PRIVATE ${GOC_X86_SRC} ${GOC_X86_BUILD})
target_link_libraries(goc-llc PRIVATE ${GOC_LLVM_LIBS})
