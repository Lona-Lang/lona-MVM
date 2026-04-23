#pragma once

#include "llvm/Support/Error.h"

namespace llvm {
class Module;
}

namespace mvm {

llvm::Error annotateManagedHeapLayouts(llvm::Module &module);

}  // namespace mvm
