#include "mvm/managed_pointer_lowering.hh"

#include "mvm/error.hh"
#include "mvm/managed_state.hh"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Support/raw_ostream.h"

#include <string>
#include <utility>
#include <vector>

namespace mvm {
namespace {

constexpr unsigned kManagedAddressSpace = 1;
constexpr llvm::StringLiteral kManagedObjectAllocName = "__mvm_malloc";
constexpr llvm::StringLiteral kManagedArrayAllocName = "__mvm_array_malloc";
constexpr llvm::StringLiteral kManagedTypedObjectAllocName = "__mvm_malloc_typed";
constexpr llvm::StringLiteral kManagedTypedArrayAllocName =
    "__mvm_array_malloc_typed";
constexpr llvm::StringLiteral kManagedArrayLengthName = "__mvm_array_length";

bool isConcreteManagedKind(ManagedPointerKind kind) {
    return kind == ManagedPointerKind::Object ||
           kind == ManagedPointerKind::Array;
}

std::string renderPointerState(PointerState state) {
    if (state.empty()) {
        return "unknown";
    }

    llvm::SmallVector<const char *, 4> parts;
    if (state.hasRaw()) {
        parts.push_back("raw");
    }
    if (state.hasManagedObject()) {
        parts.push_back("object");
    }
    if (state.hasManagedArray()) {
        parts.push_back("array");
    }
    if (state.hasNull()) {
        parts.push_back("null");
    }

    std::string text;
    for (auto *part : parts) {
        if (!text.empty()) {
            text += '|';
        }
        text += part;
    }
    return text;
}

std::string renderValue(const llvm::Value &value) {
    std::string text;
    llvm::raw_string_ostream out(text);
    value.print(out);
    out.flush();
    return text;
}

std::string renderType(const llvm::Type &type) {
    std::string text;
    llvm::raw_string_ostream out(text);
    type.print(out);
    out.flush();
    return text;
}

bool isManagedRuntimeDeclaration(const llvm::Function &function) {
    auto name = function.getName();
    return name == kManagedObjectAllocName || name == kManagedArrayAllocName ||
           name == kManagedTypedObjectAllocName ||
           name == kManagedTypedArrayAllocName || name == kManagedArrayLengthName;
}

llvm::PointerType *managedPointerType(llvm::LLVMContext &context) {
    return llvm::PointerType::get(context, kManagedAddressSpace);
}

void collectDominancePreorder(
    llvm::DomTreeNode *node, llvm::SmallVectorImpl<llvm::BasicBlock *> &order,
    llvm::SmallPtrSetImpl<llvm::BasicBlock *> &scheduled) {
    if (!node) {
        return;
    }

    auto *block = node->getBlock();
    if (block && scheduled.insert(block).second) {
        order.push_back(block);
    }

    for (auto *child : *node) {
        collectDominancePreorder(child, order, scheduled);
    }
}

class ManagedPointerLowerer {
public:
    explicit ManagedPointerLowerer(llvm::Module &module)
        : module_(module), analysis_(module) {}

    llvm::Error run() {
        if (auto error = analysis_.run()) {
            return error;
        }

        if (auto error = createFunctionReplacements()) {
            return error;
        }

        for (auto &[oldFunction, newFunction] : functionMap_) {
            if (oldFunction->isDeclaration()) {
                continue;
            }
            if (auto error = rebuildFunction(*oldFunction, *newFunction)) {
                return error;
            }
        }

        for (auto &[oldFunction, newFunction] : functionMap_) {
            if (oldFunction->getFunctionType() == newFunction->getFunctionType()) {
                oldFunction->replaceAllUsesWith(newFunction);
            }
        }

        for (auto &[oldFunction, _] : functionMap_) {
            if (!oldFunction->isDeclaration()) {
                oldFunction->deleteBody();
            }
        }

        for (auto &[oldFunction, newFunction] : functionMap_) {
            if (!oldFunction->use_empty()) {
                return makeError("managed pointer lowering cannot yet preserve uses "
                                 "of rewritten function `" +
                                 oldFunction->getName().str() +
                                 "` with the old raw-pointer ABI\n");
            }
        }

        for (auto &[oldFunction, _] : functionMap_) {
            oldFunction->eraseFromParent();
        }

        return llvm::Error::success();
    }

private:
    bool functionNeedsLowering(const llvm::Function &function) const {
        auto &functionState = analysis_.getFunctionState(function);
        if (isConcreteManagedKind(functionState.returnState.dispatchKind())) {
            return true;
        }

        for (auto &argument : function.args()) {
            if (!argument.getType()->isPointerTy()) {
                continue;
            }
            if (isConcreteManagedKind(
                    functionState.params[argument.getArgNo()].dispatchKind())) {
                return true;
            }
        }

        for (auto &block : function) {
            for (auto &instruction : block) {
                if (instruction.getType()->isPointerTy() &&
                    isConcreteManagedKind(
                        analysis_.getValueState(instruction).dispatchKind())) {
                    return true;
                }

                if (auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(&instruction)) {
                    if (alloca->getAllocatedType()->isPointerTy() &&
                        isConcreteManagedKind(
                            analysis_.getSlotState(*alloca).dispatchKind())) {
                        return true;
                    }
                }
            }
        }

        return false;
    }

