#include "mvm/runtime.hh"

#include "mvm/bitcode.hh"
#include "mvm/error.hh"
#include "mvm/gc.hh"
#include "mvm/jit.hh"
#include "mvm/pipeline.hh"
#include "mvm/runtime_threads.hh"
#include "mvm/verifier.hh"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Support/raw_ostream.h"
#include <future>
#include <thread>

namespace mvm {
namespace {

llvm::Error prepareModuleForExecution(llvm::Module &module,
                                      const llvm::Triple &targetTriple,
                                      const llvm::DataLayout &dataLayout) {
    if (!module.getTargetTriple().empty()) {
        llvm::Triple inputTriple(module.getTargetTriple());
        if (inputTriple.getArch() != targetTriple.getArch()) {
            return makeError("bitcode target `" + module.getTargetTriple() +
                             "` does not match host architecture `" +
                             targetTriple.str() + "`\n");
        }
    }

    module.setTargetTriple(targetTriple.str());
    module.setDataLayout(dataLayout);
    return llvm::Error::success();
}

}  // namespace

llvm::Expected<int> runManagedProgram(const Options &options) {
    auto executorOrErr = JitExecutor::create();
    if (!executorOrErr) {
        return executorOrErr.takeError();
    }
    auto executor = std::move(*executorOrErr);

    auto loadedOrErr = loadBitcodeModule(options.inputPath);
    if (!loadedOrErr) {
        return loadedOrErr.takeError();
    }
    auto loaded = std::move(*loadedOrErr);

    if (auto error = prepareModuleForExecution(
            *loaded.module, executor->getTargetTriple(), executor->getDataLayout())) {
        return std::move(error);
    }

    if (auto error = verifyLLVMModule(*loaded.module, "load")) {
        return std::move(error);
    }

    ManagedVerifier verifier;
    if (auto error = verifier.verify(*loaded.module, "load")) {
        return std::move(error);
    }

    ModulePipeline pipeline;
    if (auto error = pipeline.run(*loaded.module, options.optLevel)) {
        return std::move(error);
    }

    if (auto error = verifyLLVMModule(*loaded.module, "post-pass")) {
        return std::move(error);
    }
    if (auto error = verifier.verify(*loaded.module, "post-pass")) {
        return std::move(error);
    }

    auto entryOrErr = resolveEntryPoint(*loaded.module, options.entrySymbol);
    if (!entryOrErr) {
        return entryOrErr.takeError();
    }

    if (options.dumpIR) {
        loaded.module->print(llvm::outs(), nullptr);
    }

    if (auto error = executor->addModule(std::move(loaded.module),
                                         std::move(loaded.context))) {
        return std::move(error);
    }

    RuntimeThreads runtimeThreads;
    installRuntimeThreads(runtimeThreads);

    std::promise<llvm::Expected<int>> resultPromise;
    auto resultFuture = resultPromise.get_future();

    std::thread mutatorThread(
        [&runtimeThreads, &resultPromise, &executor, entry = *entryOrErr,
         argv0 = options.inputPath, args = options.programArgs]() mutable {
            llvm::EnablePrettyStackTraceOnSigInfoForThisThread(true);
            llvm::PrettyStackTraceFormat stackFrame(
                "while executing managed entry `%s` on the MVM mutator thread",
                entry.symbol.c_str());

            runtimeThreads.markMutatorStarted();
            installGCStackMapRegistry(executor->getStackMaps());
            registerMutatorThread();
            clearGCRequest();
            resetGCAllocationBudget();
            clearLastRootScanSummary();
            auto exitCodeOrErr = executor->invoke(entry, argv0, args);
            unregisterMutatorThread();
            clearGCStackMapRegistry();
            runtimeThreads.markMutatorStopped();
            resultPromise.set_value(std::move(exitCodeOrErr));
        });

    mutatorThread.join();
    clearRuntimeThreads();
    return resultFuture.get();
}

}  // namespace mvm
