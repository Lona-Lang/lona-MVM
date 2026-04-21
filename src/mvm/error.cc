#include "mvm/error.hh"

#include "llvm/Support/raw_ostream.h"

namespace mvm {

std::string renderError(llvm::Error error) {
    std::string text;
    llvm::raw_string_ostream out(text);
    llvm::logAllUnhandledErrors(std::move(error), out, "error: ");
    if (!text.empty() && text.back() != '\n') {
        out << '\n';
    }
    out.flush();
    return text;
}

}  // namespace mvm
