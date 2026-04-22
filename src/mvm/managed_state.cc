#include "mvm/managed_state.hh"

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Metadata.h"

#include <string>

namespace mvm {
namespace {

constexpr llvm::StringLiteral kManagedObjectAllocName = "__mvm_malloc";
constexpr llvm::StringLiteral kManagedArrayAllocName = "__mvm_array_malloc";
constexpr llvm::StringLiteral kManagedKindMetadataName = "mvm.managed.kind";
constexpr llvm::StringLiteral kManagedSlotMetadataName = "mvm.managed.slot";
constexpr llvm::StringLiteral kManagedSignatureMetadataName =
    "mvm.managed.signature";

enum PointerStateBits : std::uint8_t {
    kStateUnknown = 0,
    kStateNull = 1u << 0,
    kStateRaw = 1u << 1,
    kStateManagedObject = 1u << 2,
    kStateManagedArray = 1u << 3,
};

}  // namespace

bool PointerState::empty() const { return bits == kStateUnknown; }

bool PointerState::hasNull() const { return (bits & kStateNull) != 0; }

bool PointerState::hasRaw() const { return (bits & kStateRaw) != 0; }

bool PointerState::hasManagedObject() const {
    return (bits & kStateManagedObject) != 0;
}

bool PointerState::hasManagedArray() const {
    return (bits & kStateManagedArray) != 0;
}

bool PointerState::hasManaged() const {
    return (bits & (kStateManagedObject | kStateManagedArray)) != 0;
}

bool PointerState::merge(PointerState other) {
    auto previous = bits;
    bits |= other.bits;
    return previous != bits;
}

ManagedPointerKind PointerState::dispatchKind() const {
    auto concreteBits = bits & (kStateRaw | kStateManagedObject | kStateManagedArray);
    switch (concreteBits) {
    case kStateRaw:
        return ManagedPointerKind::Raw;
    case kStateManagedObject:
        return ManagedPointerKind::Object;
    case kStateManagedArray:
        return ManagedPointerKind::Array;
    default:
        return ManagedPointerKind::Unknown;
    }
}

ManagedStateAnalysis::ManagedStateAnalysis(llvm::Module &module) : module_(module) {
    for (auto &function : module_) {
        functionStates_[&function].params.resize(function.arg_size());
    }
}

llvm::Error ManagedStateAnalysis::run() {
    bool changed = false;
    do {
        changed = false;
        for (auto &function : module_) {
            changed |= visitFunction(function);
        }
    } while (changed);

    return llvm::Error::success();
}

PointerState ManagedStateAnalysis::getValueState(const llvm::Value &value) const {
    if (!value.getType()->isPointerTy()) {
        return {};
    }

    if (llvm::isa<llvm::ConstantPointerNull>(value)) {
        return {kStateNull};
    }

    if (auto *argument = llvm::dyn_cast<llvm::Argument>(&value)) {
        auto functionIt = functionStates_.find(argument->getParent());
        if (functionIt == functionStates_.end() ||
            argument->getArgNo() >= functionIt->second.params.size()) {
            return {};
        }
        return functionIt->second.params[argument->getArgNo()];
    }

    if (llvm::isa<llvm::GlobalValue>(value)) {
        return {kStateRaw};
    }

    auto stateIt = valueStates_.find(&value);
    if (stateIt == valueStates_.end()) {
        return {};
    }
    return stateIt->second;
}

const FunctionState &ManagedStateAnalysis::getFunctionState(
    const llvm::Function &function) const {
    return functionStates_.find(&function)->second;
}

bool ManagedStateAnalysis::visitFunction(llvm::Function &function) {
    bool changed = false;
    for (auto &argument : function.args()) {
        if (!argument.getType()->isPointerTy()) {
            continue;
        }
        if (function.isDeclaration() || function.hasAddressTaken()) {
            changed |= updateParamState(argument, PointerState{kStateRaw});
        }
    }

    for (auto &block : function) {
        for (auto &instruction : block) {
            changed |= visitInstruction(instruction);
        }
    }
    return changed;
}

bool ManagedStateAnalysis::visitInstruction(llvm::Instruction &instruction) {
    if (auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(&instruction)) {
        if (alloca->getType()->isPointerTy()) {
            return updateValueState(*alloca, PointerState{kStateRaw});
        }
        return false;
    }

    if (auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(&instruction)) {
        return updateValueState(*gep, getValueState(*gep->getPointerOperand()));
    }

    if (auto *phi = llvm::dyn_cast<llvm::PHINode>(&instruction)) {
        PointerState state;
        for (llvm::Value *incoming : phi->incoming_values()) {
            state.merge(getValueState(*incoming));
        }
        return updateValueState(*phi, state);
    }

    if (auto *select = llvm::dyn_cast<llvm::SelectInst>(&instruction)) {
        PointerState state = getValueState(*select->getTrueValue());
        state.merge(getValueState(*select->getFalseValue()));
        return updateValueState(*select, state);
    }

    if (auto *freeze = llvm::dyn_cast<llvm::FreezeInst>(&instruction)) {
        return updateValueState(*freeze, getValueState(*freeze->getOperand(0)));
    }

    if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
        if (!load->getType()->isPointerTy()) {
            return false;
        }

        if (auto *slot = getTrackedSlot(*load->getPointerOperand())) {
            return updateValueState(*load, slotStates_[slot]);
        }
        return false;
    }

    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
        if (!store->getValueOperand()->getType()->isPointerTy()) {
            return false;
        }

