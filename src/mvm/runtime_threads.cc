#include "mvm/runtime_threads.hh"

#include "mvm/gc.hh"
#include "mvm/runtime_memory.hh"
#include "llvm/Support/PrettyStackTrace.h"

namespace mvm {
namespace {

std::mutex installedRuntimeThreadsMutex;
RuntimeThreads *installedRuntimeThreads = nullptr;

void mergeRootScanSummary(GCRootScanSummary &destination,
                          const GCRootScanSummary &source) {
    if (destination.safepointAddress == 0) {
        destination.safepointAddress = source.safepointAddress;
    }
    destination.rootPairCount += source.rootPairCount;
    destination.rootLocationCount += source.rootLocationCount;
    destination.nonNullRootCount += source.nonNullRootCount;
    destination.rootValues.insert(destination.rootValues.end(),
                                  source.rootValues.begin(),
                                  source.rootValues.end());
}

RuntimeThreads *getInstalledRuntimeThreads() {
    std::lock_guard lock(installedRuntimeThreadsMutex);
    return installedRuntimeThreads;
}

}  // namespace

RuntimeThreads::RuntimeThreads()
    : vmThread_([this](std::stop_token stopToken) { vmThreadMain(stopToken); }) {}

RuntimeThreads::~RuntimeThreads() {
    vmThread_.request_stop();
    condition_.notify_all();
}

void RuntimeThreads::markMutatorStarted() {
    std::lock_guard lock(mutex_);
    ++activeMutatorCount_;
    condition_.notify_all();
}

void RuntimeThreads::markMutatorStopped() {
    std::lock_guard lock(mutex_);
    if (activeMutatorCount_ != 0) {
        --activeMutatorCount_;
    }
    if (activeMutatorCount_ == 0) {
        gcPending_ = false;
        pendingRootSummary_ = {};
    }
    condition_.notify_all();
}

void RuntimeThreads::notifyGCRequested() {
    std::lock_guard lock(mutex_);
    gcPending_ = true;
    condition_.notify_all();
}

void RuntimeThreads::parkCurrentMutatorAtSafepoint(
    const GCRootScanSummary &summary) {
    std::unique_lock lock(mutex_);
    auto cycleEpoch = gcCycleEpoch_;
    ++parkedMutatorCount_;
    mergeRootScanSummary(pendingRootSummary_, summary);
    condition_.notify_all();
    condition_.wait(lock, [this, cycleEpoch] { return gcCycleEpoch_ != cycleEpoch; });
    --parkedMutatorCount_;
    condition_.notify_all();
}

void RuntimeThreads::vmThreadMain(std::stop_token stopToken) {
    llvm::EnablePrettyStackTraceOnSigInfoForThisThread(true);
    llvm::PrettyStackTraceString stackFrame("while running the MVM runtime thread");

    std::unique_lock lock(mutex_);
    while (!stopToken.stop_requested()) {
        condition_.wait(lock, stopToken, [this] { return gcPending_; });
        if (stopToken.stop_requested()) {
            break;
        }

        if (!gcPending_ || activeMutatorCount_ == 0) {
            continue;
        }

        condition_.wait(lock, stopToken, [this] {
            return activeMutatorCount_ == 0 ||
                   parkedMutatorCount_ >= activeMutatorCount_;
        });
        if (stopToken.stop_requested()) {
            break;
        }
        if (!gcPending_ || activeMutatorCount_ == 0) {
            continue;
        }

        ++completedGCCycleCount_;
        auto collectionStats = collectManagedHeap(pendingRootSummary_.rootValues);
        pendingRootSummary_.mutatorCount = activeMutatorCount_;
        pendingRootSummary_.gcCycle = completedGCCycleCount_;
        recordLastRootScanSummary(pendingRootSummary_);
        (void)collectionStats;

        clearGCRequest();
        gcPending_ = false;
        pendingRootSummary_ = {};
        ++gcCycleEpoch_;
        condition_.notify_all();
    }
}

void installRuntimeThreads(RuntimeThreads &runtimeThreads) {
    std::lock_guard lock(installedRuntimeThreadsMutex);
    installedRuntimeThreads = &runtimeThreads;
}

void clearRuntimeThreads() {
    std::lock_guard lock(installedRuntimeThreadsMutex);
    installedRuntimeThreads = nullptr;
}

void notifyRuntimeGCRequested() {
    if (auto *runtimeThreads = getInstalledRuntimeThreads()) {
        runtimeThreads->notifyGCRequested();
    }
}

bool parkCurrentMutatorForGC(const GCRootScanSummary &summary) {
    auto *runtimeThreads = getInstalledRuntimeThreads();
    if (!runtimeThreads) {
        return false;
    }
    runtimeThreads->parkCurrentMutatorAtSafepoint(summary);
    return true;
}

}  // namespace mvm
