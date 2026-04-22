#include "mvm/gc.hh"

#include "mvm/error.hh"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/SafepointIRVerifier.h"
#include "llvm/Transforms/Scalar/PlaceSafepoints.h"
#include "llvm/Transforms/Scalar/RewriteStatepointsForGC.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <string>

namespace mvm {
namespace {

struct FunctionGCInfo {
    unsigned statepointCount = 0;
    unsigned relocateCount = 0;
    unsigned maxLiveRoots = 0;
};

std::atomic<bool> gcRequested{false};

bool shouldAttachManagedGC(const llvm::Function &function) {
    if (function.isDeclaration() || function.isIntrinsic()) {
        return false;
    }
    return function.getName() != kSafepointPollFunctionName;
}

bool isManagedGCFunction(const llvm::Function &function) {
    return !function.isDeclaration() && function.hasGC() &&
           function.getGC() == kManagedGCStrategy;
}

bool isIntrinsicCall(const llvm::CallBase &call, llvm::Intrinsic::ID id) {
    auto *callee = call.getCalledFunction();
    return callee && callee->getIntrinsicID() == id;
}

bool isStatepointCall(const llvm::CallBase &call) {
    return isIntrinsicCall(call, llvm::Intrinsic::experimental_gc_statepoint);
}

bool isRelocateCall(const llvm::CallBase &call) {
    return isIntrinsicCall(call, llvm::Intrinsic::experimental_gc_relocate);
}

llvm::Metadata *toMetadata(llvm::LLVMContext &context, uint64_t value,
                           unsigned bitWidth) {
    auto *constant = llvm::ConstantInt::get(llvm::IntegerType::get(context, bitWidth),
                                            value);
    return llvm::ConstantAsMetadata::get(constant);
}

llvm::Metadata *toMetadata(llvm::LLVMContext &context, bool value) {
    return toMetadata(context, value ? 1 : 0, 1);
}

llvm::Metadata *toMetadata(llvm::Function &function) {
    return llvm::ValueAsMetadata::get(&function);
}

llvm::MDNode *makeMetadataTuple(
    llvm::LLVMContext &context, std::initializer_list<llvm::Metadata *> values) {
    llvm::SmallVector<llvm::Metadata *, 8> operands(values);
    return llvm::MDNode::get(context, operands);
}

void clearNamedMetadata(llvm::Module &module, llvm::StringRef name) {
    if (auto *existing = module.getNamedMetadata(name)) {
        module.eraseNamedMetadata(existing);
    }
}

unsigned getLiveRootCount(const llvm::CallBase &call) {
    unsigned liveRoots = 0;
    for (unsigned bundleIndex = 0; bundleIndex < call.getNumOperandBundles();
         ++bundleIndex) {
        auto bundle = call.getOperandBundleAt(bundleIndex);
        if (bundle.getTagName() != "gc-live") {
            continue;
        }
        liveRoots += bundle.Inputs.size();
    }
    return liveRoots;
}

std::string describeStatepointTarget(const llvm::CallBase &call) {
    if (call.arg_size() < 3) {
        return "<invalid>";
    }

    auto *callee = call.getArgOperand(2)->stripPointerCasts();
    if (auto *function = llvm::dyn_cast<llvm::Function>(callee)) {
        return std::string(function->getName());
    }
    return "<indirect>";
}

bool isSafepointPollStatepoint(const llvm::CallBase &call) {
    return describeStatepointTarget(call) == kRuntimeSafepointPollSymbol ||
           describeStatepointTarget(call) == kSafepointPollFunctionName;
}

llvm::Expected<unsigned> getRelocateIndex(const llvm::CallBase &call,
                                          unsigned operandIndex) {
    if (call.arg_size() <= operandIndex) {
        return makeError("gc.relocate is missing operand `" +
                         std::to_string(operandIndex) + "`\n");
    }

    auto *constant = llvm::dyn_cast<llvm::ConstantInt>(call.getArgOperand(operandIndex));
    if (!constant) {
        return makeError("gc.relocate operand `" +
                         std::to_string(operandIndex) +
                         "` must be an immediate constant\n");
    }
    return static_cast<unsigned>(constant->getZExtValue());
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

llvm::Error prepareManagedGCModule(llvm::Module &module) {
    bool hasManagedFunctions = false;
    for (auto &function : module) {
        if (!shouldAttachManagedGC(function)) {
            continue;
        }
        function.setGC(kManagedGCStrategy);
        hasManagedFunctions = true;
    }

    clearNamedMetadata(module, kManagedGCModuleMetadataName);
    clearNamedMetadata(module, kManagedGCFunctionMetadataName);

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

llvm::Error annotateManagedGCMetadata(llvm::Module &module) {
    auto &context = module.getContext();
    clearNamedMetadata(module, kManagedGCModuleMetadataName);
    clearNamedMetadata(module, kManagedGCFunctionMetadataName);

    auto *moduleMetadata = module.getOrInsertNamedMetadata(kManagedGCModuleMetadataName);
    auto *functionMetadata =
        module.getOrInsertNamedMetadata(kManagedGCFunctionMetadataName);

    llvm::DenseMap<const llvm::Value *, llvm::SmallVector<llvm::CallBase *, 4>>
        relocatesByStatepoint;
    llvm::DenseMap<const llvm::Function *, FunctionGCInfo> functionInfo;

    unsigned managedFunctionCount = 0;
    unsigned relocateCount = 0;
    for (auto &function : module) {
        if (!isManagedGCFunction(function)) {
            continue;
        }
        ++managedFunctionCount;
        auto &info = functionInfo[&function];
        for (auto &block : function) {
            for (auto &instruction : block) {
                auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                if (!call || !isRelocateCall(*call)) {
                    continue;
                }
                ++info.relocateCount;
                ++relocateCount;
                relocatesByStatepoint[call->getArgOperand(0)].push_back(call);
            }
        }
    }

    unsigned statepointCount = 0;
    unsigned safepointPollCount = 0;
    uint64_t nextStatepointId = 0;

    for (auto &function : module) {
        if (!isManagedGCFunction(function)) {
            continue;
        }

        auto &info = functionInfo[&function];
        for (auto &block : function) {
            for (auto &instruction : block) {
                auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                if (!call || !isStatepointCall(*call)) {
                    continue;
                }

                auto statepointId = nextStatepointId++;

                auto liveRootCount = getLiveRootCount(*call);
                auto relocateIt = relocatesByStatepoint.find(call);
                unsigned localRelocateCount =
                    relocateIt == relocatesByStatepoint.end()
                        ? 0
                        : static_cast<unsigned>(relocateIt->second.size());

                call->setMetadata(
                    kManagedGCStatepointMetadataName,
                    makeMetadataTuple(
                        context, {toMetadata(context, statepointId, 64),
                                  llvm::MDString::get(context,
                                                      describeStatepointTarget(*call)),
                                  toMetadata(context, liveRootCount, 32),
                                  toMetadata(context, localRelocateCount, 32),
                                  toMetadata(context, isSafepointPollStatepoint(*call))}));

                if (relocateIt != relocatesByStatepoint.end()) {
                    for (auto *relocate : relocateIt->second) {
                        auto baseIndexOrErr = getRelocateIndex(*relocate, 1);
                        if (!baseIndexOrErr) {
                            return baseIndexOrErr.takeError();
                        }

                        auto derivedIndexOrErr = getRelocateIndex(*relocate, 2);
                        if (!derivedIndexOrErr) {
                            return derivedIndexOrErr.takeError();
                        }

                        relocate->setMetadata(
                            kManagedGCRelocateMetadataName,
                            makeMetadataTuple(
                                context,
                                {toMetadata(context, statepointId, 64),
                                 toMetadata(context, *baseIndexOrErr, 32),
                                 toMetadata(context, *derivedIndexOrErr, 32)}));
                    }
                }

                ++info.statepointCount;
                info.maxLiveRoots = std::max(info.maxLiveRoots, liveRootCount);
                ++statepointCount;
                if (isSafepointPollStatepoint(*call)) {
                    ++safepointPollCount;
                }
            }
        }

        functionMetadata->addOperand(makeMetadataTuple(
            context, {toMetadata(function), toMetadata(context, info.statepointCount, 32),
                      toMetadata(context, info.relocateCount, 32),
                      toMetadata(context, info.maxLiveRoots, 32)}));
    }

    moduleMetadata->addOperand(makeMetadataTuple(
        context, {llvm::MDString::get(context, kManagedGCStrategy),
                  toMetadata(context, managedFunctionCount, 32),
                  toMetadata(context, statepointCount, 32),
                  toMetadata(context, relocateCount, 32),
                  toMetadata(context, safepointPollCount, 32)}));

    return llvm::Error::success();
}

}  // namespace

llvm::Error runManagedGCPasses(llvm::Module &module,
                               llvm::ModuleAnalysisManager &moduleAnalysisManager) {
    if (auto error = prepareManagedGCModule(module)) {
        return error;
    }

    llvm::ModulePassManager gcPassManager;
    gcPassManager.addPass(
        llvm::createModuleToFunctionPassAdaptor(llvm::PlaceSafepointsPass()));
    gcPassManager.addPass(llvm::RewriteStatepointsForGC());
    gcPassManager.addPass(llvm::createModuleToFunctionPassAdaptor(
        llvm::SafepointIRVerifierPass()));
    gcPassManager.run(module, moduleAnalysisManager);

    return annotateManagedGCMetadata(module);
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
