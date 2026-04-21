#include "mvm/runtime_threads.hh"

#include "llvm/Support/PrettyStackTrace.h"

namespace mvm {

RuntimeThreads::RuntimeThreads()
    : vmThread_([this](std::stop_token stopToken) { vmThreadMain(stopToken); }) {}

RuntimeThreads::~RuntimeThreads() {
    vmThread_.request_stop();
    condition_.notify_all();
}

void RuntimeThreads::markMutatorStarted() {
    std::lock_guard lock(mutex_);
    mutatorRunning_ = true;
    condition_.notify_all();
}

void RuntimeThreads::markMutatorStopped() {
    std::lock_guard lock(mutex_);
    mutatorRunning_ = false;
    condition_.notify_all();
}

void RuntimeThreads::vmThreadMain(std::stop_token stopToken) {
    llvm::EnablePrettyStackTraceOnSigInfoForThisThread(true);
    llvm::PrettyStackTraceString stackFrame("while running the MVM runtime thread");

    std::unique_lock lock(mutex_);
    while (!stopToken.stop_requested()) {
        condition_.wait(lock, stopToken, [this] { return mutatorRunning_; });
        if (stopToken.stop_requested()) {
            break;
        }

        // The mutator currently runs without external coordination. This wait is
        // the placeholder that future safepoint and GC requests will hook into.
        condition_.wait(lock, stopToken, [this] { return !mutatorRunning_; });
    }
}

}  // namespace mvm
