#include "mvm/gc.hh"

#include "mvm/error.hh"
#include "mvm/runtime_threads.hh"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/SmallPtrSet.h"
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
#include "llvm/Object/ObjectFile.h"
#include "llvm/Object/StackMapParser.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Transforms/Scalar/RewriteStatepointsForGC.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mvm {
namespace {

using StackMapParser = llvm::StackMapParser<llvm::endianness::little>;

constexpr llvm::StringLiteral kStackMapSectionSuffix = "llvm_stackmaps";
constexpr std::uint16_t kX86_64RBPDwarfRegister = 6;
constexpr std::uint16_t kX86_64RSPDwarfRegister = 7;
constexpr std::size_t kStackMapHeaderSize = 16;
constexpr std::size_t kStackMapFunctionRecordSize = 24;
constexpr std::uint64_t kDefaultManagedHeapLimitBytes = 8 * 1024 * 1024;
constexpr llvm::StringLiteral kGCLeafFunctionAttrName = "gc-leaf-function";
constexpr llvm::StringLiteral kStatepointIDAttrName = "statepoint-id";
constexpr std::uint64_t kFirstMVMStatepointID = 1;

struct FunctionGCInfo {
    unsigned statepointCount = 0;
    unsigned relocateCount = 0;
    unsigned maxLiveRoots = 0;
};

std::atomic<bool> gcRequested{false};
std::atomic<std::uint64_t> gcTrackedHeapBytes{0};
std::atomic<std::uint64_t> gcHeapLimitBytes{kDefaultManagedHeapLimitBytes};

std::mutex installedStackMapRegistryMutex;
std::shared_ptr<GCStackMapRegistry> installedStackMapRegistry;

std::mutex lastRootScanSummaryMutex;
GCRootScanSummary lastRootScanSummary;

thread_local bool currentThreadIsMutator = false;

bool gcDebugEnabled() {
    static bool enabled = [] {
        auto *value = std::getenv("MVM_DEBUG_GC");
        return value && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

bool shouldAttachManagedGC(const llvm::Function &function) {
    if (function.isDeclaration() || function.isIntrinsic()) {
        return false;
    }
    return !function.hasFnAttribute(kGCLeafFunctionAttrName);
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
    return describeStatepointTarget(call) == kRuntimeSafepointPollSymbol;
}

bool isExplicitRuntimeSafepointFunctionName(llvm::StringRef name) {
    return name == kRuntimeSafepointPollSymbol || name == "__mvm_request_gc";
}

bool isKnownRuntimeGCLeafFunctionName(llvm::StringRef name) {
    return name == "__mvm_malloc" || name == "__mvm_malloc_typed" ||
           name == "__mvm_array_malloc" || name == "__mvm_array_malloc_typed" ||
           name == "__mvm_array_length";
}

bool isRuntimeAllocationFunctionName(llvm::StringRef name) {
    return name == "__mvm_malloc" || name == "__mvm_malloc_typed" ||
           name == "__mvm_array_malloc" || name == "__mvm_array_malloc_typed";
}

bool isRuntimeAllocationCall(const llvm::CallBase &call) {
    auto *callee = call.getCalledFunction();
    return callee && isRuntimeAllocationFunctionName(callee->getName());
}

bool functionHasLoop(llvm::Function &function) {
    if (function.empty()) {
        return false;
    }

    llvm::DominatorTree dominatorTree(function);
    llvm::LoopInfo loopInfo(dominatorTree);
    return !loopInfo.empty();
}

bool callCanRemainLeaf(const llvm::CallBase &call,
                       const llvm::SmallPtrSetImpl<const llvm::Function *> &leafFunctions) {
    if (call.isInlineAsm()) {
        return false;
    }

    auto *callee = call.getCalledFunction();
    if (!callee) {
        return false;
    }

    if (callee->isIntrinsic()) {
        return true;
    }

    if (isExplicitRuntimeSafepointFunctionName(callee->getName())) {
        return false;
    }

    return callee->hasFnAttribute(kGCLeafFunctionAttrName) ||
           leafFunctions.contains(callee);
}

void markKnownRuntimeGCLeafFunctions(llvm::Module &module) {
    for (auto &function : module) {
        if (isKnownRuntimeGCLeafFunctionName(function.getName())) {
            function.addFnAttr(kGCLeafFunctionAttrName);
        }
    }
}

void inferManagedGCLeafFunctions(llvm::Module &module) {
    markKnownRuntimeGCLeafFunctions(module);

    llvm::SmallPtrSet<const llvm::Function *, 32> leafFunctions;
    for (auto &function : module) {
        if (function.hasFnAttribute(kGCLeafFunctionAttrName)) {
            leafFunctions.insert(&function);
        }
    }

    bool changed = false;
    do {
        changed = false;
        for (auto &function : module) {
            if (function.isDeclaration() || function.isIntrinsic() ||
                function.hasFnAttribute(kGCLeafFunctionAttrName)) {
                continue;
            }

            if (functionHasLoop(function)) {
                continue;
            }

            bool canRemainLeaf = true;
            for (auto &block : function) {
                for (auto &instruction : block) {
                    auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                    if (!call) {
                        continue;
                    }
                    if (isRuntimeAllocationCall(*call)) {
                        canRemainLeaf = false;
                        break;
                    }
                    if (!callCanRemainLeaf(*call, leafFunctions)) {
                        canRemainLeaf = false;
                        break;
                    }
                }
                if (!canRemainLeaf) {
                    break;
                }
            }

            if (!canRemainLeaf) {
                continue;
            }

            function.addFnAttr(kGCLeafFunctionAttrName);
            leafFunctions.insert(&function);
            changed = true;
        }
    } while (changed);
}

llvm::Error prepareManagedGCModule(llvm::Module &module) {
    inferManagedGCLeafFunctions(module);

    bool hasManagedFunctions = false;
    for (auto &function : module) {
        if (!shouldAttachManagedGC(function)) {
            continue;
        }
        function.setGC(kManagedGCStrategy);
        function.addFnAttr("frame-pointer", "all");
        hasManagedFunctions = true;
    }

    clearNamedMetadata(module, kManagedGCModuleMetadataName);
    clearNamedMetadata(module, kManagedGCFunctionMetadataName);

    if (!hasManagedFunctions) {
        return llvm::Error::success();
    }

    auto &context = module.getContext();
    auto *i64 = llvm::Type::getInt64Ty(context);
    auto *pollType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {i64}, false);
    auto runtimeCallee =
        module.getOrInsertFunction(kRuntimeSafepointPollSymbol, pollType);
    auto *runtimePoll = llvm::cast<llvm::Function>(runtimeCallee.getCallee());
    runtimePoll->setCallingConv(llvm::CallingConv::C);
    runtimePoll->addFnAttr(llvm::Attribute::NoInline);
    runtimePoll->addFnAttr("frame-pointer", "all");
    return llvm::Error::success();
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

llvm::CallInst *insertManagedSafepointPoll(llvm::Instruction &insertBefore,
                                           llvm::Function &poll,
                                           llvm::LLVMContext &context,
                                           std::uint64_t statepointID) {
    auto *i64 = llvm::Type::getInt64Ty(context);
    llvm::IRBuilder<> builder(&insertBefore);
    builder.SetCurrentDebugLocation(insertBefore.getDebugLoc());
    auto *call = builder.CreateCall(
        &poll, {llvm::ConstantInt::get(i64, statepointID)});
    call->setCallingConv(llvm::CallingConv::C);
    call->addFnAttr(llvm::Attribute::get(
        context, kStatepointIDAttrName, std::to_string(statepointID)));
    return call;
}

bool isDirectSafepointPollCall(const llvm::Instruction &instruction) {
    auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
    if (!call) {
        return false;
    }

    auto *callee = call->getCalledFunction();
    return callee && callee->getName() == kRuntimeSafepointPollSymbol;
}

llvm::Error insertManagedSafepointPolls(llvm::Module &module) {
    auto &context = module.getContext();
    auto *i64 = llvm::Type::getInt64Ty(context);
    auto *pollType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {i64}, false);
    auto runtimeCallee =
        module.getOrInsertFunction(kRuntimeSafepointPollSymbol, pollType);
    auto *poll = llvm::cast<llvm::Function>(runtimeCallee.getCallee());
    std::uint64_t nextStatepointID = kFirstMVMStatepointID;

    for (auto &function : module) {
        if (!isManagedGCFunction(function) || function.empty()) {
            continue;
        }

        llvm::SmallVector<llvm::CallBase *, 16> allocationCalls;
        for (auto &block : function) {
            for (auto &instruction : block) {
                auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                if (call && isRuntimeAllocationCall(*call)) {
                    allocationCalls.push_back(call);
                }
            }
        }

        for (auto *call : allocationCalls) {
            auto *next = call->getNextNode();
            if (!next || isDirectSafepointPollCall(*next)) {
                continue;
            }

            insertManagedSafepointPoll(*next, *poll, context, nextStatepointID++);
        }

        llvm::DominatorTree dominatorTree(function);
        llvm::LoopInfo loopInfo(dominatorTree);
        llvm::SmallVector<llvm::Loop *, 8> worklist;
        for (auto *loop : loopInfo) {
            worklist.push_back(loop);
        }
        llvm::SmallPtrSet<llvm::BasicBlock *, 16> latchBlocks;

        while (!worklist.empty()) {
            auto *loop = worklist.pop_back_val();
            for (auto *subLoop : loop->getSubLoops()) {
                worklist.push_back(subLoop);
            }

            llvm::SmallVector<llvm::BasicBlock *, 4> loopLatches;
            loop->getLoopLatches(loopLatches);
            latchBlocks.insert(loopLatches.begin(), loopLatches.end());
        }

        for (auto *latch : latchBlocks) {
            auto *terminator = latch->getTerminator();
            if (!terminator) {
                continue;
            }

            insertManagedSafepointPoll(*terminator, *poll, context,
                                       nextStatepointID++);
        }
    }

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

bool isStackMapSection(llvm::StringRef sectionName) {
    return sectionName == ".llvm_stackmaps" || sectionName == "llvm_stackmaps";
}

void rememberLastRootScanSummary(const GCRootScanSummary &summary) {
    std::lock_guard lock(lastRootScanSummaryMutex);
    lastRootScanSummary = summary;
}

std::shared_ptr<GCStackMapRegistry> getInstalledGCStackMapRegistry() {
    std::lock_guard lock(installedStackMapRegistryMutex);
    return installedStackMapRegistry;
}

llvm::Expected<GCRootScanSummary> scanCurrentRuntimeSafepointFromFrame(
    std::uintptr_t *runtimeFrame, std::uint64_t statepointID) {
    if (!currentThreadIsMutator) {
        return makeError("precise GC root scan requires execution on a registered "
                         "mutator thread\n");
    }

    auto registry = getInstalledGCStackMapRegistry();
    if (!registry) {
        return makeError("precise GC root scan requires an installed stackmap "
                         "registry\n");
    }

#if defined(__x86_64__)
    if (!runtimeFrame) {
        return makeError("precise GC root scan could not capture the runtime "
                         "helper frame\n");
    }

    auto returnAddress = runtimeFrame[1];
    auto callerSP =
        reinterpret_cast<std::uintptr_t>(runtimeFrame) + (2 * sizeof(std::uintptr_t));
    auto callerBP = runtimeFrame[0];
    if (statepointID != 0) {
        return registry->scanCurrentSafepointByID(statepointID, callerSP, callerBP);
    }
    return registry->scanCurrentSafepoint(returnAddress, callerSP, callerBP);
#else
    return makeError("precise GC root scan is currently implemented only on "
                     "x86_64\n");
#endif
}

void rememberCurrentRuntimeSafepointOrFatal(std::uintptr_t *runtimeFrame,
                                            std::uint64_t statepointID) {
    auto summaryOrErr =
        scanCurrentRuntimeSafepointFromFrame(runtimeFrame, statepointID);
    if (!summaryOrErr) {
        auto message = renderError(summaryOrErr.takeError());
        llvm::report_fatal_error(llvm::StringRef(message));
    }

    if (parkCurrentMutatorForGC(*summaryOrErr)) {
        return;
    }

    rememberLastRootScanSummary(*summaryOrErr);
    clearGCRequest();
}

llvm::Expected<std::uint64_t> readStackMapConstant(
    const StackMapParser::LocationAccessor &location,
    llvm::ArrayRef<std::uint64_t> constants) {
    switch (location.getKind()) {
    case StackMapParser::LocationKind::Constant:
        return location.getSmallConstant();
    case StackMapParser::LocationKind::ConstantIndex: {
        auto index = location.getConstantIndex();
        if (index >= constants.size()) {
            return makeError("stackmap constant index `" + std::to_string(index) +
                             "` is out of range\n");
        }
        return constants[index];
    }
    default:
        return makeError("expected stackmap constant location\n");
    }
}

llvm::Expected<GCRootLocation> parseRootLocation(
    const StackMapParser::LocationAccessor &location,
    llvm::ArrayRef<std::uint64_t> constants) {
    GCRootLocation parsed;
    parsed.size = static_cast<std::uint16_t>(location.getSizeInBytes());
    parsed.dwarfRegister = location.getDwarfRegNum();

    switch (location.getKind()) {
    case StackMapParser::LocationKind::Register:
        parsed.kind = GCRootLocation::Kind::Register;
        return parsed;
    case StackMapParser::LocationKind::Direct:
        parsed.kind = GCRootLocation::Kind::Direct;
        parsed.offset = location.getOffset();
        return parsed;
    case StackMapParser::LocationKind::Indirect:
        parsed.kind = GCRootLocation::Kind::Indirect;
        parsed.offset = location.getOffset();
        return parsed;
    case StackMapParser::LocationKind::Constant:
    case StackMapParser::LocationKind::ConstantIndex: {
        auto constantOrErr = readStackMapConstant(location, constants);
        if (!constantOrErr) {
            return constantOrErr.takeError();
        }
        parsed.kind = GCRootLocation::Kind::Constant;
        parsed.constant = *constantOrErr;
        parsed.dwarfRegister = 0;
        parsed.offset = 0;
        return parsed;
    }
    }

    return makeError("unsupported stackmap location kind in root scan\n");
}

llvm::Expected<std::vector<GCRootLocationPair>> parseRootPairs(
    const StackMapParser::RecordAccessor &record,
    llvm::ArrayRef<std::uint64_t> constants) {
    constexpr unsigned kStatepointPrefixLocationCount = 3;

    if (record.getNumLocations() < kStatepointPrefixLocationCount) {
        return makeError("stackmap record for precise GC scan is missing the "
                         "statepoint prefix locations\n");
    }

    for (unsigned index = 0; index < kStatepointPrefixLocationCount; ++index) {
        auto kind = record.getLocation(index).getKind();
        if (kind != StackMapParser::LocationKind::Constant &&
            kind != StackMapParser::LocationKind::ConstantIndex) {
            return makeError("stackmap statepoint prefix location `" +
                             std::to_string(index) +
                             "` must be encoded as a constant\n");
        }
    }

    unsigned rootLocationCount =
        record.getNumLocations() - kStatepointPrefixLocationCount;
    if (rootLocationCount % 2 != 0) {
        return makeError("stackmap root locations must be encoded as base/derived "
                         "pairs\n");
    }

    std::vector<GCRootLocationPair> pairs;
    pairs.reserve(rootLocationCount / 2);

    for (unsigned index = kStatepointPrefixLocationCount;
         index < record.getNumLocations(); index += 2) {
        auto baseOrErr = parseRootLocation(record.getLocation(index), constants);
        if (!baseOrErr) {
            return baseOrErr.takeError();
        }

        auto derivedOrErr = parseRootLocation(record.getLocation(index + 1), constants);
        if (!derivedOrErr) {
            return derivedOrErr.takeError();
        }

        pairs.push_back({*baseOrErr, *derivedOrErr});
    }

    return pairs;
}

llvm::Expected<std::uintptr_t> resolveTrackedRegisterValue(std::uint16_t dwarfRegister,
                                                           std::uintptr_t callerSP,
                                                           std::uintptr_t callerBP) {
    switch (dwarfRegister) {
    case kX86_64RSPDwarfRegister:
        return callerSP;
    case kX86_64RBPDwarfRegister:
        return callerBP;
    default:
        return makeError("precise GC root scan does not yet support DWARF register `" +
                         std::to_string(dwarfRegister) + "`\n");
    }
}

llvm::Expected<std::uintptr_t> resolveRootValue(const GCRootLocation &location,
                                                std::uintptr_t callerSP,
                                                std::uintptr_t callerBP) {
    if (location.kind != GCRootLocation::Kind::Constant &&
        location.size != sizeof(std::uintptr_t)) {
        return makeError("precise GC root scan expected pointer-sized locations, got `" +
                         std::to_string(location.size) + "` bytes\n");
    }

    switch (location.kind) {
    case GCRootLocation::Kind::Constant:
        return static_cast<std::uintptr_t>(location.constant);
    case GCRootLocation::Kind::Register:
        return resolveTrackedRegisterValue(location.dwarfRegister, callerSP, callerBP);
    case GCRootLocation::Kind::Direct: {
        auto baseOrErr =
            resolveTrackedRegisterValue(location.dwarfRegister, callerSP, callerBP);
        if (!baseOrErr) {
            return baseOrErr.takeError();
        }
        return static_cast<std::uintptr_t>(
            static_cast<std::intptr_t>(*baseOrErr) + location.offset);
    }
    case GCRootLocation::Kind::Indirect: {
        auto baseOrErr =
            resolveTrackedRegisterValue(location.dwarfRegister, callerSP, callerBP);
        if (!baseOrErr) {
            return baseOrErr.takeError();
        }

        auto slotAddress = static_cast<std::uintptr_t>(
            static_cast<std::intptr_t>(*baseOrErr) + location.offset);
        std::uintptr_t value = 0;
        std::memcpy(&value, reinterpret_cast<const void *>(slotAddress), sizeof(value));
        return value;
    }
    }

    return makeError("unsupported precise GC root location kind\n");
}

}  // namespace

llvm::Error runManagedGCPasses(llvm::Module &module,
                               llvm::ModuleAnalysisManager &moduleAnalysisManager) {
    if (auto error = prepareManagedGCModule(module)) {
        return error;
    }

    if (auto error = insertManagedSafepointPolls(module)) {
        return error;
    }

    llvm::ModulePassManager rewritePassManager;
    rewritePassManager.addPass(llvm::RewriteStatepointsForGC());
    rewritePassManager.addPass(llvm::createModuleToFunctionPassAdaptor(
        llvm::SafepointIRVerifierPass()));
    rewritePassManager.run(module, moduleAnalysisManager);

    return annotateManagedGCMetadata(module);
}

void GCStackMapRegistry::recordManagedFunctionOrder(const llvm::Module &module) {
    std::vector<std::string> functionOrder;
    if (auto *metadata = module.getNamedMetadata(kManagedGCFunctionMetadataName)) {
        functionOrder.reserve(metadata->getNumOperands());
        for (auto *operand : metadata->operands()) {
            if (!operand || operand->getNumOperands() == 0) {
                continue;
            }

            auto *valueMetadata =
                llvm::dyn_cast_or_null<llvm::ValueAsMetadata>(
                    operand->getOperand(0).get());
            if (!valueMetadata) {
                continue;
            }

            auto *function =
                llvm::dyn_cast<llvm::Function>(valueMetadata->getValue());
            if (!function) {
                continue;
            }

            functionOrder.push_back(function->getName().str());
        }
    }

    std::lock_guard lock(mutex_);
    if (gcDebugEnabled()) {
        std::fprintf(stderr, "mvm-gc: recorded managed function order=%zu\n",
                     functionOrder.size());
        for (std::size_t index = 0; index < functionOrder.size(); ++index) {
            std::fprintf(stderr, "mvm-gc: managed function[%zu]=%s\n", index,
                         functionOrder[index].c_str());
        }
    }
    managedFunctionOrder_ = std::move(functionOrder);
}

llvm::Error GCStackMapRegistry::registerObject(
    const llvm::object::ObjectFile &objectFile,
    const llvm::RuntimeDyld::LoadedObjectInfo &loadedInfo) {
    std::vector<SafepointRecord> loadedSafepoints;
    std::vector<std::string> managedFunctionOrder;
    {
        std::lock_guard lock(mutex_);
        managedFunctionOrder = managedFunctionOrder_;
    }

    llvm::DenseMap<unsigned, llvm::object::SectionRef> originalSectionsByIndex;
    for (const auto &section : objectFile.sections()) {
        originalSectionsByIndex[static_cast<unsigned>(section.getIndex())] = section;
    }

    struct FunctionSymbolAddress {
        std::uint64_t objectAddress = 0;
        std::uintptr_t loadedAddress = 0;
        std::string name;
    };

    llvm::StringMap<FunctionSymbolAddress> originalFunctionSymbolsByName;
    std::vector<FunctionSymbolAddress> functionSymbols;
    for (const auto &symbol : objectFile.symbols()) {
        auto typeOrErr = symbol.getType();
        if (!typeOrErr) {
            return typeOrErr.takeError();
        }
        if (*typeOrErr != llvm::object::SymbolRef::ST_Function) {
            continue;
        }

        auto symbolNameOrErr = symbol.getName();
        if (!symbolNameOrErr) {
            return symbolNameOrErr.takeError();
        }

        auto symbolSectionOrErr = symbol.getSection();
        if (!symbolSectionOrErr) {
            return symbolSectionOrErr.takeError();
        }
        if (*symbolSectionOrErr == objectFile.section_end()) {
            continue;
        }

        auto symbolAddressOrErr = symbol.getAddress();
        if (!symbolAddressOrErr) {
            return symbolAddressOrErr.takeError();
        }

        auto sectionIndex = static_cast<unsigned>((*symbolSectionOrErr)->getIndex());
        auto originalSectionIt = originalSectionsByIndex.find(sectionIndex);
        if (originalSectionIt == originalSectionsByIndex.end()) {
            continue;
        }

        auto objectSectionAddress = (*symbolSectionOrErr)->getAddress();
        auto loadedSectionAddress =
            loadedInfo.getSectionLoadAddress(originalSectionIt->second);
        FunctionSymbolAddress functionSymbol{
            *symbolAddressOrErr,
            static_cast<std::uintptr_t>(loadedSectionAddress +
                                        *symbolAddressOrErr - objectSectionAddress),
            symbolNameOrErr->str(),
        };
        originalFunctionSymbolsByName[functionSymbol.name] = functionSymbol;
        functionSymbols.push_back(functionSymbol);
    }

    llvm::sort(functionSymbols, [](const FunctionSymbolAddress &left,
                                  const FunctionSymbolAddress &right) {
        return left.objectAddress < right.objectAddress;
    });
    auto debugObject = loadedInfo.getObjectForDebug(objectFile);
    const auto &stackMapObject = *debugObject.getBinary();

    for (const auto &section : stackMapObject.sections()) {
        auto nameOrErr = section.getName();
        if (!nameOrErr) {
            return nameOrErr.takeError();
        }
        if (!isStackMapSection(*nameOrErr)) {
            continue;
        }

        auto contentsOrErr = section.getContents();
        if (!contentsOrErr) {
            return contentsOrErr.takeError();
        }
        if (contentsOrErr->empty()) {
            continue;
        }
        llvm::ArrayRef<std::uint8_t> stackMapSection(
            reinterpret_cast<const std::uint8_t *>(contentsOrErr->data()),
            contentsOrErr->size());

        std::vector<llvm::object::RelocationRef> stackMapRelocations;
        bool usingOriginalStackMapRelocations = false;
        for (const auto &originalSection : objectFile.sections()) {
            auto originalNameOrErr = originalSection.getName();
            if (!originalNameOrErr) {
                return originalNameOrErr.takeError();
            }
            if (*originalNameOrErr != *nameOrErr) {
                continue;
            }

            for (const auto &relocation : originalSection.relocations()) {
                stackMapRelocations.push_back(relocation);
            }
            usingOriginalStackMapRelocations = !stackMapRelocations.empty();
            break;
        }
        if (!usingOriginalStackMapRelocations) {
            for (const auto &relocation : section.relocations()) {
                stackMapRelocations.push_back(relocation);
            }
        }
        if (gcDebugEnabled()) {
            std::fprintf(stderr,
                         "mvm-gc: stackmap relocations=%zu source=%s\n",
                         stackMapRelocations.size(),
                         usingOriginalStackMapRelocations ? "object" : "debug");
        }

        if (auto error = StackMapParser::validateHeader(stackMapSection)) {
            return error;
        }

        StackMapParser parser(stackMapSection);

        std::vector<std::uintptr_t> relocatedFunctionAddresses(parser.getNumFunctions(), 0);
        for (const auto &relocation : stackMapRelocations) {
            auto relocationOffset = relocation.getOffset();
            if (relocationOffset < kStackMapHeaderSize) {
                continue;
            }
            if ((relocationOffset - kStackMapHeaderSize) % kStackMapFunctionRecordSize !=
                0) {
                continue;
            }

            auto functionIndex = static_cast<std::size_t>(
                (relocationOffset - kStackMapHeaderSize) / kStackMapFunctionRecordSize);
            if (functionIndex >= relocatedFunctionAddresses.size()) {
                continue;
            }

            auto symbolIt = relocation.getSymbol();
            if (usingOriginalStackMapRelocations) {
                if (symbolIt == objectFile.symbol_end()) {
                    return makeError(
                        "stackmap relocation is missing its target symbol\n");
                }
            } else if (symbolIt == stackMapObject.symbol_end()) {
                return makeError("stackmap relocation is missing its target symbol\n");
            }

            auto symbolAddressOrErr = symbolIt->getAddress();
            if (!symbolAddressOrErr) {
                return symbolAddressOrErr.takeError();
            }
            auto symbolNameOrErr = symbolIt->getName();
            if (!symbolNameOrErr) {
                return symbolNameOrErr.takeError();
            }
            auto symbolNameForDebug = symbolNameOrErr->str();

            auto originalSymbolIt =
                originalFunctionSymbolsByName.find(symbolNameForDebug);
            if (originalSymbolIt != originalFunctionSymbolsByName.end()) {
                relocatedFunctionAddresses[functionIndex] =
                    originalSymbolIt->second.loadedAddress;
                if (gcDebugEnabled()) {
                    std::fprintf(stderr,
                                 "mvm-gc: stackmap relocation function[%zu] symbol=%s "
                                 "resolved=%llu\n",
                                 functionIndex, symbolNameForDebug.c_str(),
                                 static_cast<unsigned long long>(
                                     relocatedFunctionAddresses[functionIndex]));
                }
                continue;
            }

            auto symbolSectionOrErr = symbolIt->getSection();
            if (!symbolSectionOrErr) {
                return symbolSectionOrErr.takeError();
            }
            if (*symbolSectionOrErr == stackMapObject.section_end()) {
                return makeError("stackmap relocation target symbol is not attached to a "
                                 "loaded section\n");
            }

            auto sectionIndex = static_cast<unsigned>((*symbolSectionOrErr)->getIndex());
            auto originalSectionIt = originalSectionsByIndex.find(sectionIndex);
            if (originalSectionIt == originalSectionsByIndex.end()) {
                return makeError("stackmap relocation target section index `" +
                                 std::to_string(sectionIndex) +
                                 "` was not found in the loaded object\n");
            }

            auto objectSectionAddress = (*symbolSectionOrErr)->getAddress();
            auto loadedSectionAddress =
                loadedInfo.getSectionLoadAddress(originalSectionIt->second);
            relocatedFunctionAddresses[functionIndex] =
                static_cast<std::uintptr_t>(loadedSectionAddress +
                                            *symbolAddressOrErr -
                                            objectSectionAddress);
            if (gcDebugEnabled()) {
                std::fprintf(stderr,
                             "mvm-gc: stackmap relocation function[%zu] symbol=%s "
                             "symbol_addr=%llu section_addr=%llu loaded_section=%llu "
                             "computed=%llu\n",
                             functionIndex, symbolNameForDebug.c_str(),
                             static_cast<unsigned long long>(*symbolAddressOrErr),
                             static_cast<unsigned long long>(objectSectionAddress),
                             static_cast<unsigned long long>(loadedSectionAddress),
                             static_cast<unsigned long long>(
                                 relocatedFunctionAddresses[functionIndex]));
            }
        }

        auto functionSymbolsByLoadedAddress = functionSymbols;
        llvm::sort(functionSymbolsByLoadedAddress,
                   [](const FunctionSymbolAddress &left,
                      const FunctionSymbolAddress &right) {
                       return left.loadedAddress < right.loadedAddress;
                   });

        std::vector<std::uint64_t> constants;
        constants.reserve(parser.getNumConstants());
        for (auto constant : parser.constants()) {
            constants.push_back(constant.getValue());
        }

        auto findFunctionEndAddress =
            [&functionSymbolsByLoadedAddress](std::uintptr_t functionAddress) {
                auto it = llvm::upper_bound(
                    functionSymbolsByLoadedAddress, functionAddress,
                    [](std::uintptr_t address, const FunctionSymbolAddress &symbol) {
                        return address < symbol.loadedAddress;
                    });
                if (it == functionSymbolsByLoadedAddress.end()) {
                    return std::numeric_limits<std::uintptr_t>::max();
                }
                return it->loadedAddress;
            };

        unsigned nextRecordIndex = 0;
        std::size_t functionIndex = 0;
        for (auto function : parser.functions()) {
            auto functionAddress = function.getFunctionAddress();
            bool resolvedFunctionAddress = false;
            if (functionIndex < relocatedFunctionAddresses.size() &&
                relocatedFunctionAddresses[functionIndex] != 0) {
                functionAddress = relocatedFunctionAddresses[functionIndex];
                resolvedFunctionAddress = true;
            }
            if (!resolvedFunctionAddress &&
                functionIndex < managedFunctionOrder.size()) {
                auto mappedSymbolIt =
                    originalFunctionSymbolsByName.find(managedFunctionOrder[functionIndex]);
                if (mappedSymbolIt != originalFunctionSymbolsByName.end()) {
                    functionAddress = mappedSymbolIt->second.loadedAddress;
                    resolvedFunctionAddress = true;
                    if (gcDebugEnabled()) {
                        std::fprintf(stderr,
                                     "mvm-gc: stackmap function[%zu] metadata=%s "
                                     "resolved=%llu\n",
                                     functionIndex,
                                     managedFunctionOrder[functionIndex].c_str(),
                                     static_cast<unsigned long long>(
                                         functionAddress));
                    }
                }
            }
            if (!resolvedFunctionAddress && !functionSymbols.empty()) {
                auto it = llvm::upper_bound(
                    functionSymbols, function.getFunctionAddress(),
                    [](std::uint64_t address, const FunctionSymbolAddress &symbol) {
                        return address < symbol.objectAddress;
                    });
                if (it != functionSymbols.begin()) {
                    --it;
                    functionAddress = static_cast<std::uintptr_t>(
                        it->loadedAddress +
                        (function.getFunctionAddress() - it->objectAddress));
                }
            }
            auto functionEndAddress = findFunctionEndAddress(functionAddress);

            for (std::uint64_t recordIndex = 0; recordIndex < function.getRecordCount();
                 ++recordIndex) {
                if (nextRecordIndex >= parser.getNumRecords()) {
                    return makeError("stackmap record count does not match function "
                                     "record descriptors\n");
                }

                auto record = parser.getRecord(nextRecordIndex++);
                auto rootPairsOrErr = parseRootPairs(record, constants);
                if (!rootPairsOrErr) {
                    return rootPairsOrErr.takeError();
                }
                SafepointRecord safepoint;
                safepoint.id = record.getID();
                safepoint.functionAddress = functionAddress;
                safepoint.functionEndAddress = functionEndAddress;
                safepoint.instructionAddress = static_cast<std::uintptr_t>(
                    functionAddress + record.getInstructionOffset());
                safepoint.rootPairs = std::move(*rootPairsOrErr);
                if (gcDebugEnabled()) {
                    std::fprintf(stderr,
                                 "mvm-gc: stackmap function[%zu] base=%llu relocated=%llu "
                                 "record_id=%llu statepoint_id=%llu record_offset=%llu "
                                 "safepoint=%llu roots=%zu\n",
                                 functionIndex,
                                 static_cast<unsigned long long>(
                                     function.getFunctionAddress()),
                                 static_cast<unsigned long long>(functionAddress),
                                 static_cast<unsigned long long>(record.getID()),
                                 static_cast<unsigned long long>(safepoint.id),
                                 static_cast<unsigned long long>(
                                     record.getInstructionOffset()),
                                 static_cast<unsigned long long>(
                                     safepoint.instructionAddress),
                                 safepoint.rootPairs.size());
                }
                loadedSafepoints.push_back(std::move(safepoint));
            }
            ++functionIndex;
        }

        if (nextRecordIndex != parser.getNumRecords()) {
            return makeError("stackmap parser left unconsumed records after decoding "
                             "function descriptors\n");
        }
    }

    std::lock_guard lock(mutex_);
    safepoints_.insert(safepoints_.end(),
                       std::make_move_iterator(loadedSafepoints.begin()),
                       std::make_move_iterator(loadedSafepoints.end()));
    return llvm::Error::success();
}

void GCStackMapRegistry::recordRegistrationError(llvm::Error error) {
    std::lock_guard lock(mutex_);
    if (pendingRegistrationError) {
        llvm::consumeError(std::move(error));
        return;
    }
    pendingRegistrationError = std::move(error);
}

llvm::Error GCStackMapRegistry::takeRegistrationError() {
    std::lock_guard lock(mutex_);
    auto error = std::move(pendingRegistrationError);
    pendingRegistrationError = llvm::Error::success();
    return error;
}

llvm::Expected<GCRootScanSummary> GCStackMapRegistry::scanCurrentSafepointByID(
    std::uint64_t statepointID, std::uintptr_t callerSP,
    std::uintptr_t callerBP) const {
    std::lock_guard lock(mutex_);

    auto findSafepointRecord =
        [this](std::uintptr_t address) -> const SafepointRecord * {
        auto it = std::find_if(safepoints_.begin(), safepoints_.end(),
                               [address](const SafepointRecord &record) {
                                   return record.instructionAddress == address;
                               });
        if (it == safepoints_.end()) {
            return nullptr;
        }
        return &*it;
    };

    auto findSafepointRecordForReturnAddress =
        [this](std::uintptr_t address) -> const SafepointRecord * {
        const SafepointRecord *best = nullptr;
        std::uintptr_t bestDistance = 0;
        constexpr std::uintptr_t kMaxReturnAddressDistance = 4096;

        for (const auto &record : safepoints_) {
            if (record.instructionAddress > address) {
                continue;
            }

            auto distance = address - record.instructionAddress;
            if (!best || distance < bestDistance) {
                best = &record;
                bestDistance = distance;
            }
        }

        if (best && bestDistance <= kMaxReturnAddressDistance) {
            return best;
        }

        return nullptr;
    };

    auto scanSingleManagedFrame =
        [](const SafepointRecord &record, std::uintptr_t frameSP,
           std::uintptr_t frameBP,
           GCRootScanSummary &summary) -> llvm::Error {
        if (summary.safepointAddress == 0) {
            summary.safepointAddress = record.instructionAddress;
        }
        summary.rootPairCount += record.rootPairs.size();
        summary.rootLocationCount += record.rootPairs.size() * 2;

        for (const auto &pair : record.rootPairs) {
            auto baseOrErr = resolveRootValue(pair.base, frameSP, frameBP);
            if (!baseOrErr) {
                return baseOrErr.takeError();
            }
            if (*baseOrErr != 0) {
                ++summary.nonNullRootCount;
                summary.rootValues.push_back(*baseOrErr);
            }

            auto derivedOrErr = resolveRootValue(pair.derived, frameSP, frameBP);
            if (!derivedOrErr) {
                return derivedOrErr.takeError();
            }
            if (*derivedOrErr != 0) {
                ++summary.nonNullRootCount;
                summary.rootValues.push_back(*derivedOrErr);
            }
        }

        return llvm::Error::success();
    };

    const SafepointRecord *record = nullptr;
    for (const auto &candidate : safepoints_) {
        if (candidate.id == statepointID) {
            record = &candidate;
            break;
        }
    }

    if (!record) {
        return makeError("no precise GC stackmap entry matched statepoint id `" +
                         std::to_string(statepointID) + "`\n");
    }

    GCRootScanSummary summary;
    summary.rootValues.reserve(32);
    if (auto error = scanSingleManagedFrame(*record, callerSP, callerBP, summary)) {
        return std::move(error);
    }

    std::uintptr_t framePointer = callerBP;
    std::size_t walkedFrames = 0;
    constexpr std::size_t kMaxManagedFrameDepth = 256;
    while (framePointer != 0 && walkedFrames < kMaxManagedFrameDepth) {
        std::uintptr_t nextFramePointer = 0;
        std::uintptr_t nextReturnAddress = 0;
        std::memcpy(&nextFramePointer, reinterpret_cast<const void *>(framePointer),
                    sizeof(nextFramePointer));
        std::memcpy(&nextReturnAddress,
                    reinterpret_cast<const void *>(framePointer +
                                                   sizeof(std::uintptr_t)),
                    sizeof(nextReturnAddress));

        if (nextReturnAddress == 0) {
            break;
        }

        auto *callerRecord = findSafepointRecord(nextReturnAddress);
        if (!callerRecord) {
            callerRecord = findSafepointRecordForReturnAddress(nextReturnAddress);
        }
        if (!callerRecord) {
            break;
        }

        auto nextCallerSP = framePointer + (2 * sizeof(std::uintptr_t));
        if (auto error =
                scanSingleManagedFrame(*callerRecord, nextCallerSP, nextFramePointer,
                                       summary)) {
            return std::move(error);
        }

        framePointer = nextFramePointer;
        ++walkedFrames;
    }

    return summary;
}

llvm::Expected<GCRootScanSummary> GCStackMapRegistry::scanCurrentSafepoint(
    std::uintptr_t returnAddress, std::uintptr_t callerSP,
    std::uintptr_t callerBP) const {
    std::lock_guard lock(mutex_);

    auto findSafepointRecord =
        [this](std::uintptr_t address) -> const SafepointRecord * {
        auto it = std::find_if(safepoints_.begin(), safepoints_.end(),
                               [address](const SafepointRecord &record) {
                                   return record.instructionAddress == address;
                               });
        if (it == safepoints_.end()) {
            return nullptr;
        }
        return &*it;
    };

    auto findSafepointRecordForReturnAddress =
        [this](std::uintptr_t address) -> const SafepointRecord * {
        const SafepointRecord *best = nullptr;
        std::uintptr_t bestDistance = 0;
        constexpr std::uintptr_t kMaxReturnAddressDistance = 4096;

        for (const auto &record : safepoints_) {
            if (record.instructionAddress > address) {
                continue;
            }

            auto distance = address - record.instructionAddress;
            if (!best || distance < bestDistance) {
                best = &record;
                bestDistance = distance;
            }
        }

        if (best && bestDistance <= kMaxReturnAddressDistance) {
            return best;
        }

        return nullptr;
    };

    auto makeMissingSafepointMessage = [this](std::uintptr_t address) {
        std::uintptr_t nearestAddress = 0;
        std::uintptr_t nearestDistance = 0;
        bool haveNearest = false;
        for (const auto &record : safepoints_) {
            auto distance = record.instructionAddress > address
                                ? record.instructionAddress - address
                                : address - record.instructionAddress;
            if (!haveNearest || distance < nearestDistance) {
                haveNearest = true;
                nearestAddress = record.instructionAddress;
                nearestDistance = distance;
            }
        }

        std::string message =
            "no precise GC stackmap entry matched safepoint return address `" +
            std::to_string(address) + "`";
        if (haveNearest) {
            message += ", nearest registered safepoint was `" +
                       std::to_string(nearestAddress) + "` (distance `" +
                       std::to_string(nearestDistance) + "`)";
        }
        message += "\n";
        return message;
    };

    auto scanSingleManagedFrame =
        [](const SafepointRecord &record, std::uintptr_t frameSP,
           std::uintptr_t frameBP,
           GCRootScanSummary &summary) -> llvm::Error {
        if (summary.safepointAddress == 0) {
            summary.safepointAddress = record.instructionAddress;
        }
        summary.rootPairCount += record.rootPairs.size();
        summary.rootLocationCount += record.rootPairs.size() * 2;

        for (const auto &pair : record.rootPairs) {
            auto baseOrErr = resolveRootValue(pair.base, frameSP, frameBP);
            if (!baseOrErr) {
                return baseOrErr.takeError();
            }
            if (*baseOrErr != 0) {
                ++summary.nonNullRootCount;
                summary.rootValues.push_back(*baseOrErr);
            }

            auto derivedOrErr = resolveRootValue(pair.derived, frameSP, frameBP);
            if (!derivedOrErr) {
                return derivedOrErr.takeError();
            }
            if (*derivedOrErr != 0) {
                ++summary.nonNullRootCount;
                summary.rootValues.push_back(*derivedOrErr);
            }
        }

        return llvm::Error::success();
    };

    const SafepointRecord *record = nullptr;
    std::uintptr_t startFramePointer = callerBP;
    std::uintptr_t startCallerSP = callerSP;
    std::uintptr_t probeReturnAddress = returnAddress;
    std::uintptr_t probeFramePointer = callerBP;
    std::uintptr_t probeCallerSP = callerSP;
    constexpr std::size_t kMaxStartFrameProbeDepth = 8;

    for (std::size_t probeDepth = 0; probeDepth < kMaxStartFrameProbeDepth;
         ++probeDepth) {
        record = findSafepointRecord(probeReturnAddress);
        if (!record && probeDepth > 0) {
            record = findSafepointRecordForReturnAddress(probeReturnAddress);
        }
        if (record) {
            if (gcDebugEnabled()) {
                std::fprintf(stderr,
                             "mvm-gc: selected safepoint probe depth=%zu "
                             "return=%llu record=%llu\n",
                             probeDepth,
                             static_cast<unsigned long long>(probeReturnAddress),
                             static_cast<unsigned long long>(
                                 record->instructionAddress));
            }
            startFramePointer = probeFramePointer;
            startCallerSP = probeCallerSP;
            break;
        }

        if (probeFramePointer == 0) {
            break;
        }

        std::uintptr_t nextFramePointer = 0;
        std::uintptr_t nextReturnAddress = 0;
        std::memcpy(&nextFramePointer,
                    reinterpret_cast<const void *>(probeFramePointer),
                    sizeof(nextFramePointer));
        std::memcpy(&nextReturnAddress,
                    reinterpret_cast<const void *>(probeFramePointer +
                                                   sizeof(std::uintptr_t)),
                    sizeof(nextReturnAddress));

        if (gcDebugEnabled()) {
            std::fprintf(stderr,
                         "mvm-gc: safepoint probe depth=%zu current=%llu next=%llu "
                         "frame=%llu next_frame=%llu\n",
                         probeDepth,
                         static_cast<unsigned long long>(probeReturnAddress),
                         static_cast<unsigned long long>(nextReturnAddress),
                         static_cast<unsigned long long>(probeFramePointer),
                         static_cast<unsigned long long>(nextFramePointer));
        }

        probeCallerSP = probeFramePointer + (2 * sizeof(std::uintptr_t));
        probeReturnAddress = nextReturnAddress;
        probeFramePointer = nextFramePointer;
    }

    if (!record) {
        if (gcDebugEnabled()) {
            std::fprintf(stderr,
                         "mvm-gc: missing safepoint for return=%llu caller_sp=%llu "
                         "caller_bp=%llu records=%zu\n",
                         static_cast<unsigned long long>(returnAddress),
                         static_cast<unsigned long long>(callerSP),
                         static_cast<unsigned long long>(callerBP),
                         safepoints_.size());
            for (std::size_t index = 0; index < safepoints_.size() && index < 8; ++index) {
                std::fprintf(stderr, "mvm-gc: record[%zu]=%llu roots=%zu\n", index,
                             static_cast<unsigned long long>(
                                 safepoints_[index].instructionAddress),
                             safepoints_[index].rootPairs.size());
            }
        }
        return makeError(makeMissingSafepointMessage(returnAddress));
    }

    GCRootScanSummary summary;
    summary.rootValues.reserve(32);
    if (auto error =
            scanSingleManagedFrame(*record, startCallerSP, startFramePointer, summary)) {
        return std::move(error);
    }

    std::uintptr_t framePointer = startFramePointer;
    std::size_t walkedFrames = 0;
    constexpr std::size_t kMaxManagedFrameDepth = 256;
    while (framePointer != 0 && walkedFrames < kMaxManagedFrameDepth) {
        std::uintptr_t nextFramePointer = 0;
        std::uintptr_t nextReturnAddress = 0;
        std::memcpy(&nextFramePointer, reinterpret_cast<const void *>(framePointer),
                    sizeof(nextFramePointer));
        std::memcpy(&nextReturnAddress,
                    reinterpret_cast<const void *>(framePointer +
                                                   sizeof(std::uintptr_t)),
                    sizeof(nextReturnAddress));

        if (nextReturnAddress == 0) {
            break;
        }

        auto *callerRecord = findSafepointRecord(nextReturnAddress);
        if (!callerRecord) {
            callerRecord = findSafepointRecordForReturnAddress(nextReturnAddress);
        }
        if (!callerRecord) {
            break;
        }

        auto nextCallerSP = framePointer + (2 * sizeof(std::uintptr_t));
        if (auto error =
                scanSingleManagedFrame(*callerRecord, nextCallerSP, nextFramePointer,
                                       summary)) {
            return std::move(error);
        }

        framePointer = nextFramePointer;
        ++walkedFrames;
    }

    return summary;
}

std::shared_ptr<GCStackMapRegistry> createGCStackMapRegistry() {
    return std::make_shared<GCStackMapRegistry>();
}

void installGCStackMapRegistry(std::shared_ptr<GCStackMapRegistry> registry) {
    std::lock_guard lock(installedStackMapRegistryMutex);
    installedStackMapRegistry = std::move(registry);
}

void clearGCStackMapRegistry() {
    std::lock_guard lock(installedStackMapRegistryMutex);
    installedStackMapRegistry.reset();
}

void registerMutatorThread() {
    currentThreadIsMutator = true;
}

void unregisterMutatorThread() {
    currentThreadIsMutator = false;
}

void recordLastRootScanSummary(const GCRootScanSummary &summary) {
    rememberLastRootScanSummary(summary);
}

void clearLastRootScanSummary() {
    rememberLastRootScanSummary({});
}

GCRootScanSummary getLastRootScanSummary() {
    std::lock_guard lock(lastRootScanSummaryMutex);
    return lastRootScanSummary;
}

void recordManagedAllocation(std::uint64_t bytes) {
    if (bytes == 0) {
        return;
    }

    auto total =
        gcTrackedHeapBytes.fetch_add(bytes, std::memory_order_acq_rel) + bytes;
    if (total >= gcHeapLimitBytes.load(std::memory_order_acquire)) {
        requestGC();
    }
}

void updateManagedHeapUsage(std::uint64_t bytes) {
    gcTrackedHeapBytes.store(bytes, std::memory_order_release);
}

void resetManagedHeapUsage() {
    updateManagedHeapUsage(0);
}

void configureManagedHeapLimit(std::uint64_t bytes) {
    gcHeapLimitBytes.store(bytes, std::memory_order_release);
}

void requestGC() {
    auto alreadyRequested =
        gcRequested.exchange(true, std::memory_order_acq_rel);
    if (!alreadyRequested) {
        notifyRuntimeGCRequested();
    }
}

void clearGCRequest() {
    gcRequested.store(false, std::memory_order_release);
}

bool isGCRequested() {
    return gcRequested.load(std::memory_order_acquire);
}

void handlePendingRuntimeGCSafepoint(std::uintptr_t *runtimeFrame,
                                     std::uint64_t statepointID) {
    if (!isGCRequested()) {
        return;
    }

    if (!currentThreadIsMutator) {
        clearGCRequest();
        return;
    }

    auto registry = getInstalledGCStackMapRegistry();
    if (!registry) {
        clearGCRequest();
        return;
    }

    (void)registry;
    rememberCurrentRuntimeSafepointOrFatal(runtimeFrame, statepointID);
}

}  // namespace mvm

extern "C" void __mvm_request_gc() {
    mvm::requestGC();
    if (mvm::currentThreadIsMutator && mvm::getInstalledGCStackMapRegistry()) {
        auto *runtimeFrame =
            reinterpret_cast<std::uintptr_t *>(__builtin_frame_address(0));
        mvm::handlePendingRuntimeGCSafepoint(runtimeFrame);
    }
}

extern "C" __attribute__((noinline)) void __mvm_gc_safepoint_poll(
    std::uint64_t statepointID) {
    if (!mvm::isGCRequested()) {
        return;
    }

    auto *runtimeFrame =
        reinterpret_cast<std::uintptr_t *>(__builtin_frame_address(0));
    mvm::handlePendingRuntimeGCSafepoint(runtimeFrame, statepointID);
}
