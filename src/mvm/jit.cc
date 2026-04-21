#include "mvm/jit.hh"

#include "mvm/error.hh"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/Support/TargetSelect.h"

namespace mvm {
namespace {

extern "C" {
// Linked bitcode no longer carries the hosted `main(argc, argv)` wrapper, so
// MVM exports the same argv globals and populates them before invoking
// `__lona_main__`.
int __lona_argc = 0;
char **__lona_argv = nullptr;
}

llvm::Expected<EntryPoint> classifyEntryPoint(const llvm::Function &function) {
    auto *functionType = function.getFunctionType();
    if (functionType->isVarArg()) {
        return makeError("entry function `" + function.getName().str() +
                         "` must not be variadic\n");
    }

    auto *returnType = functionType->getReturnType();
    bool returnsInt32 = returnType->isIntegerTy(32);
    bool returnsVoid = returnType->isVoidTy();
    if (!returnsInt32 && !returnsVoid) {
        return makeError("entry function `" + function.getName().str() +
                         "` must return `i32` or `void`\n");
    }

    if (functionType->getNumParams() == 0) {
        return EntryPoint{
            function.getName().str(),
            returnsInt32 ? EntrySignature::NoArgsInt32 : EntrySignature::NoArgsVoid,
        };
    }

    if (functionType->getNumParams() == 2 &&
        functionType->getParamType(0)->isIntegerTy(32) &&
        functionType->getParamType(1)->isPointerTy()) {
        return EntryPoint{
            function.getName().str(),
            returnsInt32 ? EntrySignature::ArgvInt32 : EntrySignature::ArgvVoid,
        };
    }

    return makeError("entry function `" + function.getName().str() +
                     "` must have signature `i32 ()`, `void ()`, "
                     "`i32 (i32, ptr)` or `void (i32, ptr)`\n");
}

}  // namespace

llvm::Expected<EntryPoint> resolveEntryPoint(const llvm::Module &module,
                                             const std::string &requestedSymbol) {
    if (!requestedSymbol.empty()) {
        auto *function = module.getFunction(requestedSymbol);
        if (!function) {
            return makeError("entry symbol `" + requestedSymbol +
                             "` was not found in the module\n");
        }
        return classifyEntryPoint(*function);
    }

    for (llvm::StringRef candidate : {"__mvm_main__", "__lona_main__", "main"}) {
        if (auto *function = module.getFunction(candidate)) {
            return classifyEntryPoint(*function);
        }
    }

    return makeError("no default entry symbol found, tried `__mvm_main__`, "
                     "`__lona_main__`, and `main`\n");
}

llvm::Expected<std::unique_ptr<JitExecutor>> JitExecutor::create() {
    if (llvm::InitializeNativeTarget()) {
        return makeError("failed to initialize the native target\n");
    }
    if (llvm::InitializeNativeTargetAsmPrinter()) {
        return makeError("failed to initialize the native asm printer\n");
    }
    if (llvm::InitializeNativeTargetAsmParser()) {
        return makeError("failed to initialize the native asm parser\n");
    }

    auto jitOrErr = llvm::orc::LLJITBuilder().create();
    if (!jitOrErr) {
        return jitOrErr.takeError();
    }

    auto executor = std::unique_ptr<JitExecutor>(
        new JitExecutor(std::move(*jitOrErr)));

    auto generatorOrErr =
        llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
            executor->getDataLayout().getGlobalPrefix());
    if (!generatorOrErr) {
        return generatorOrErr.takeError();
    }
    executor->jit_->getMainJITDylib().addGenerator(std::move(*generatorOrErr));
    return executor;
}

JitExecutor::JitExecutor(std::unique_ptr<llvm::orc::LLJIT> jit)
    : jit_(std::move(jit)) {}

const llvm::DataLayout &JitExecutor::getDataLayout() const {
    return jit_->getDataLayout();
}

const llvm::Triple &JitExecutor::getTargetTriple() const {
    return jit_->getTargetTriple();
}

llvm::Error JitExecutor::addModule(std::unique_ptr<llvm::Module> module,
                                   std::unique_ptr<llvm::LLVMContext> context) {
    return jit_->addIRModule(
        llvm::orc::ThreadSafeModule(std::move(module), std::move(context)));
}

llvm::Expected<int> JitExecutor::invoke(const EntryPoint &entry,
                                        llvm::StringRef argv0,
                                        llvm::ArrayRef<std::string> args) const {
    auto addressOrErr = jit_->lookup(entry.symbol);
    if (!addressOrErr) {
        return addressOrErr.takeError();
    }

    std::vector<std::string> ownedArgs;
    ownedArgs.reserve(args.size() + 1);
    ownedArgs.push_back(argv0.str());
    for (const auto &arg : args) {
        ownedArgs.push_back(arg);
    }

    std::vector<char *> argv;
    argv.reserve(ownedArgs.size() + 1);
    for (auto &arg : ownedArgs) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    __lona_argc = static_cast<int>(ownedArgs.size());
    __lona_argv = argv.data();

    switch (entry.signature) {
    case EntrySignature::NoArgsInt32: {
        using EntryFn = int (*)();
        return (*addressOrErr).toPtr<EntryFn>()();
    }
    case EntrySignature::NoArgsVoid: {
        using EntryFn = void (*)();
        (*addressOrErr).toPtr<EntryFn>()();
        return 0;
    }
    case EntrySignature::ArgvInt32: {
        using EntryFn = int (*)(int, char **);
        return (*addressOrErr).toPtr<EntryFn>()(
            static_cast<int>(ownedArgs.size()), argv.data());
    }
    case EntrySignature::ArgvVoid: {
        using EntryFn = void (*)(int, char **);
        (*addressOrErr).toPtr<EntryFn>()(
            static_cast<int>(ownedArgs.size()), argv.data());
        return 0;
    }
    }

    return makeError("unreachable entry signature state\n");
}

}  // namespace mvm
