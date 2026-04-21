#pragma once

#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"
#include <string>
#include <vector>

namespace mvm {

struct Options {
    bool showHelp = false;
    bool dumpIR = false;
    int optLevel = 2;
    std::string inputPath;
    std::string entrySymbol;
    std::vector<std::string> programArgs;
};

llvm::Expected<Options> parseCommandLine(int argc, char **argv);
void printUsage(llvm::raw_ostream &out, const char *argv0);

}  // namespace mvm
