#include "mvm/gc.hh"

#include "mvm/error.hh"
#include "mvm/runtime_threads.hh"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
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
#include "llvm/Transforms/Scalar/PlaceSafepoints.h"
#include "llvm/Transforms/Scalar/RewriteStatepointsForGC.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <initializer_list>
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
constexpr std::uint64_t kDefaultGCAllocationThresholdBytes = 4096;

struct FunctionGCInfo {
    unsigned statepointCount = 0;
    unsigned relocateCount = 0;
    unsigned maxLiveRoots = 0;
};

std::atomic<bool> gcRequested{false};
std::atomic<std::uint64_t> gcAllocatedBytes{0};

std::mutex installedStackMapRegistryMutex;
std::shared_ptr<GCStackMapRegistry> installedStackMapRegistry;

std::mutex lastRootScanSummaryMutex;
GCRootScanSummary lastRootScanSummary;

thread_local bool currentThreadIsMutator = false;

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
    poll->addFnAttr("frame-pointer", "all");

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
    std::uintptr_t *runtimeFrame) {
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
    return registry->scanCurrentSafepoint(returnAddress, callerSP, callerBP);
#else
    return makeError("precise GC root scan is currently implemented only on "
                     "x86_64\n");
#endif
}

void rememberCurrentRuntimeSafepointOrFatal(std::uintptr_t *runtimeFrame) {
    auto summaryOrErr = scanCurrentRuntimeSafepointFromFrame(runtimeFrame);
    if (!summaryOrErr) {
        auto message = renderError(summaryOrErr.takeError());
        llvm::report_fatal_error(llvm::StringRef(message));
    }

    if (parkCurrentMutatorForGC(*summaryOrErr)) {
        return;
    }

    rememberLastRootScanSummary(*summaryOrErr);
    clearGCRequest();
    resetGCAllocationBudget();
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

    llvm::ModulePassManager gcPassManager;
    gcPassManager.addPass(
        llvm::createModuleToFunctionPassAdaptor(llvm::PlaceSafepointsPass()));
    gcPassManager.addPass(llvm::RewriteStatepointsForGC());
    gcPassManager.addPass(llvm::createModuleToFunctionPassAdaptor(
        llvm::SafepointIRVerifierPass()));
    gcPassManager.run(module, moduleAnalysisManager);

    return annotateManagedGCMetadata(module);
}

