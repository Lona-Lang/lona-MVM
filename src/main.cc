#include "mvm/cli.hh"
#include "mvm/error.hh"
#include "mvm/runtime.hh"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/raw_ostream.h"

int main(int argc, char **argv) {
    llvm::InitLLVM initLLVM(argc, argv);

    auto optionsOrErr = mvm::parseCommandLine(argc, argv);
    if (!optionsOrErr) {
        llvm::errs() << mvm::renderError(optionsOrErr.takeError());
        return 1;
    }

    auto options = std::move(*optionsOrErr);
    if (options.showHelp) {
        mvm::printUsage(llvm::outs(), argv[0]);
        return 0;
    }

    auto exitCodeOrErr = mvm::runManagedProgram(options);
    if (!exitCodeOrErr) {
        llvm::errs() << mvm::renderError(exitCodeOrErr.takeError());
        return 1;
    }

    return *exitCodeOrErr;
}
