#pragma once

#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"

namespace mvm {

class ManagedVerifier {
public:
    llvm::Error verify(const llvm::Module &module, llvm::StringRef stage) const;
};

llvm::Error verifyLLVMModule(const llvm::Module &module, llvm::StringRef stage);

}  // namespace mvm
