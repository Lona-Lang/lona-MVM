#include "mvm/bounds_instrumentation.hh"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"

#include <cstdint>

namespace mvm {
namespace {

constexpr llvm::StringLiteral kDynamicBoundsCheckName = "__mvm_bounds_check";
constexpr llvm::StringLiteral kStaticBoundsCheckName =
    "__mvm_bounds_check_static";
constexpr llvm::StringLiteral kManagedArrayAllocName = "__mvm_malloc_array";

struct StaticCheckPlan {
    llvm::Value *index = nullptr;
    std::uint64_t length = 0;
};

llvm::FunctionCallee getDynamicBoundsCheck(llvm::Module &module) {
    auto &context = module.getContext();
    auto *voidTy = llvm::Type::getVoidTy(context);
    auto *ptrTy = llvm::PointerType::get(context, 0);
    auto *i64Ty = llvm::Type::getInt64Ty(context);
    return module.getOrInsertFunction(kDynamicBoundsCheckName, voidTy, ptrTy, i64Ty,
                                      i64Ty);
}

llvm::FunctionCallee getStaticBoundsCheck(llvm::Module &module) {
    auto &context = module.getContext();
    auto *voidTy = llvm::Type::getVoidTy(context);
    auto *i64Ty = llvm::Type::getInt64Ty(context);
    return module.getOrInsertFunction(kStaticBoundsCheckName, voidTy, i64Ty, i64Ty,
                                      i64Ty);
}

llvm::Value *normalizeIndex(llvm::IRBuilder<> &builder, llvm::Value *index) {
    auto *i64Ty = builder.getInt64Ty();
    auto *integerTy = llvm::dyn_cast<llvm::IntegerType>(index->getType());
    if (!integerTy) {
        return nullptr;
    }

    if (integerTy == i64Ty) {
        return index;
    }

    return builder.CreateIntCast(index, i64Ty, false,
                                 index->getName() + ".mvm.bounds.idx");
}

bool isIgnorableAllocaUser(const llvm::User &user) {
    if (auto *intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(&user)) {
        switch (intrinsic->getIntrinsicID()) {
        case llvm::Intrinsic::dbg_assign:
        case llvm::Intrinsic::dbg_declare:
        case llvm::Intrinsic::dbg_label:
        case llvm::Intrinsic::dbg_value:
        case llvm::Intrinsic::lifetime_start:
        case llvm::Intrinsic::lifetime_end:
            return true;
        default:
            return false;
        }
    }
    return false;
}

bool isManagedArrayBaseImpl(llvm::Value &value, llvm::DominatorTree &domTree,
                            llvm::DenseSet<llvm::Value *> &visiting);

bool isManagedArrayBaseStoredInAlloca(llvm::LoadInst &load,
                                      llvm::DominatorTree &domTree,
                                      llvm::DenseSet<llvm::Value *> &visiting) {
    auto *slot = llvm::dyn_cast<llvm::AllocaInst>(
        load.getPointerOperand()->stripPointerCasts());
    if (!slot) {
        return false;
    }

    llvm::Value *storedValue = nullptr;
    for (llvm::User *user : slot->users()) {
        if (user == &load || isIgnorableAllocaUser(*user)) {
            continue;
        }

        if (auto *otherLoad = llvm::dyn_cast<llvm::LoadInst>(user)) {
            (void)otherLoad;
            continue;
        }

        auto *store = llvm::dyn_cast<llvm::StoreInst>(user);
        if (!store ||
            store->getPointerOperand()->stripPointerCasts() != slot ||
            !domTree.dominates(store, &load)) {
            return false;
        }

        if (!storedValue) {
            storedValue = store->getValueOperand();
            continue;
        }

        if (storedValue != store->getValueOperand()) {
            return false;
        }
    }

    if (!storedValue) {
        return false;
    }

    return isManagedArrayBaseImpl(*storedValue, domTree, visiting);
}

bool isManagedArrayBaseImpl(llvm::Value &value, llvm::DominatorTree &domTree,
                            llvm::DenseSet<llvm::Value *> &visiting) {
    auto *stripped = value.stripPointerCasts();
    if (!stripped->getType()->isPointerTy()) {
        return false;
    }

    if (!visiting.insert(stripped).second) {
        return false;
    }

    if (auto *call = llvm::dyn_cast<llvm::CallBase>(stripped)) {
        if (auto *callee = call->getCalledFunction()) {
            return callee->getName() == kManagedArrayAllocName;
        }
        return false;
    }

    if (auto *load = llvm::dyn_cast<llvm::LoadInst>(stripped)) {
        return isManagedArrayBaseStoredInAlloca(*load, domTree, visiting);
    }

    if (auto *phi = llvm::dyn_cast<llvm::PHINode>(stripped)) {
        if (phi->getNumIncomingValues() == 0) {
            return false;
        }
        for (llvm::Value *incoming : phi->incoming_values()) {
            if (!isManagedArrayBaseImpl(*incoming, domTree, visiting)) {
                return false;
            }
        }
        return true;
    }

    if (auto *select = llvm::dyn_cast<llvm::SelectInst>(stripped)) {
        return isManagedArrayBaseImpl(*select->getTrueValue(), domTree, visiting) &&
               isManagedArrayBaseImpl(*select->getFalseValue(), domTree, visiting);
    }

    return false;
}

bool isManagedArrayBase(llvm::Value &value, llvm::DominatorTree &domTree) {
    llvm::DenseSet<llvm::Value *> visiting;
    return isManagedArrayBaseImpl(value, domTree, visiting);
}

bool isStaticallyInBounds(const llvm::Value &index, std::uint64_t length) {
    auto *constantIndex = llvm::dyn_cast<llvm::ConstantInt>(&index);
    if (!constantIndex) {
        return false;
    }

    return constantIndex->getValue().ult(length);
}

llvm::SmallVector<StaticCheckPlan, 4>
collectStaticChecks(const llvm::GetElementPtrInst &instruction) {
    llvm::SmallVector<StaticCheckPlan, 4> checks;

    auto indexIt = instruction.idx_begin();
    if (indexIt == instruction.idx_end()) {
        return checks;
    }

    ++indexIt;  // Skip the top-level pointer step. Remaining indices walk the
                // actual source aggregate.

    llvm::Type *currentType = instruction.getSourceElementType();
    for (; indexIt != instruction.idx_end() && currentType; ++indexIt) {
        llvm::Value *index = *indexIt;

        if (auto *structType = llvm::dyn_cast<llvm::StructType>(currentType)) {
            auto *fieldIndex = llvm::dyn_cast<llvm::ConstantInt>(index);
            if (!fieldIndex) {
                break;
            }

            auto field = fieldIndex->getZExtValue();
            if (field >= structType->getNumElements()) {
                break;
            }

            currentType = structType->getElementType(field);
            continue;
        }

        if (auto *arrayType = llvm::dyn_cast<llvm::ArrayType>(currentType)) {
            checks.push_back({index, arrayType->getNumElements()});
            currentType = arrayType->getElementType();
            continue;
        }

        if (auto *vectorType = llvm::dyn_cast<llvm::VectorType>(currentType)) {
            currentType = vectorType->getElementType();
            continue;
        }

        break;
    }

    return checks;
}

bool instrumentGEP(llvm::GetElementPtrInst &instruction, llvm::DominatorTree &domTree,
                   llvm::FunctionCallee dynamicBoundsCheck,
                   llvm::FunctionCallee staticBoundsCheck) {
    bool needsDynamicCheck = false;
    if (instruction.getNumIndices() != 0) {
        needsDynamicCheck =
            isManagedArrayBase(*instruction.getPointerOperand(), domTree);
    }

    auto staticChecks = collectStaticChecks(instruction);
    bool hasStaticCheck = false;
    for (const auto &check : staticChecks) {
        if (!isStaticallyInBounds(*check.index, check.length)) {
            hasStaticCheck = true;
            break;
        }
    }

    if (!needsDynamicCheck && !hasStaticCheck) {
        return false;
    }

    llvm::IRBuilder<> builder(&instruction);
    builder.SetCurrentDebugLocation(instruction.getDebugLoc());

    if (needsDynamicCheck) {
        auto *dynamicIndex =
            normalizeIndex(builder, instruction.getOperand(1));
        if (dynamicIndex) {
            builder.CreateCall(dynamicBoundsCheck,
                               {instruction.getPointerOperand(), dynamicIndex,
                                builder.getInt64(1)});
        }
    }

    for (const auto &check : staticChecks) {
        if (isStaticallyInBounds(*check.index, check.length)) {
            continue;
        }

        auto *index = normalizeIndex(builder, check.index);
        if (!index) {
            continue;
        }

        builder.CreateCall(staticBoundsCheck,
                           {index, builder.getInt64(1),
                            builder.getInt64(check.length)});
    }

    return true;
}

}  // namespace

llvm::Error injectBoundsChecks(llvm::Module &module) {
    auto dynamicBoundsCheck = getDynamicBoundsCheck(module);
    auto staticBoundsCheck = getStaticBoundsCheck(module);

    bool changed = false;
    for (auto &function : module) {
        if (function.isDeclaration()) {
            continue;
        }

        llvm::DominatorTree domTree(function);
        llvm::SmallVector<llvm::GetElementPtrInst *, 32> geps;
        for (auto &block : function) {
            for (auto &instruction : block) {
                if (auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(&instruction)) {
                    geps.push_back(gep);
                }
            }
        }

        for (auto *gep : geps) {
            changed |=
                instrumentGEP(*gep, domTree, dynamicBoundsCheck, staticBoundsCheck);
        }
    }

    (void)changed;
    return llvm::Error::success();
}

}  // namespace mvm
