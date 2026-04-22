#pragma once

#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/Error.h"

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

llvm::Error runManagedGCPasses(llvm::Module &module,
                               llvm::ModuleAnalysisManager &moduleAnalysisManager);

void requestGC();
void clearGCRequest();
bool isGCRequested();

}  // namespace mvm

extern "C" void __mvm_gc_safepoint_poll();
