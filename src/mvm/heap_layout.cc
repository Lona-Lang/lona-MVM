#include "mvm/heap_layout.hh"

#include "mvm/error.hh"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace mvm {
namespace {

constexpr llvm::StringLiteral kManagedObjectAllocName = "__mvm_malloc";
constexpr llvm::StringLiteral kManagedArrayAllocName = "__mvm_array_malloc";
constexpr llvm::StringLiteral kManagedTypedObjectAllocName = "__mvm_malloc_typed";
constexpr llvm::StringLiteral kManagedTypedArrayAllocName =
    "__mvm_array_malloc_typed";
constexpr llvm::StringLiteral kManagedAllocTypeMetadataName = "lona.alloc.type";

std::string renderValue(const llvm::Value &value) {
    std::string text;
    llvm::raw_string_ostream out(text);
    value.print(out);
    out.flush();
    return text;
}

class HeapLayoutRewriter {
public:
    explicit HeapLayoutRewriter(llvm::Module &module) : module_(module) {}

    llvm::Error run() {
        std::vector<llvm::CallBase *> calls;
        for (auto &function : module_) {
            for (auto &block : function) {
                for (auto &instruction : block) {
                    auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                    if (!call) {
                        continue;
                    }

                    auto *callee = llvm::dyn_cast<llvm::Function>(
                        call->getCalledOperand()->stripPointerCasts());
                    if (!callee) {
                        continue;
                    }

                    auto name = callee->getName();
                    if (name == kManagedObjectAllocName ||
                        name == kManagedArrayAllocName) {
                        calls.push_back(call);
                    }
                }
            }
        }

        for (auto *call : calls) {
            if (auto error = rewriteCall(*call)) {
                return error;
            }
        }

        return llvm::Error::success();
    }

private:
    llvm::Error rewriteCall(llvm::CallBase &call) {
        auto *callee = llvm::cast<llvm::Function>(
            call.getCalledOperand()->stripPointerCasts());
        auto layoutType = resolveLayoutType(call);
        if (!layoutType) {
            return layoutType.takeError();
        }

        auto descriptor = getOrCreateDescriptor(**layoutType);
        if (!descriptor) {
            return descriptor.takeError();
        }

        llvm::IRBuilder<> builder(&call);
        llvm::SmallVector<llvm::Value *, 4> args;
        for (llvm::Value *arg : call.args()) {
            args.push_back(arg);
        }
        args.push_back(*descriptor);

        auto &context = module_.getContext();
        auto *i64 = llvm::Type::getInt64Ty(context);
        auto *ptr = llvm::PointerType::get(context, 0);

        llvm::FunctionCallee typedHelper;
        if (callee->getName() == kManagedObjectAllocName) {
            typedHelper = module_.getOrInsertFunction(
                kManagedTypedObjectAllocName,
                llvm::FunctionType::get(call.getType(), {ptr}, false));
        } else {
            typedHelper = module_.getOrInsertFunction(
                kManagedTypedArrayAllocName,
                llvm::FunctionType::get(call.getType(), {i64, ptr}, false));
        }

        auto *newCall = builder.CreateCall(typedHelper, args, call.getName());
        newCall->setCallingConv(call.getCallingConv());
        newCall->copyMetadata(call);
        newCall->setDebugLoc(call.getDebugLoc());
        call.replaceAllUsesWith(newCall);
        call.eraseFromParent();
        return llvm::Error::success();
    }

    llvm::Expected<llvm::Type *> resolveLayoutType(llvm::CallBase &call) const {
        auto *metadata = call.getMetadata(kManagedAllocTypeMetadataName);
        if (!metadata || metadata->getNumOperands() == 0) {
            return makeError("managed allocation call must carry !" +
                             kManagedAllocTypeMetadataName.str() +
                             " metadata:\n  " + renderValue(call) + "\n");
        }

        auto *typeName =
            llvm::dyn_cast<llvm::MDString>(metadata->getOperand(0).get());
        if (!typeName || typeName->getString().empty()) {
            return makeError("managed allocation metadata !" +
                             kManagedAllocTypeMetadataName.str() +
                             " must contain a non-empty type name:\n  " +
                             renderValue(call) + "\n");
        }

        return resolveTypeName(typeName->getString());
    }

    llvm::Expected<llvm::Type *> resolveTypeName(llvm::StringRef typeName) const {
        auto &context = module_.getContext();

        if (typeName.ends_with("[*]") || typeName.ends_with("*")) {
            return llvm::PointerType::get(context, 0);
        }

        if (typeName == "bool") {
            return llvm::Type::getInt1Ty(context);
        }
        if (typeName == "u8" || typeName == "i8") {
            return llvm::Type::getInt8Ty(context);
        }
        if (typeName == "u16" || typeName == "i16") {
            return llvm::Type::getInt16Ty(context);
        }
        if (typeName == "u32" || typeName == "i32") {
            return llvm::Type::getInt32Ty(context);
        }
        if (typeName == "u64" || typeName == "i64") {
            return llvm::Type::getInt64Ty(context);
        }
        if (typeName == "usize") {
            auto pointerBits =
                module_.getDataLayout().getPointerSizeInBits(0);
            return llvm::Type::getIntNTy(context, pointerBits);
        }
        if (typeName == "f32") {
            return llvm::Type::getFloatTy(context);
        }
        if (typeName == "f64") {
            return llvm::Type::getDoubleTy(context);
        }

        if (typeName.ends_with("]")) {
            auto openBracket = typeName.rfind('[');
            if (openBracket != llvm::StringRef::npos &&
                openBracket + 1 < typeName.size() - 1) {
                auto elementTypeName = typeName.take_front(openBracket);
                auto countText = typeName.slice(openBracket + 1, typeName.size() - 1);
                std::uint64_t elementCount = 0;
                if (!elementTypeName.empty() && !countText.empty() &&
                    !countText.consumeInteger(10, elementCount)) {
                    auto elementType = resolveTypeName(elementTypeName);
                    if (!elementType) {
                        return elementType.takeError();
                    }
                    return llvm::ArrayType::get(*elementType, elementCount);
                }
            }
        }

        if (auto *structType = llvm::StructType::getTypeByName(context, typeName)) {
            if (structType->isOpaque()) {
                return makeError("managed allocation metadata resolved to opaque "
                                 "type `" +
                                 typeName.str() + "`\n");
            }
            return structType;
        }

        return makeError("managed allocation metadata references unknown type `" +
                         typeName.str() + "`\n");
    }

