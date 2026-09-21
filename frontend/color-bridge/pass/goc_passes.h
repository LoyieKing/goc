#pragma once
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include <cstdint>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// Opaque runners (wrap MachineFunctionPass; public runOnMF bypasses protected).
// Spill/maps runners that need LiveIntervals MUST be driven via PassManager
// factories below — not the opaque runners.
struct GocPassRunner {
  virtual ~GocPassRunner() = default;
  virtual bool runOnMF(llvm::MachineFunction &MF) = 0;
};

GocPassRunner *createGocInsertStackCheckRunner(std::vector<std::string> *RecipeOut);
GocPassRunner *createGocExpandStoreGptrRunner(std::vector<std::string> *RecipeOut);
GocPassRunner *createGocEmitPointerMapsRunner(std::string OutDir);
GocPassRunner *createGocSpillGptrsAtSafepointsRunner(std::vector<std::string> *RecipeOut);
GocPassRunner *createGocRebuildLISAfterStackCheckRunner(std::vector<std::string> *RecipeOut);

// PassManager factories (ownership transferred to legacy::PassManager).
llvm::MachineFunctionPass *
createGocInsertStackCheckPass(std::vector<std::string> *RecipeOut);
llvm::MachineFunctionPass *
createGocExpandStoreGptrPass(std::vector<std::string> *RecipeOut);
llvm::MachineFunctionPass *
createGocSpillGptrsAtSafepointsPass(std::vector<std::string> *RecipeOut);
llvm::MachineFunctionPass *createGocEmitPointerMapsPass(std::string OutDir);
llvm::MachineFunctionPass *
createGocRebuildLISAfterStackCheckPass(std::vector<std::string> *RecipeOut);

// Filled by spill pass; consumed by EmitPointerMaps / RebuildLIS (Go SP layout).
namespace goc_spill_api {
std::vector<std::pair<int, int64_t>> fiGoSpOffs(llvm::MachineFunction &MF);
std::vector<std::tuple<unsigned, int, int64_t>> spills(llvm::MachineFunction &MF);
}