        if (auto *slot = getTrackedSlot(*store->getPointerOperand())) {
            return updateSlotState(*slot, getValueState(*store->getValueOperand()));
        }
        return false;
    }

    if (auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
        return visitCall(*call);
    }

    if (auto *ret = llvm::dyn_cast<llvm::ReturnInst>(&instruction)) {
        if (auto *value = ret->getReturnValue();
            value && value->getType()->isPointerTy()) {
            return updateReturnState(*ret->getFunction(), getValueState(*value));
        }
        return false;
    }

    return false;
}

bool ManagedStateAnalysis::visitCall(llvm::CallBase &call) {
    bool changed = false;
    auto *callee = getDirectCallee(call);

    if (callee) {
        if (call.getType()->isPointerTy()) {
            changed |= updateValueState(call, getCallResultState(*callee));
        }

        if (!callee->isDeclaration()) {
            auto argIt = callee->arg_begin();
            for (llvm::Value *arg : call.args()) {
                if (argIt == callee->arg_end()) {
                    break;
                }
                if (arg->getType()->isPointerTy()) {
                    changed |= updateParamState(*argIt, getValueState(*arg));
                }
                ++argIt;
            }
        }
        return changed;
    }

    if (call.getType()->isPointerTy()) {
        changed |= updateValueState(call, PointerState{kStateRaw});
    }
    return changed;
}

PointerState ManagedStateAnalysis::getCallResultState(
    const llvm::Function &callee) const {
    if (!callee.getReturnType()->isPointerTy()) {
        return {};
    }

    if (callee.getName() == kManagedObjectAllocName) {
        return {kStateManagedObject};
    }
    if (callee.getName() == kManagedArrayAllocName) {
        return {kStateManagedArray};
    }

    if (callee.isDeclaration()) {
        return {kStateRaw};
    }

    return getFunctionState(callee).returnState;
}

bool ManagedStateAnalysis::updateValueState(llvm::Value &value, PointerState state) {
    if (!value.getType()->isPointerTy() || state.empty()) {
        return false;
    }
    return valueStates_[&value].merge(state);
}

bool ManagedStateAnalysis::updateSlotState(llvm::Value &slot, PointerState state) {
    if (state.empty()) {
        return false;
    }
    return slotStates_[&slot].merge(state);
}

bool ManagedStateAnalysis::updateParamState(llvm::Argument &argument,
                                            PointerState state) {
    if (state.empty()) {
        return false;
    }
    return functionStates_[argument.getParent()]
        .params[argument.getArgNo()]
        .merge(state);
}

bool ManagedStateAnalysis::updateReturnState(llvm::Function &function,
                                             PointerState state) {
    if (state.empty()) {
        return false;
    }
    return functionStates_[&function].returnState.merge(state);
}

llvm::Function *ManagedStateAnalysis::getDirectCallee(llvm::CallBase &call) const {
    auto *callee = call.getCalledOperand()->stripPointerCasts();
    return llvm::dyn_cast<llvm::Function>(callee);
}

llvm::Value *ManagedStateAnalysis::getTrackedSlot(
    llvm::Value &pointerOperand) const {
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

std::string ManagedStateAnalysis::renderState(PointerState state) const {
    if (state.empty()) {
        return "unknown";
    }

    llvm::SmallVector<llvm::StringRef, 4> parts;
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
    for (auto it = parts.begin(); it != parts.end(); ++it) {
        if (it != parts.begin()) {
            text += '|';
        }
        text += it->str();
    }
    return text;
}

llvm::MDNode *ManagedStateAnalysis::createStateNode(PointerState state) const {
    auto &context = module_.getContext();
    return llvm::MDNode::get(context,
                             llvm::MDString::get(context, renderState(state)));
}

void ManagedStateAnalysis::attachMetadata() {
    for (auto &function : module_) {
        attachFunctionMetadata(function);

        for (auto &block : function) {
            for (auto &instruction : block) {
                instruction.setMetadata(kManagedKindMetadataName, nullptr);
                instruction.setMetadata(kManagedSlotMetadataName, nullptr);

                if (!instruction.getType()->isPointerTy()) {
                    continue;
                }

                auto state = getValueState(instruction);
                if (!state.empty()) {
                    instruction.setMetadata(kManagedKindMetadataName,
                                            createStateNode(state));
                }

                if (auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(&instruction)) {
                    auto slotIt = slotStates_.find(alloca);
                    if (slotIt != slotStates_.end() && !slotIt->second.empty()) {
                        instruction.setMetadata(kManagedSlotMetadataName,
                                                createStateNode(slotIt->second));
                    }
                }
            }
        }
    }
}

void ManagedStateAnalysis::attachFunctionMetadata(llvm::Function &function) {
    function.setMetadata(kManagedSignatureMetadataName, nullptr);

    llvm::SmallVector<llvm::Metadata *, 8> operands;
    auto functionIt = functionStates_.find(&function);
    if (functionIt == functionStates_.end()) {
        return;
    }

    auto &context = module_.getContext();
    if (function.getReturnType()->isPointerTy()) {
        auto text = "ret=" + renderState(functionIt->second.returnState);
        operands.push_back(llvm::MDString::get(context, text));
    }

    for (auto &argument : function.args()) {
        if (!argument.getType()->isPointerTy()) {
            continue;
        }

        auto text = "arg" + std::to_string(argument.getArgNo()) + "=" +
                    renderState(functionIt->second.params[argument.getArgNo()]);
        operands.push_back(llvm::MDString::get(context, text));
    }

    if (operands.empty()) {
        return;
    }

    function.setMetadata(kManagedSignatureMetadataName,
                         llvm::MDNode::get(context, operands));
}

llvm::Error annotateManagedState(llvm::Module &module) {
    ManagedStateAnalysis analysis(module);
    if (auto error = analysis.run()) {
        return error;
    }

    analysis.attachMetadata();
    return llvm::Error::success();
}

}  // namespace mvm
