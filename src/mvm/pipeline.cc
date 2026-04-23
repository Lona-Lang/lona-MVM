#include "mvm/pipeline.hh"

#include "mvm/bounds_instrumentation.hh"
#include "mvm/error.hh"
#include "mvm/gc.hh"
#include "mvm/heap_layout.hh"
#include "mvm/managed_dispatch.hh"
#include "mvm/managed_pointer_lowering.hh"
#include "mvm/managed_state.hh"
#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/OptimizationLevel.h"
#include "llvm/Passes/PassBuilder.h"

namespace mvm {
namespace {

llvm::Expected<llvm::OptimizationLevel> toOptimizationLevel(int optLevel) {
    switch (optLevel) {
    case 1:
        return llvm::OptimizationLevel::O1;
    case 2:
        return llvm::OptimizationLevel::O2;
    case 3:
        return llvm::OptimizationLevel::O3;
    default:
        if (optLevel == 0) {
            return makeError("managed mode requires -O1 or higher; -O0 is "
                             "not supported\n");
        }
        return makeError("unsupported optimization level `" +
                         std::to_string(optLevel) + "`, expected 1-3\n");
    }
}

}  // namespace

llvm::Error ModulePipeline::run(llvm::Module &module, int optLevel) const {
    auto levelOrErr = toOptimizationLevel(optLevel);
    if (!levelOrErr) {
        return levelOrErr.takeError();
    }

    if (auto error = injectBoundsChecks(module)) {
        return error;
    }

    llvm::LoopAnalysisManager loopAnalysisManager;
    llvm::FunctionAnalysisManager functionAnalysisManager;
    llvm::CGSCCAnalysisManager cgsccAnalysisManager;
    llvm::ModuleAnalysisManager moduleAnalysisManager;
    llvm::PassBuilder passBuilder;

    passBuilder.registerLoopAnalyses(loopAnalysisManager);
    passBuilder.registerFunctionAnalyses(functionAnalysisManager);
    passBuilder.registerCGSCCAnalyses(cgsccAnalysisManager);
    passBuilder.registerModuleAnalyses(moduleAnalysisManager);
    passBuilder.crossRegisterProxies(loopAnalysisManager, functionAnalysisManager,
                                     cgsccAnalysisManager, moduleAnalysisManager);

    auto passManager = passBuilder.buildPerModuleDefaultPipeline(*levelOrErr);
    passManager.run(module, moduleAnalysisManager);

    if (auto error = specializeManagedDispatch(module)) {
        return error;
    }

    if (auto error = annotateManagedHeapLayouts(module)) {
        return error;
    }

    if (auto error = lowerManagedPointers(module)) {
        return error;
    }

    if (auto error = annotateManagedState(module)) {
        return error;
    }

    if (auto error = runManagedGCPasses(module, moduleAnalysisManager)) {
        return error;
    }
    return llvm::Error::success();
}

}  // namespace mvm