    void collectPointerOffsets(llvm::Type &type, std::uint64_t baseOffset,
                               llvm::SmallVectorImpl<std::uint64_t> &offsets) const {
        if (type.isPointerTy()) {
            offsets.push_back(baseOffset);
            return;
        }

        if (auto *arrayType = llvm::dyn_cast<llvm::ArrayType>(&type)) {
            auto elementSize = module_.getDataLayout().getTypeAllocSize(
                arrayType->getElementType());
            if (!elementSize.isScalable()) {
                auto stride = static_cast<std::uint64_t>(elementSize.getFixedValue());
                for (std::uint64_t index = 0; index < arrayType->getNumElements();
                     ++index) {
                    collectPointerOffsets(*arrayType->getElementType(),
                                          baseOffset + index * stride, offsets);
                }
            }
            return;
        }

        auto *structType = llvm::dyn_cast<llvm::StructType>(&type);
        if (!structType || structType->isOpaque()) {
            return;
        }

        auto *layout = module_.getDataLayout().getStructLayout(structType);
        for (unsigned index = 0; index < structType->getNumElements(); ++index) {
            collectPointerOffsets(*structType->getElementType(index),
                                  baseOffset + layout->getElementOffset(index),
                                  offsets);
        }
    }

    llvm::Expected<llvm::Constant *> getOrCreateDescriptor(llvm::Type &type) {
        if (auto found = descriptorMap_.find(&type); found != descriptorMap_.end()) {
            return found->second;
        }

        if (!type.isSized()) {
            return makeError("managed allocation metadata resolved to unsized type\n");
        }

        auto typeSize = module_.getDataLayout().getTypeAllocSize(&type);
        if (typeSize.isScalable()) {
            return makeError(
                "managed allocation metadata resolved to scalable type\n");
        }

        llvm::SmallVector<std::uint64_t, 8> offsets;
        collectPointerOffsets(type, 0, offsets);
        std::sort(offsets.begin(), offsets.end());
        offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());

        auto &context = module_.getContext();
        auto *i64 = llvm::Type::getInt64Ty(context);
        auto *ptr = llvm::PointerType::get(context, 0);

        llvm::Constant *offsetBase = llvm::ConstantPointerNull::get(ptr);
        if (!offsets.empty()) {
            std::vector<llvm::Constant *> offsetConstants;
            offsetConstants.reserve(offsets.size());
            for (auto offset : offsets) {
                offsetConstants.push_back(llvm::ConstantInt::get(i64, offset));
            }

            auto *offsetArrayType = llvm::ArrayType::get(
                i64, static_cast<unsigned>(offsetConstants.size()));
            auto *offsetArray =
                llvm::ConstantArray::get(offsetArrayType, offsetConstants);
            auto *offsetGlobal = new llvm::GlobalVariable(
                module_, offsetArrayType, true, llvm::GlobalValue::PrivateLinkage,
                offsetArray, "__mvm_layout_offsets." + std::to_string(nextId_++));
            offsetGlobal->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
            offsetGlobal->setAlignment(llvm::Align(8));

            auto *zero = llvm::ConstantInt::get(i64, 0);
            llvm::SmallVector<llvm::Constant *, 2> zeroIndices = {zero, zero};
            offsetBase = llvm::ConstantExpr::getPointerCast(
                llvm::ConstantExpr::getInBoundsGetElementPtr(offsetArrayType,
                                                             offsetGlobal,
                                                             zeroIndices),
                ptr);
        }

        auto descriptorAlign = module_.getDataLayout().getABITypeAlign(&type);
        auto *descriptorType = llvm::StructType::get(context, {i64, i64, i64, ptr});
        auto *descriptorValue = llvm::ConstantStruct::get(
            descriptorType,
            {
                llvm::ConstantInt::get(i64, typeSize.getFixedValue()),
                llvm::ConstantInt::get(i64, descriptorAlign.value()),
                llvm::ConstantInt::get(i64, offsets.size()),
                offsetBase,
            });

        auto *descriptorGlobal = new llvm::GlobalVariable(
            module_, descriptorType, true, llvm::GlobalValue::PrivateLinkage,
            descriptorValue, "__mvm_layout." + std::to_string(nextId_++));
        descriptorGlobal->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
        descriptorGlobal->setAlignment(llvm::Align(8));

        auto *descriptor = llvm::ConstantExpr::getPointerCast(descriptorGlobal, ptr);
        descriptorMap_[&type] = descriptor;
        return descriptor;
    }

    llvm::Module &module_;
    llvm::DenseMap<llvm::Type *, llvm::Constant *> descriptorMap_;
    std::uint64_t nextId_ = 0;
};

}  // namespace

llvm::Error annotateManagedHeapLayouts(llvm::Module &module) {
    HeapLayoutRewriter rewriter(module);
    return rewriter.run();
}

}  // namespace mvm