llvm::Error GCStackMapRegistry::registerObject(
    const llvm::object::ObjectFile &objectFile,
    const llvm::RuntimeDyld::LoadedObjectInfo &loadedInfo) {
    std::vector<SafepointRecord> loadedSafepoints;
    llvm::DenseMap<unsigned, llvm::object::SectionRef> originalSectionsByIndex;
    for (const auto &section : objectFile.sections()) {
        originalSectionsByIndex[static_cast<unsigned>(section.getIndex())] = section;
    }

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

        if (auto error = StackMapParser::validateHeader(stackMapSection)) {
            return error;
        }

        StackMapParser parser(stackMapSection);

        std::vector<std::uintptr_t> relocatedFunctionAddresses(parser.getNumFunctions(), 0);
        for (const auto &relocation : section.relocations()) {
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
            if (symbolIt == stackMapObject.symbol_end()) {
                return makeError("stackmap relocation is missing its target symbol\n");
            }

            auto symbolAddressOrErr = symbolIt->getAddress();
            if (!symbolAddressOrErr) {
                return symbolAddressOrErr.takeError();
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
        }

        if (llvm::all_of(relocatedFunctionAddresses,
                         [](std::uintptr_t address) { return address == 0; })) {
            struct FunctionSymbolAddress {
                std::uint64_t objectAddress = 0;
                std::uintptr_t loadedAddress = 0;
            };

            std::vector<FunctionSymbolAddress> functionSymbols;
            for (const auto &symbol : stackMapObject.symbols()) {
                auto typeOrErr = symbol.getType();
                if (!typeOrErr) {
                    return typeOrErr.takeError();
                }
                if (*typeOrErr != llvm::object::SymbolRef::ST_Function) {
                    continue;
                }

                auto symbolSectionOrErr = symbol.getSection();
                if (!symbolSectionOrErr) {
                    return symbolSectionOrErr.takeError();
                }
                if (*symbolSectionOrErr == stackMapObject.section_end()) {
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
                functionSymbols.push_back(FunctionSymbolAddress{
                    *symbolAddressOrErr,
                    static_cast<std::uintptr_t>(loadedSectionAddress +
                                                *symbolAddressOrErr -
                                                objectSectionAddress),
                });
            }

            llvm::sort(functionSymbols, [](const FunctionSymbolAddress &left,
                                          const FunctionSymbolAddress &right) {
                return left.objectAddress < right.objectAddress;
            });

            for (std::size_t index = 0;
                 index < relocatedFunctionAddresses.size() &&
                 index < functionSymbols.size();
                 ++index) {
                relocatedFunctionAddresses[index] = functionSymbols[index].loadedAddress;
            }
        }

        std::vector<std::uint64_t> constants;
        constants.reserve(parser.getNumConstants());
        for (auto constant : parser.constants()) {
            constants.push_back(constant.getValue());
        }

        unsigned nextRecordIndex = 0;
        std::size_t functionIndex = 0;
        for (auto function : parser.functions()) {
            auto functionAddress = function.getFunctionAddress();
            if (functionIndex < relocatedFunctionAddresses.size() &&
                relocatedFunctionAddresses[functionIndex] != 0) {
                functionAddress = relocatedFunctionAddresses[functionIndex];
            }

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
                safepoint.instructionAddress = static_cast<std::uintptr_t>(
                    functionAddress + record.getInstructionOffset());
                safepoint.rootPairs = std::move(*rootPairsOrErr);
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

llvm::Expected<GCRootScanSummary> GCStackMapRegistry::scanCurrentSafepoint(
    std::uintptr_t returnAddress, std::uintptr_t callerSP,
    std::uintptr_t callerBP) const {
    std::lock_guard lock(mutex_);

    auto it = std::find_if(safepoints_.begin(), safepoints_.end(),
                           [returnAddress](const SafepointRecord &record) {
                               return record.instructionAddress == returnAddress;
                           });
    if (it == safepoints_.end()) {
        std::uintptr_t nearestAddress = 0;
        std::uintptr_t nearestDistance = 0;
        bool haveNearest = false;
        for (const auto &record : safepoints_) {
            auto distance = record.instructionAddress > returnAddress
                                ? record.instructionAddress - returnAddress
                                : returnAddress - record.instructionAddress;
            if (!haveNearest || distance < nearestDistance) {
                haveNearest = true;
                nearestAddress = record.instructionAddress;
                nearestDistance = distance;
            }
        }

        std::string message =
            "no precise GC stackmap entry matched safepoint return address `" +
            std::to_string(returnAddress) + "`";
        if (haveNearest) {
            message += ", nearest registered safepoint was `" +
                       std::to_string(nearestAddress) + "` (distance `" +
                       std::to_string(nearestDistance) + "`)";
        }
        message += "\n";
        return makeError(message);
    }

    GCRootScanSummary summary;
    summary.safepointAddress = it->instructionAddress;
    summary.rootPairCount = it->rootPairs.size();
    summary.rootLocationCount = it->rootPairs.size() * 2;

    for (const auto &pair : it->rootPairs) {
        auto baseOrErr = resolveRootValue(pair.base, callerSP, callerBP);
        if (!baseOrErr) {
            return baseOrErr.takeError();
        }
        if (*baseOrErr != 0) {
            ++summary.nonNullRootCount;
        }

        auto derivedOrErr = resolveRootValue(pair.derived, callerSP, callerBP);
        if (!derivedOrErr) {
            return derivedOrErr.takeError();
        }
        if (*derivedOrErr != 0) {
            ++summary.nonNullRootCount;
        }
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
        gcAllocatedBytes.fetch_add(bytes, std::memory_order_acq_rel) + bytes;
    if (total >= kDefaultGCAllocationThresholdBytes) {
        requestGC();
    }
}

void resetGCAllocationBudget() {
    gcAllocatedBytes.store(0, std::memory_order_release);
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

void handlePendingRuntimeGCSafepoint(std::uintptr_t *runtimeFrame) {
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
    rememberCurrentRuntimeSafepointOrFatal(runtimeFrame);
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

extern "C" __attribute__((noinline)) void __mvm_gc_safepoint_poll() {
    if (!mvm::isGCRequested()) {
        return;
    }

    auto *runtimeFrame =
        reinterpret_cast<std::uintptr_t *>(__builtin_frame_address(0));
    mvm::handlePendingRuntimeGCSafepoint(runtimeFrame);
}
