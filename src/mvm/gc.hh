#pragma once

#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"

namespace mvm {

inline constexpr const char *kManagedGCStrategy = "statepoint-example";
inline constexpr const char *kSafepointPollFunctionName = "gc.safepoint_poll";
inline constexpr const char *kRuntimeSafepointPollSymbol =
    "__mvm_gc_safepoint_poll";

llvm::Error prepareManagedGCModule(llvm::Module &module);

void requestGC();
void clearGCRequest();
bool isGCRequested();

}  // namespace mvm

extern "C" void __mvm_gc_safepoint_poll();
