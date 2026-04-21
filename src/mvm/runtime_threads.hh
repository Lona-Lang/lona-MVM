#pragma once

#include <condition_variable>
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

private:
    void vmThreadMain(std::stop_token stopToken);

    std::mutex mutex_;
    std::condition_variable_any condition_;
    bool mutatorRunning_ = false;
    std::jthread vmThread_;
};

}  // namespace mvm
