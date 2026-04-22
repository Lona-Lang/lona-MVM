#pragma once

#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/ExecutionEngine/RuntimeDyld.h"
#include "llvm/Support/Error.h"
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <memory>
#include <vector>

namespace mvm {

inline constexpr const char *kManagedGCStrategy = "statepoint-example";
inline constexpr const char *kSafepointPollFunctionName = "gc.safepoint_poll";
inline constexpr const char *kRuntimeSafepointPollSymbol =
    "__mvm_gc_safepoint_poll";
inline constexpr const char *kManagedGCModuleMetadataName = "mvm.gc.module";
inline constexpr const char *kManagedGCFunctionMetadataName = "mvm.gc.function";
inline constexpr const char *kManagedGCStatepointMetadataName =
    "mvm.gc.statepoint";
inline constexpr const char *kManagedGCRelocateMetadataName =
    "mvm.gc.relocate";

struct GCRootLocation {
    enum class Kind {
        Register,
        Direct,
        Indirect,
        Constant,
    };

    Kind kind = Kind::Indirect;
    std::uint16_t dwarfRegister = 0;
    std::int32_t offset = 0;
    std::uint16_t size = 0;
    std::uint64_t constant = 0;
};

struct GCRootLocationPair {
    GCRootLocation base;
    GCRootLocation derived;
};

struct GCRootScanSummary {
    std::uintptr_t safepointAddress = 0;
    std::size_t rootPairCount = 0;
    std::size_t rootLocationCount = 0;
    std::size_t nonNullRootCount = 0;
    std::size_t mutatorCount = 0;
    std::uint64_t gcCycle = 0;
};

class GCStackMapRegistry {
public:
    llvm::Error registerObject(const llvm::object::ObjectFile &objectFile,
                               const llvm::RuntimeDyld::LoadedObjectInfo &loadedInfo);
    void recordRegistrationError(llvm::Error error);
    llvm::Error takeRegistrationError();
    llvm::Expected<GCRootScanSummary> scanCurrentSafepoint(std::uintptr_t returnAddress,
                                                           std::uintptr_t callerSP,
                                                           std::uintptr_t callerBP) const;

private:
    struct SafepointRecord {
        std::uintptr_t instructionAddress = 0;
        std::vector<GCRootLocationPair> rootPairs;
    };

    mutable std::mutex mutex_;
    std::vector<SafepointRecord> safepoints_;
    llvm::Error pendingRegistrationError = llvm::Error::success();
};

std::shared_ptr<GCStackMapRegistry> createGCStackMapRegistry();
void installGCStackMapRegistry(std::shared_ptr<GCStackMapRegistry> registry);
void clearGCStackMapRegistry();
void registerMutatorThread();
void unregisterMutatorThread();
void recordLastRootScanSummary(const GCRootScanSummary &summary);
void clearLastRootScanSummary();
GCRootScanSummary getLastRootScanSummary();

llvm::Error runManagedGCPasses(llvm::Module &module,
                               llvm::ModuleAnalysisManager &moduleAnalysisManager);

void recordManagedAllocation(std::uint64_t bytes);
void resetGCAllocationBudget();
void requestGC();
void clearGCRequest();
bool isGCRequested();
void handlePendingRuntimeGCSafepoint(std::uintptr_t *runtimeFrame);

}  // namespace mvm

extern "C" void __mvm_gc_safepoint_poll();
extern "C" void __mvm_request_gc();
