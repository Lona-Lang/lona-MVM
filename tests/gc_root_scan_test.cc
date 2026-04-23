#include "mvm/error.hh"
#include "mvm/gc.hh"
#include "mvm/runtime.hh"

#include <cstdio>
#include <cstdlib>

namespace {

void require(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "gc_root_scan_test: %s\n", message);
        std::exit(1);
    }
}

}  // namespace

int main(int argc, char **argv) {
    require(argc == 2, "expected exactly one bitcode path argument");

    mvm::Options options;
    options.optLevel = 1;
    options.heapSizeBytes = 4 * 1024;
    options.inputPath = argv[1];

    auto exitCodeOrErr = mvm::runManagedProgram(options);
    if (!exitCodeOrErr) {
        std::fputs(mvm::renderError(exitCodeOrErr.takeError()).c_str(), stderr);
        return 1;
    }

    require(*exitCodeOrErr == 0, "managed program exited with a non-zero status");

    auto summary = mvm::getLastRootScanSummary();
    require(summary.safepointAddress != 0,
            "root scan did not record a safepoint address");
    require(summary.rootPairCount != 0,
            "root scan did not observe any live root pairs");
    require(summary.rootLocationCount >= summary.rootPairCount * 2,
            "root scan summary reported an invalid location count");
    require(summary.nonNullRootCount != 0,
            "root scan did not observe any non-null managed roots");
    require(summary.mutatorCount != 0,
            "root scan summary did not record any parked mutators");
    require(summary.gcCycle != 0,
            "root scan summary did not record a completed GC cycle");
    return 0;
}
