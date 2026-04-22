#include "mvm/pipeline.hh"

#include "mvm/bounds_instrumentation.hh"
#include "mvm/error.hh"
#include "mvm/gc.hh"
#include "mvm/managed_state.hh"
#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/IR/SafepointIRVerifier.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/OptimizationLevel.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Transforms/Scalar/PlaceSafepoints.h"
#include "llvm/Transforms/Scalar/RewriteStatepointsForGC.h"

namespace mvm {
namespace {

llvm::Expected<llvm::OptimizationLevel> toOptimizationLevel(int optLevel) {
    switch (optLevel) {
    case 0:
        return llvm::OptimizationLevel::O0;
    case 1:
        return llvm::OptimizationLevel::O1;
    case 2:
        return llvm::OptimizationLevel::O2;
    case 3:
        return llvm::OptimizationLevel::O3;
    default:
        return makeError("unsupported optimization level `" +
                         std::to_string(optLevel) + "`\n");
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

    if (*levelOrErr != llvm::OptimizationLevel::O0) {
        auto passManager = passBuilder.buildPerModuleDefaultPipeline(*levelOrErr);
        passManager.run(module, moduleAnalysisManager);
    }

    if (auto error = annotateManagedState(module)) {
        return error;
    }

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
    return llvm::Error::success();
}

}  // namespace mvm
