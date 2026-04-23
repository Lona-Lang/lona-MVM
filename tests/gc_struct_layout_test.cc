#include "mvm/error.hh"
#include "mvm/runtime.hh"
#include "mvm/runtime_memory.hh"

#include <cstdio>
#include <cstdlib>

namespace {

void require(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "gc_struct_layout_test: %s\n", message);
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
    require(stats.heapObjectCountBefore == 3,
            "expected exactly three managed allocations before collection");
    require(stats.sweptObjectCount == 1,
            "collector did not preserve the object reachable through a struct field");
    require(stats.heapObjectCountAfter == 2,
            "collector should keep the holder object and its referenced array");
    return 0;
}
