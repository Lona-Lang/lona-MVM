#pragma once

#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"

namespace mvm {

llvm::Error specializeManagedDispatch(llvm::Module &module);

}  // namespace mvm
