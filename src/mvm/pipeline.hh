#pragma once

#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"

namespace mvm {

class ModulePipeline {
public:
    llvm::Error run(llvm::Module &module, int optLevel) const;
};

}  // namespace mvm
