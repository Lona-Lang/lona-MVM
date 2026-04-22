#pragma once

#include "mvm/gc.hh"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace mvm {

class RuntimeThreads {
public:
    RuntimeThreads();
    ~RuntimeThreads();

    RuntimeThreads(const RuntimeThreads &) = delete;
    RuntimeThreads &operator=(const RuntimeThreads &) = delete;

    void markMutatorStarted();
    void markMutatorStopped();
    void notifyGCRequested();
    void parkCurrentMutatorAtSafepoint(const GCRootScanSummary &summary);

private:
    void vmThreadMain(std::stop_token stopToken);

    std::mutex mutex_;
    std::condition_variable_any condition_;
    std::size_t activeMutatorCount_ = 0;
    std::size_t parkedMutatorCount_ = 0;
    bool gcPending_ = false;
    std::uint64_t gcCycleEpoch_ = 0;
    std::uint64_t completedGCCycleCount_ = 0;
    GCRootScanSummary pendingRootSummary_;
    std::jthread vmThread_;
};

void installRuntimeThreads(RuntimeThreads &runtimeThreads);
void clearRuntimeThreads();
void notifyRuntimeGCRequested();
bool parkCurrentMutatorForGC(const GCRootScanSummary &summary);

}  // namespace mvm