    llvm::Expected<bool> shouldLowerFunction(llvm::Function &function) const {
        if (function.isDeclaration()) {
            return isManagedRuntimeDeclaration(function);
        }

        if (!functionNeedsLowering(function)) {
            return false;
        }

        auto &functionState = analysis_.getFunctionState(function);
        bool signatureChanges = false;
        if (isConcreteManagedKind(functionState.returnState.dispatchKind())) {
            signatureChanges = true;
        }

        for (auto &argument : function.args()) {
            if (!argument.getType()->isPointerTy()) {
                continue;
            }
            if (isConcreteManagedKind(
                    functionState.params[argument.getArgNo()].dispatchKind())) {
                signatureChanges = true;
                break;
            }
        }

        if (function.hasAddressTaken() && signatureChanges) {
            return makeError("managed pointer lowering does not yet support "
                             "address-taken function `" +
                             function.getName().str() +
                             "` whose ABI needs managed-pointer rewriting\n");
        }

        return true;
    }

    llvm::FunctionType *buildReplacementType(const llvm::Function &function) const {
        auto &context = module_.getContext();
        auto &functionState = analysis_.getFunctionState(function);

        auto *returnType = function.getReturnType();
        if (returnType->isPointerTy() &&
            isConcreteManagedKind(functionState.returnState.dispatchKind())) {
            returnType = managedPointerType(context);
        }

        llvm::SmallVector<llvm::Type *, 8> params;
        params.reserve(function.arg_size());
        for (auto &argument : function.args()) {
            auto *paramType = argument.getType();
            if (paramType->isPointerTy() &&
                isConcreteManagedKind(
                    functionState.params[argument.getArgNo()].dispatchKind())) {
                paramType = managedPointerType(context);
            }
            params.push_back(paramType);
        }

        return llvm::FunctionType::get(returnType, params, function.isVarArg());
    }

    llvm::FunctionType *buildManagedRuntimeDeclarationType(
        const llvm::Function &function) const {
        auto &context = module_.getContext();
        auto *i64 = llvm::Type::getInt64Ty(context);
        auto *ptr = llvm::PointerType::get(context, 0);
        auto *managedPtr = managedPointerType(context);

        auto name = function.getName();
        if (name == kManagedObjectAllocName) {
            return llvm::FunctionType::get(managedPtr, {}, false);
        }
        if (name == kManagedArrayAllocName) {
            return llvm::FunctionType::get(managedPtr, {i64}, false);
        }
        if (name == kManagedTypedObjectAllocName) {
            return llvm::FunctionType::get(managedPtr, {ptr}, false);
        }
        if (name == kManagedTypedArrayAllocName) {
            return llvm::FunctionType::get(managedPtr, {i64, ptr}, false);
        }
        if (name == kManagedArrayLengthName) {
            return llvm::FunctionType::get(i64, {managedPtr}, false);
        }
        return function.getFunctionType();
    }

    void copyFunctionProperties(const llvm::Function &oldFunction,
                                llvm::Function &newFunction) const {
        newFunction.setCallingConv(oldFunction.getCallingConv());
        newFunction.setLinkage(oldFunction.getLinkage());
        newFunction.setVisibility(oldFunction.getVisibility());
        newFunction.setDLLStorageClass(oldFunction.getDLLStorageClass());
        newFunction.setUnnamedAddr(oldFunction.getUnnamedAddr());
        newFunction.setDSOLocal(oldFunction.isDSOLocal());
        newFunction.setAttributes(oldFunction.getAttributes());
        if (auto *subprogram = oldFunction.getSubprogram()) {
            newFunction.setSubprogram(subprogram);
        }

        llvm::SmallVector<std::pair<unsigned, llvm::MDNode *>, 8> metadata;
        oldFunction.getAllMetadata(metadata);
        for (auto &[kind, node] : metadata) {
            newFunction.setMetadata(kind, node);
        }
    }

