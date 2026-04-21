#pragma once

#include "mvm/cli.hh"
#include "llvm/Support/Error.h"

namespace mvm {

llvm::Expected<int> runManagedProgram(const Options &options);

}  // namespace mvm
