#pragma once

#include "llvm/Support/Error.h"
#include <string>

namespace mvm {

inline llvm::Error makeError(const std::string &message) {
    return llvm::createStringError(llvm::inconvertibleErrorCode(), "%s",
                                   message.c_str());
}

std::string renderError(llvm::Error error);

}  // namespace mvm