    llvm::Error createFunctionReplacements() {
        std::vector<llvm::Function *> functions;
        functions.reserve(module_.size());
        for (auto &function : module_) {
            functions.push_back(&function);
        }

        for (auto *function : functions) {
            auto shouldLowerOrErr = shouldLowerFunction(*function);
            if (!shouldLowerOrErr) {
                return shouldLowerOrErr.takeError();
            }
            if (!*shouldLowerOrErr) {
                continue;
            }

            llvm::FunctionType *replacementType = nullptr;
            if (function->isDeclaration()) {
                replacementType = buildManagedRuntimeDeclarationType(*function);
            } else {
                replacementType = buildReplacementType(*function);
            }

            auto originalName = function->getName().str();
            function->setName(originalName + ".__mvm.raw");

            auto *replacement = llvm::Function::Create(
                replacementType, function->getLinkage(), originalName, module_);
            copyFunctionProperties(*function, *replacement);
            functionMap_[function] = replacement;
        }

        return llvm::Error::success();
    }

    llvm::Expected<llvm::Value *> mapValue(llvm::Value &value,
                                           llvm::Type *expectedType = nullptr) {
        if (auto mappedIt = valueMap_.find(&value); mappedIt != valueMap_.end()) {
            return mappedIt->second;
        }

        if (auto mappedFnIt =
                functionMap_.find(llvm::dyn_cast<llvm::Function>(&value));
            mappedFnIt != functionMap_.end()) {
            return mappedFnIt->second;
        }

        if (auto mappedBlockIt =
                blockMap_.find(llvm::dyn_cast<llvm::BasicBlock>(&value));
            mappedBlockIt != blockMap_.end()) {
            return mappedBlockIt->second;
        }

        if (auto *null = llvm::dyn_cast<llvm::ConstantPointerNull>(&value)) {
            auto *pointerType = llvm::dyn_cast<llvm::PointerType>(
                expectedType ? expectedType : null->getType());
            if (!pointerType) {
                return makeError("expected a pointer type while remapping null in "
                                 "managed pointer lowering\n");
            }
            return llvm::ConstantPointerNull::get(pointerType);
        }

        if (auto *undef = llvm::dyn_cast<llvm::UndefValue>(&value)) {
            return llvm::UndefValue::get(expectedType ? expectedType
                                                      : undef->getType());
        }

        if (auto *poison = llvm::dyn_cast<llvm::PoisonValue>(&value)) {
            return llvm::PoisonValue::get(expectedType ? expectedType
                                                       : poison->getType());
        }

        if (auto *zero = llvm::dyn_cast<llvm::ConstantAggregateZero>(&value)) {
            return llvm::ConstantAggregateZero::get(expectedType ? expectedType
                                                                 : zero->getType());
        }

        if (auto *constant = llvm::dyn_cast<llvm::Constant>(&value)) {
            auto *targetType = expectedType ? expectedType : constant->getType();
            if (constant->getType() == targetType) {
                return constant;
            }

            return makeError("managed pointer lowering does not yet support "
                             "retargeting constant `" +
                             value.getName().str() + "` to a different type\n");
        }

        return makeError("managed pointer lowering could not remap value of class `" +
                         std::string(value.getValueID() ==
                                             llvm::Value::InstructionVal
                                         ? "instruction"
                                         : "value") +
                         "`:\n  " + renderValue(value) + "\n");
    }

    llvm::Expected<llvm::Instruction *> cloneGenericInstruction(
        llvm::Instruction &instruction, llvm::BasicBlock &newBlock) {
        auto *clone = instruction.clone();
        clone->setDebugLoc(instruction.getDebugLoc());
        if (instruction.hasName()) {
            clone->setName(instruction.getName());
        }
        clone->insertInto(&newBlock, newBlock.end());

        for (unsigned operandIndex = 0; operandIndex < instruction.getNumOperands();
             ++operandIndex) {
            auto *oldOperand = instruction.getOperand(operandIndex);
            auto mappedOperandOrErr = mapValue(*oldOperand, oldOperand->getType());
            if (!mappedOperandOrErr) {
                return mappedOperandOrErr.takeError();
            }
            if ((*mappedOperandOrErr)->getType() != oldOperand->getType()) {
                return makeError("managed pointer lowering encountered unsupported "
                                 "pointer-sensitive instruction `" +
                                 std::string(instruction.getOpcodeName()) +
                                 "` in function `" +
                                 instruction.getFunction()->getName().str() +
                                 "`\n");
            }
            clone->setOperand(operandIndex, *mappedOperandOrErr);
        }

        return clone;
    }

