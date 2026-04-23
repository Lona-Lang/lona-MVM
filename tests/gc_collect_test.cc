#include "mvm/error.hh"
#include "mvm/runtime.hh"
#include "mvm/runtime_memory.hh"

#include <cstdio>
#include <cstdlib>

namespace {

void require(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "gc_collect_test: %s\n", message);
        std::exit(1);
    }
}

}  // namespace

int main(int argc, char **argv) {
    require(argc == 2, "expected exactly one bitcode path argument");

    mvm::Options options;
    options.optLevel = 1;
    options.inputPath = argv[1];

    mvm::clearLastGCCollectionStats();
    auto exitCodeOrErr = mvm::runManagedProgram(options);
    if (!exitCodeOrErr) {
        std::fputs(mvm::renderError(exitCodeOrErr.takeError()).c_str(), stderr);
        return 1;
    }

    require(*exitCodeOrErr == 0, "managed program exited with a non-zero status");

    auto stats = mvm::getLastGCCollectionStats();
    require(stats.collectionCount != 0, "collector did not run");
    require(stats.sweptObjectCount != 0, "collector did not sweep any garbage");
    require(stats.sweptBytes != 0, "collector did not reclaim any bytes");
    require(stats.heapObjectCountAfter < stats.heapObjectCountBefore,
            "collector did not reduce heap object count");
    return 0;
}
