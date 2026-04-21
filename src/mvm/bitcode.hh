#pragma once

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"
#include <memory>
#include <string>

namespace mvm {

struct LoadedModule {
    std::unique_ptr<llvm::LLVMContext> context;
    std::unique_ptr<llvm::Module> module;
};

llvm::Expected<LoadedModule> loadBitcodeModule(const std::string &path);

}  // namespace mvm