    llvm::Value *getTrackedSlot(llvm::Value &pointerOperand) const {
        auto *slot = pointerOperand.stripPointerCasts();
        if (auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(slot)) {
            if (alloca->getAllocatedType()->isPointerTy()) {
                return alloca;
            }
            return nullptr;
        }

        if (auto *global = llvm::dyn_cast<llvm::GlobalVariable>(slot)) {
            if (global->getValueType()->isPointerTy()) {
                return global;
            }
        }

        return nullptr;
    }

    llvm::Expected<llvm::Instruction *> lowerAlloca(
        llvm::AllocaInst &alloca, llvm::BasicBlock &newBlock) {
        llvm::Type *allocatedType = alloca.getAllocatedType();
        if (allocatedType->isPointerTy() &&
            isConcreteManagedKind(analysis_.getSlotState(alloca).dispatchKind())) {
            allocatedType = managedPointerType(module_.getContext());
        }

        auto arraySizeOrErr =
            mapValue(*alloca.getArraySize(), alloca.getArraySize()->getType());
        if (!arraySizeOrErr) {
            return arraySizeOrErr.takeError();
        }

        auto *newAlloca = new llvm::AllocaInst(
            allocatedType, alloca.getAddressSpace(), *arraySizeOrErr,
            alloca.getAlign(), alloca.getName(), &newBlock);
        newAlloca->setDebugLoc(alloca.getDebugLoc());
        return newAlloca;
    }

    llvm::Expected<llvm::Instruction *> lowerLoad(llvm::LoadInst &load,
                                                  llvm::BasicBlock &newBlock) {
        auto pointerState = analysis_.getValueState(load).dispatchKind();
        llvm::Type *loadType = load.getType();
        if (loadType->isPointerTy() && isConcreteManagedKind(pointerState)) {
            loadType = managedPointerType(module_.getContext());
        }

        auto pointerOrErr =
            mapValue(*load.getPointerOperand(), load.getPointerOperand()->getType());
        if (!pointerOrErr) {
            return pointerOrErr.takeError();
        }

        auto *newLoad = new llvm::LoadInst(loadType, *pointerOrErr, load.getName(),
                                           load.isVolatile(), load.getAlign(),
                                           &newBlock);
        newLoad->setDebugLoc(load.getDebugLoc());
        return newLoad;
    }

    llvm::Expected<llvm::Instruction *> lowerStore(llvm::StoreInst &store,
                                                   llvm::BasicBlock &newBlock) {
        auto pointerOrErr = mapValue(*store.getPointerOperand(),
                                     store.getPointerOperand()->getType());
        if (!pointerOrErr) {
            return pointerOrErr.takeError();
        }

        llvm::Type *expectedValueType = store.getValueOperand()->getType();
        if (expectedValueType->isPointerTy()) {
            if (auto *slot = getTrackedSlot(*store.getPointerOperand())) {
                auto slotKind = analysis_.getSlotState(*slot).dispatchKind();
                if (isConcreteManagedKind(slotKind)) {
                    expectedValueType = managedPointerType(module_.getContext());
                }
            } else {
                auto locationKind =
                    analysis_.getLocationState(*store.getPointerOperand())
                        .dispatchKind();
                if (isConcreteManagedKind(locationKind)) {
                    expectedValueType = managedPointerType(module_.getContext());
                } else {
                    auto valueKind =
                        analysis_.getValueState(*store.getValueOperand()).dispatchKind();
                    if (isConcreteManagedKind(valueKind)) {
                        expectedValueType = managedPointerType(module_.getContext());
                    }
                }
            }
        }

        auto valueOrErr = mapValue(*store.getValueOperand(), expectedValueType);
        if (!valueOrErr) {
            return valueOrErr.takeError();
        }
        if ((*valueOrErr)->getType() != expectedValueType) {
            return makeError("managed pointer lowering could not rewrite store "
                             "value for function `" +
                             store.getFunction()->getName().str() + "`\n"
                             "  store: " +
                             renderValue(store) + "\n"
                             "  expected type: " +
                             renderType(*expectedValueType) + "\n"
                             "  actual type: " +
                             renderType(*(*valueOrErr)->getType()) + "\n"
                             "  value state: " +
                             renderPointerState(
                                 analysis_.getValueState(*store.getValueOperand())) +
                             "\n"
                             "  location state: " +
                             renderPointerState(
                                 analysis_.getLocationState(*store.getPointerOperand())) +
                             "\n"
                             "  value: " +
                             renderValue(*store.getValueOperand()) + "\n");
        }

        auto *newStore = new llvm::StoreInst(*valueOrErr, *pointerOrErr,
                                             store.isVolatile(), store.getAlign(),
                                             &newBlock);
        newStore->setDebugLoc(store.getDebugLoc());
        return newStore;
    }

