#include "mvm/managed_dispatch.hh"

#include "mvm/managed_state.hh"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <string>
#include <utility>

namespace mvm {
namespace {

enum class DispatchArgKind : std::uint8_t {
    Generic,
    Raw,
    Object,
    Array,
};

struct DispatchSignature {
    llvm::SmallVector<DispatchArgKind, 4> args;

    bool hasSpecialization() const {
        for (auto kind : args) {
            if (kind != DispatchArgKind::Generic) {
                return true;
            }
        }
        return false;
    }

    bool operator==(const DispatchSignature &other) const {
        return args == other.args;
    }
};

struct CallRecord {
    llvm::CallBase *call = nullptr;
    DispatchSignature signature;
};

DispatchArgKind toDispatchArgKind(PointerState state) {
    switch (state.dispatchKind()) {
    case ManagedPointerKind::Raw:
        return DispatchArgKind::Raw;
    case ManagedPointerKind::Object:
        return DispatchArgKind::Object;
    case ManagedPointerKind::Array:
        return DispatchArgKind::Array;
    case ManagedPointerKind::Unknown:
        return DispatchArgKind::Generic;
    }

    return DispatchArgKind::Generic;
}

llvm::StringLiteral renderDispatchArgKind(DispatchArgKind kind) {
    switch (kind) {
    case DispatchArgKind::Generic:
        return "generic";
    case DispatchArgKind::Raw:
        return "raw";
    case DispatchArgKind::Object:
        return "object";
    case DispatchArgKind::Array:
        return "array";
    }

    return "generic";
}

bool shouldSpecializeFunction(const llvm::Function &function) {
    return !function.isDeclaration() && !function.hasAddressTaken() &&
           !function.isVarArg();
}

DispatchSignature buildDispatchSignature(llvm::CallBase &call,
                                         const llvm::Function &callee,
                                         const ManagedStateAnalysis &analysis) {
    DispatchSignature signature;
    signature.args.resize(callee.arg_size(), DispatchArgKind::Generic);

    auto argIt = callee.arg_begin();
    for (llvm::Value *arg : call.args()) {
        if (argIt == callee.arg_end()) {
            break;
        }
        if (arg->getType()->isPointerTy()) {
            signature.args[argIt->getArgNo()] =
                toDispatchArgKind(analysis.getValueState(*arg));
        }
        ++argIt;
    }

    return signature;
}

bool containsSignature(llvm::ArrayRef<DispatchSignature> signatures,
                       const DispatchSignature &signature) {
    for (auto &candidate : signatures) {
        if (candidate == signature) {
            return true;
        }
    }
    return false;
}

std::string buildCloneName(const llvm::Function &function,
                           const DispatchSignature &signature) {
    std::string name = function.getName().str();
    name += ".__mvm";

    for (std::size_t index = 0; index < signature.args.size(); ++index) {
        auto kind = signature.args[index];
        if (kind == DispatchArgKind::Generic) {
            continue;
        }

        name += ".arg";
        name += std::to_string(index);
        name += "_";
        name += renderDispatchArgKind(kind).str();
    }

    return name;
}

llvm::Function *findClone(
    const llvm::SmallVectorImpl<std::pair<DispatchSignature, llvm::Function *>> &clones,
    const DispatchSignature &signature) {
    for (auto &[candidateSignature, clone] : clones) {
        if (candidateSignature == signature) {
            return clone;
        }
    }
    return nullptr;
}

llvm::Expected<bool> specializeManagedDispatchRound(llvm::Module &module) {
    ManagedStateAnalysis analysis(module);
    if (auto error = analysis.run()) {
        return std::move(error);
    }

    llvm::DenseMap<llvm::Function *, llvm::SmallVector<CallRecord, 8>> callMap;
    llvm::SmallVector<llvm::Function *, 32> functions;
    functions.reserve(module.size());
    for (auto &function : module) {
        functions.push_back(&function);
    }

    for (auto *function : functions) {
        for (auto &block : *function) {
            for (auto &instruction : block) {
                auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                if (!call) {
                    continue;
                }

                auto *calleeOperand = call->getCalledOperand()->stripPointerCasts();
                auto *callee = llvm::dyn_cast<llvm::Function>(calleeOperand);
                if (!callee || !shouldSpecializeFunction(*callee)) {
                    continue;
                }

                callMap[callee].push_back(
                    {call, buildDispatchSignature(*call, *callee, analysis)});
            }
        }
    }

    bool changed = false;
    for (auto *function : functions) {
        if (!shouldSpecializeFunction(*function)) {
            continue;
        }

        auto callsIt = callMap.find(function);
        if (callsIt == callMap.end()) {
            continue;
        }

        llvm::SmallVector<DispatchSignature, 4> specializedSignatures;
        bool sawGenericCall = false;
        for (auto &record : callsIt->second) {
            if (!record.signature.hasSpecialization()) {
                sawGenericCall = true;
                continue;
            }

            if (!containsSignature(specializedSignatures, record.signature)) {
                specializedSignatures.push_back(record.signature);
            }
        }

        if (specializedSignatures.empty()) {
            continue;
        }
        if (specializedSignatures.size() == 1 && !sawGenericCall) {
            continue;
        }

        llvm::SmallVector<std::pair<DispatchSignature, llvm::Function *>, 4> clones;
        for (auto &signature : specializedSignatures) {
            llvm::ValueToValueMapTy valueMap;
            auto *clone = llvm::CloneFunction(function, valueMap);
            clone->setName(buildCloneName(*function, signature));
            clone->setLinkage(llvm::GlobalValue::InternalLinkage);
            clones.push_back({signature, clone});
        }

        for (auto &record : callsIt->second) {
            if (auto *clone = findClone(clones, record.signature)) {
                record.call->setCalledFunction(clone);
            }
        }

        changed = true;
    }

    return changed;
}

}  // namespace

llvm::Error specializeManagedDispatch(llvm::Module &module) {
    while (true) {
        auto changedOrErr = specializeManagedDispatchRound(module);
        if (!changedOrErr) {
            return changedOrErr.takeError();
        }
        if (!*changedOrErr) {
            break;
        }
    }
    return llvm::Error::success();
}

}  // namespace mvm
