#include "mvm/gc.hh"

#include "mvm/error.hh"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include <atomic>

namespace mvm {
namespace {

std::atomic<bool> gcRequested{false};

bool shouldAttachManagedGC(const llvm::Function &function) {
    if (function.isDeclaration() || function.isIntrinsic()) {
        return false;
    }
    return function.getName() != kSafepointPollFunctionName;
}

llvm::Expected<llvm::Function *> getOrCreateSafepointPoll(llvm::Module &module) {
    auto &context = module.getContext();
    auto *pollType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), false);

    if (auto *existing = module.getFunction(kSafepointPollFunctionName)) {
        if (existing->getFunctionType() != pollType) {
            return makeError("managed GC helper `" +
                             std::string(kSafepointPollFunctionName) +
                             "` must have signature `void ()`\n");
        }
        if (!existing->isDeclaration()) {
            return existing;
        }
    }

    auto runtimeCallee =
        module.getOrInsertFunction(kRuntimeSafepointPollSymbol, pollType);
    auto *runtimePoll = llvm::cast<llvm::Function>(runtimeCallee.getCallee());

    llvm::Function *poll = module.getFunction(kSafepointPollFunctionName);
    if (!poll) {
        poll = llvm::Function::Create(pollType, llvm::GlobalValue::InternalLinkage,
                                      kSafepointPollFunctionName, module);
    }

    poll->setLinkage(llvm::GlobalValue::InternalLinkage);
    poll->addFnAttr(llvm::Attribute::NoInline);

    if (!poll->empty()) {
        poll->deleteBody();
    }

    auto *entryBlock = llvm::BasicBlock::Create(context, "entry", poll);
    llvm::IRBuilder<> builder(entryBlock);
    builder.CreateCall(runtimePoll);
    builder.CreateRetVoid();
    return poll;
}

}  // namespace

llvm::Error prepareManagedGCModule(llvm::Module &module) {
    bool hasManagedFunctions = false;
    for (auto &function : module) {
        if (!shouldAttachManagedGC(function)) {
            continue;
        }
        function.setGC(kManagedGCStrategy);
        hasManagedFunctions = true;
    }

    if (!hasManagedFunctions) {
        return llvm::Error::success();
    }

    auto pollOrErr = getOrCreateSafepointPoll(module);
    if (!pollOrErr) {
        return pollOrErr.takeError();
    }

    (*pollOrErr)->setCallingConv(llvm::CallingConv::C);
    return llvm::Error::success();
}

void requestGC() {
    gcRequested.store(true, std::memory_order_release);
}

void clearGCRequest() {
    gcRequested.store(false, std::memory_order_release);
}

bool isGCRequested() {
    return gcRequested.load(std::memory_order_acquire);
}

}  // namespace mvm

extern "C" void __mvm_gc_safepoint_poll() {
    if (!mvm::isGCRequested()) {
        return;
    }

    // Placeholder until the VM thread owns a full safepoint handshake.
    mvm::clearGCRequest();
}