    llvm::Expected<llvm::Instruction *> lowerGEP(
        llvm::GetElementPtrInst &gep, llvm::BasicBlock &newBlock) {
        auto pointerOrErr =
            mapValue(*gep.getPointerOperand(), gep.getPointerOperand()->getType());
        if (!pointerOrErr) {
            return pointerOrErr.takeError();
        }

        llvm::SmallVector<llvm::Value *, 8> indices;
        indices.reserve(gep.getNumIndices());
        for (auto &index : gep.indices()) {
            auto mappedIndexOrErr = mapValue(*index.get(), index->getType());
            if (!mappedIndexOrErr) {
                return mappedIndexOrErr.takeError();
            }
            indices.push_back(*mappedIndexOrErr);
        }

        auto *newGep = llvm::GetElementPtrInst::Create(
            gep.getSourceElementType(), *pointerOrErr, indices, gep.getName(),
            &newBlock);
        newGep->setIsInBounds(gep.isInBounds());
        newGep->setDebugLoc(gep.getDebugLoc());
        return newGep;
    }

    llvm::Expected<llvm::Instruction *> lowerPhi(llvm::PHINode &phi,
                                                 llvm::BasicBlock &newBlock) {
        llvm::Type *phiType = phi.getType();
        if (phiType->isPointerTy() &&
            isConcreteManagedKind(analysis_.getValueState(phi).dispatchKind())) {
            phiType = managedPointerType(module_.getContext());
        }

        auto *newPhi = llvm::PHINode::Create(phiType, phi.getNumIncomingValues(),
                                             phi.getName(), &newBlock);
        newPhi->setDebugLoc(phi.getDebugLoc());
        pendingPhis_.push_back({&phi, newPhi});
        return newPhi;
    }

    llvm::Expected<llvm::Instruction *> lowerSelect(
        llvm::SelectInst &select, llvm::BasicBlock &newBlock) {
        auto conditionOrErr =
            mapValue(*select.getCondition(), select.getCondition()->getType());
        if (!conditionOrErr) {
            return conditionOrErr.takeError();
        }

        llvm::Type *resultType = select.getType();
        if (resultType->isPointerTy() &&
            isConcreteManagedKind(analysis_.getValueState(select).dispatchKind())) {
            resultType = managedPointerType(module_.getContext());
        }

        auto trueOrErr = mapValue(*select.getTrueValue(), resultType);
        if (!trueOrErr) {
            return trueOrErr.takeError();
        }
        auto falseOrErr = mapValue(*select.getFalseValue(), resultType);
        if (!falseOrErr) {
            return falseOrErr.takeError();
        }

        auto *newSelect =
            llvm::SelectInst::Create(*conditionOrErr, *trueOrErr, *falseOrErr,
                                     select.getName(), &newBlock);
        newSelect->setDebugLoc(select.getDebugLoc());
        return newSelect;
    }

    llvm::Expected<llvm::Instruction *> lowerFreeze(llvm::FreezeInst &freeze,
                                                    llvm::BasicBlock &newBlock) {
        llvm::Type *resultType = freeze.getType();
        if (resultType->isPointerTy() &&
            isConcreteManagedKind(analysis_.getValueState(freeze).dispatchKind())) {
            resultType = managedPointerType(module_.getContext());
        }

        auto operandOrErr = mapValue(*freeze.getOperand(0), resultType);
        if (!operandOrErr) {
            return operandOrErr.takeError();
        }

        auto *newFreeze =
            new llvm::FreezeInst(*operandOrErr, freeze.getName(), &newBlock);
        newFreeze->setDebugLoc(freeze.getDebugLoc());
        return newFreeze;
    }

