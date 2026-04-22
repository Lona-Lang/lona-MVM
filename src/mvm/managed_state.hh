#pragma once

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <string>

namespace mvm {

enum class ManagedPointerKind : std::uint8_t {
    Unknown,
    Raw,
    Object,
    Array,
};

struct PointerState {
    std::uint8_t bits = 0;

    bool empty() const;
    bool hasNull() const;
    bool hasRaw() const;
    bool hasManagedObject() const;
    bool hasManagedArray() const;
    bool hasManaged() const;
    bool merge(PointerState other);
    ManagedPointerKind dispatchKind() const;
};

struct FunctionState {
    llvm::SmallVector<PointerState, 4> params;
    PointerState returnState;
};

class ManagedStateAnalysis {
public:
    explicit ManagedStateAnalysis(llvm::Module &module);

    llvm::Error run();
    PointerState getValueState(const llvm::Value &value) const;
    const FunctionState &getFunctionState(const llvm::Function &function) const;
    void attachMetadata();

private:
    bool visitFunction(llvm::Function &function);
    bool visitInstruction(llvm::Instruction &instruction);
    bool visitCall(llvm::CallBase &call);
    PointerState getCallResultState(const llvm::Function &callee) const;
    bool updateValueState(llvm::Value &value, PointerState state);
    bool updateSlotState(llvm::Value &slot, PointerState state);
    bool updateParamState(llvm::Argument &argument, PointerState state);
    bool updateReturnState(llvm::Function &function, PointerState state);
    llvm::Function *getDirectCallee(llvm::CallBase &call) const;
    llvm::Value *getTrackedSlot(llvm::Value &pointerOperand) const;
    std::string renderState(PointerState state) const;
    llvm::MDNode *createStateNode(PointerState state) const;
    void attachFunctionMetadata(llvm::Function &function);

    llvm::Module &module_;
    llvm::DenseMap<const llvm::Value *, PointerState> valueStates_;
    llvm::DenseMap<const llvm::Value *, PointerState> slotStates_;
    llvm::DenseMap<const llvm::Function *, FunctionState> functionStates_;
};

llvm::Error annotateManagedState(llvm::Module &module);

}  // namespace mvm