    llvm::Expected<llvm::Instruction *> lowerICmp(llvm::ICmpInst &icmp,
                                                  llvm::BasicBlock &newBlock) {
        llvm::Type *lhsType = icmp.getOperand(0)->getType();
        llvm::Type *rhsType = icmp.getOperand(1)->getType();
        if (icmp.getOperand(0)->getType()->isPointerTy() ||
            icmp.getOperand(1)->getType()->isPointerTy()) {
            auto *lhsValue = valueMap_.lookup(icmp.getOperand(0));
            auto *rhsValue = valueMap_.lookup(icmp.getOperand(1));
            if (lhsValue && lhsValue->getType()->isPointerTy()) {
                lhsType = lhsValue->getType();
                rhsType = lhsType;
            } else if (rhsValue && rhsValue->getType()->isPointerTy()) {
                rhsType = rhsValue->getType();
                lhsType = rhsType;
            }
        }

        auto lhsOrErr = mapValue(*icmp.getOperand(0), lhsType);
        if (!lhsOrErr) {
            return lhsOrErr.takeError();
        }
        auto rhsOrErr = mapValue(*icmp.getOperand(1), rhsType);
        if (!rhsOrErr) {
            return rhsOrErr.takeError();
        }

        auto *newICmp =
            llvm::ICmpInst::Create(icmp.getOpcode(), icmp.getPredicate(),
                                   *lhsOrErr, *rhsOrErr, icmp.getName(), &newBlock);
        newICmp->setDebugLoc(icmp.getDebugLoc());
        return newICmp;
    }

    llvm::Expected<llvm::Instruction *> lowerCall(llvm::CallBase &call,
                                                  llvm::BasicBlock &newBlock) {
        if (llvm::isa<llvm::DbgInfoIntrinsic>(call)) {
            return static_cast<llvm::Instruction *>(nullptr);
        }
        if (llvm::isa<llvm::InvokeInst>(call)) {
            return makeError("managed pointer lowering does not yet support invoke "
                             "instructions\n");
        }

        auto *directCallee =
            llvm::dyn_cast<llvm::Function>(call.getCalledOperand()->stripPointerCasts());
        llvm::Function *callee = nullptr;
        if (directCallee) {
            if (auto mappedIt = functionMap_.find(directCallee);
                mappedIt != functionMap_.end()) {
                callee = mappedIt->second;
            } else {
                callee = directCallee;
            }
        } else {
            if (call.getType()->isPointerTy() &&
                isConcreteManagedKind(
                    analysis_.getValueState(call).dispatchKind())) {
                return makeError("managed pointer lowering does not yet support "
                                 "indirect calls with managed pointer results\n");
            }
            for (auto &argUse : call.args()) {
                auto *arg = argUse.get();
                if (arg->getType()->isPointerTy() &&
                    isConcreteManagedKind(
                        analysis_.getValueState(*arg).dispatchKind())) {
                    return makeError("managed pointer lowering does not yet support "
                                     "indirect calls with managed pointer arguments\n");
                }
            }
        }

        llvm::SmallVector<llvm::Value *, 8> args;
        args.reserve(call.arg_size());
        if (callee) {
            auto paramIt = callee->arg_begin();
            for (auto &argUse : call.args()) {
                auto *arg = argUse.get();
                llvm::Type *expectedType = arg->getType();
                if (paramIt != callee->arg_end()) {
                    expectedType = paramIt->getType();
                    ++paramIt;
                }
                auto mappedArgOrErr = mapValue(*arg, expectedType);
                if (!mappedArgOrErr) {
                    return mappedArgOrErr.takeError();
                }
                args.push_back(*mappedArgOrErr);
            }
        } else {
            for (auto &argUse : call.args()) {
                auto *arg = argUse.get();
                auto mappedArgOrErr = mapValue(*arg, arg->getType());
                if (!mappedArgOrErr) {
                    return mappedArgOrErr.takeError();
                }
                args.push_back(*mappedArgOrErr);
            }
        }

        llvm::CallInst *newCall = nullptr;
        if (callee) {
            newCall = llvm::CallInst::Create(callee, args, call.getName(), &newBlock);
        } else {
            auto calleeOrErr =
                mapValue(*call.getCalledOperand(), call.getCalledOperand()->getType());
            if (!calleeOrErr) {
                return calleeOrErr.takeError();
            }
            auto *functionType = call.getFunctionType();
            newCall = llvm::CallInst::Create(functionType, *calleeOrErr, args,
                                             call.getName(), &newBlock);
        }

        newCall->setCallingConv(call.getCallingConv());
        newCall->setAttributes(call.getAttributes());
        if (auto *oldCall = llvm::dyn_cast<llvm::CallInst>(&call)) {
            newCall->setTailCallKind(oldCall->getTailCallKind());
        }
        newCall->setDebugLoc(call.getDebugLoc());
        return newCall;
    }

    llvm::Expected<llvm::Instruction *> lowerReturn(
        llvm::ReturnInst &ret, llvm::BasicBlock &newBlock,
        llvm::Function &newFunction) {
        if (!ret.getReturnValue()) {
            auto *newRet = llvm::ReturnInst::Create(module_.getContext(), &newBlock);
            newRet->setDebugLoc(ret.getDebugLoc());
            return newRet;
        }

        auto returnOrErr =
            mapValue(*ret.getReturnValue(), newFunction.getReturnType());
        if (!returnOrErr) {
            return returnOrErr.takeError();
        }

        auto *newRet =
            llvm::ReturnInst::Create(module_.getContext(), *returnOrErr, &newBlock);
        newRet->setDebugLoc(ret.getDebugLoc());
        return newRet;
    }

    llvm::Expected<llvm::Instruction *> lowerInstruction(
        llvm::Instruction &instruction, llvm::BasicBlock &newBlock,
        llvm::Function &newFunction) {
        if (auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(&instruction)) {
            return lowerAlloca(*alloca, newBlock);
        }
        if (auto *phi = llvm::dyn_cast<llvm::PHINode>(&instruction)) {
            return lowerPhi(*phi, newBlock);
        }
        if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
            return lowerLoad(*load, newBlock);
        }
        if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
            return lowerStore(*store, newBlock);
        }
        if (auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(&instruction)) {
            return lowerGEP(*gep, newBlock);
        }
        if (auto *select = llvm::dyn_cast<llvm::SelectInst>(&instruction)) {
            return lowerSelect(*select, newBlock);
        }
        if (auto *freeze = llvm::dyn_cast<llvm::FreezeInst>(&instruction)) {
            if (freeze->getType()->isPointerTy() ||
                freeze->getOperand(0)->getType()->isPointerTy()) {
                return lowerFreeze(*freeze, newBlock);
            }
        }
        if (auto *icmp = llvm::dyn_cast<llvm::ICmpInst>(&instruction)) {
            if (icmp->getOperand(0)->getType()->isPointerTy() ||
                icmp->getOperand(1)->getType()->isPointerTy()) {
                return lowerICmp(*icmp, newBlock);
            }
        }
        if (auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
            return lowerCall(*call, newBlock);
        }
        if (auto *ret = llvm::dyn_cast<llvm::ReturnInst>(&instruction)) {
            return lowerReturn(*ret, newBlock, newFunction);
        }
        return cloneGenericInstruction(instruction, newBlock);
    }

    llvm::Error rebuildFunction(llvm::Function &oldFunction,
                                llvm::Function &newFunction) {
        valueMap_.clear();
        blockMap_.clear();
        pendingPhis_.clear();

        auto newArgIt = newFunction.arg_begin();
        for (auto &oldArg : oldFunction.args()) {
            auto &newArg = *newArgIt++;
            newArg.setName(oldArg.getName());
            valueMap_[&oldArg] = &newArg;
        }

        for (auto &oldBlock : oldFunction) {
            auto *newBlock = llvm::BasicBlock::Create(module_.getContext(),
                                                      oldBlock.getName(),
                                                      &newFunction);
            blockMap_[&oldBlock] = newBlock;
        }

        llvm::SmallVector<llvm::BasicBlock *, 16> loweringOrder;
        loweringOrder.reserve(oldFunction.size());

        llvm::DominatorTree domTree(oldFunction);
        llvm::SmallPtrSet<llvm::BasicBlock *, 16> scheduled;
        collectDominancePreorder(domTree.getRootNode(), loweringOrder, scheduled);

        for (auto &oldBlock : oldFunction) {
            if (!scheduled.contains(&oldBlock)) {
                loweringOrder.push_back(&oldBlock);
            }
        }

        for (auto *oldBlock : loweringOrder) {
            auto *newBlock = blockMap_[oldBlock];
            for (auto &instruction : *oldBlock) {
                auto loweredOrErr =
                    lowerInstruction(instruction, *newBlock, newFunction);
                if (!loweredOrErr) {
                    return loweredOrErr.takeError();
                }
                if (!*loweredOrErr) {
                    continue;
                }

                if (!instruction.getType()->isVoidTy()) {
                    valueMap_[&instruction] = *loweredOrErr;
                }
            }
        }

        for (auto &[oldPhi, newPhi] : pendingPhis_) {
            for (unsigned index = 0; index < oldPhi->getNumIncomingValues(); ++index) {
                auto *oldIncoming = oldPhi->getIncomingValue(index);
                auto incomingOrErr = mapValue(*oldIncoming, newPhi->getType());
                if (!incomingOrErr) {
                    return incomingOrErr.takeError();
                }
                if ((*incomingOrErr)->getType() != newPhi->getType()) {
                    std::string loadState = "n/a";
                    std::string slotState = "n/a";
                    std::string pointerOperand = "n/a";
                    std::string aggregateBaseState = "n/a";
                    std::string pointerIncomingStates = "n/a";
                    if (auto *load = llvm::dyn_cast<llvm::LoadInst>(oldIncoming)) {
                        loadState =
                            renderPointerState(analysis_.getValueState(*load));
                        pointerOperand = renderValue(*load->getPointerOperand());
                        if (auto *slot = getTrackedSlot(*load->getPointerOperand())) {
                            slotState =
                                renderPointerState(analysis_.getSlotState(*slot));
                        }
                        auto *location =
                            load->getPointerOperand()->stripPointerCasts();
                        if (auto *gep =
                                llvm::dyn_cast<llvm::GetElementPtrInst>(location)) {
                            aggregateBaseState = renderPointerState(
                                analysis_.getValueState(*gep->getPointerOperand()));
                        } else if (auto *phi =
                                       llvm::dyn_cast<llvm::PHINode>(location)) {
                            pointerIncomingStates.clear();
                            for (unsigned incomingIndex = 0;
                                 incomingIndex < phi->getNumIncomingValues();
                                 ++incomingIndex) {
                                if (!pointerIncomingStates.empty()) {
                                    pointerIncomingStates += "\n";
                                }
                                auto *incomingValue =
                                    phi->getIncomingValue(incomingIndex);
                                pointerIncomingStates += "    from ";
                                pointerIncomingStates +=
                                    phi->getIncomingBlock(incomingIndex)->getName().str();
                                pointerIncomingStates += ": ";
                                pointerIncomingStates += renderValue(*incomingValue);
                                pointerIncomingStates += " ; location-state=";
                                pointerIncomingStates += renderPointerState(
                                    analysis_.getLocationState(*incomingValue));
                                pointerIncomingStates += " ; value-state=";
                                pointerIncomingStates += renderPointerState(
                                    analysis_.getValueState(*incomingValue));
                            }
                        }
                    }
                    return makeError(
                        "managed pointer lowering produced a PHI type mismatch in "
                        "function `" +
                        oldFunction.getName().str() + "`\n"
                        "  phi: " +
                        renderValue(*oldPhi) + "\n"
                        "  incoming: " +
                        renderValue(*oldIncoming) + "\n"
                        "  expected type: " +
                        renderType(*newPhi->getType()) + "\n"
                        "  actual type: " +
                        renderType(*(*incomingOrErr)->getType()) + "\n"
                        "  incoming state: " +
                        loadState + "\n"
                        "  incoming slot state: " +
                        slotState + "\n"
                        "  load pointer operand: " +
                        pointerOperand + "\n"
                        "  aggregate base state: " +
                        aggregateBaseState + "\n"
                        "  pointer incoming states:\n" +
                        pointerIncomingStates + "\n");
                }
                newPhi->addIncoming(*incomingOrErr,
                                    blockMap_[oldPhi->getIncomingBlock(index)]);
            }
        }

        return llvm::Error::success();
    }

    llvm::Module &module_;
    ManagedStateAnalysis analysis_;
    llvm::DenseMap<llvm::Function *, llvm::Function *> functionMap_;
    llvm::DenseMap<llvm::BasicBlock *, llvm::BasicBlock *> blockMap_;
    llvm::DenseMap<llvm::Value *, llvm::Value *> valueMap_;
    llvm::SmallVector<std::pair<llvm::PHINode *, llvm::PHINode *>, 8> pendingPhis_;
};

}  // namespace

llvm::Error lowerManagedPointers(llvm::Module &module) {
    ManagedPointerLowerer lowerer(module);
    return lowerer.run();
}

}  // namespace mvm
