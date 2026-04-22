#include "mvm/verifier.hh"

#include "mvm/error.hh"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
#include <vector>

namespace mvm {
namespace {

struct Violation {
    std::string functionName;
    std::string reason;
    std::string instructionText;
};

void recordViolation(const llvm::Instruction &instruction,
                     llvm::StringRef reason,
                     std::vector<Violation> &violations) {
    std::string instructionText;
    llvm::raw_string_ostream instructionStream(instructionText);
    instruction.print(instructionStream);
    instructionStream.flush();

    violations.push_back({
        instruction.getFunction() ? instruction.getFunction()->getName().str()
                                  : "<unknown>",
        reason.str(),
        instructionText,
    });
}

bool isRuntimeInjectedHelper(const llvm::Function &function) {
    return function.getName() == "gc.safepoint_poll";
}

}  // namespace

llvm::Error verifyLLVMModule(const llvm::Module &module, llvm::StringRef stage) {
    std::string diagnosticText;
    llvm::raw_string_ostream diagnostics(diagnosticText);
    if (llvm::verifyModule(module, &diagnostics)) {
        diagnostics.flush();
        return makeError("LLVM verifier failed at stage `" + stage.str() +
                         "`:\n" + diagnosticText);
    }
    return llvm::Error::success();
}

llvm::Error ManagedVerifier::verify(const llvm::Module &module,
                                    llvm::StringRef stage) const {
    std::vector<Violation> violations;

    if (module.debug_compile_units().empty()) {
        return makeError("managed verifier failed at stage `" + stage.str() +
                         "`: module must carry debug compile units\n");
    }

    if (!module.getModuleInlineAsm().empty()) {
        return makeError("managed verifier failed at stage `" + stage.str() +
                         "`: module inline assembly is not supported\n");
    }

    for (const auto &function : module) {
        if (!function.isDeclaration() && !function.isIntrinsic() &&
            !function.getSubprogram() && !isRuntimeInjectedHelper(function)) {
            std::string instructionText;
            llvm::raw_string_ostream signature(instructionText);
            function.getFunctionType()->print(signature);
            signature.flush();
            violations.push_back({
                function.getName().str(),
                "defined function is missing debug subprogram metadata",
                instructionText,
            });
        }

        for (const auto &basicBlock : function) {
            for (const auto &instruction : basicBlock) {
                if (llvm::isa<llvm::PtrToIntInst>(instruction)) {
                    recordViolation(
                        instruction,
                        "ptrtoint is currently forbidden to preserve future "
                        "precise-GC guarantees",
                        violations);
                } else if (llvm::isa<llvm::IntToPtrInst>(instruction)) {
                    recordViolation(
                        instruction,
                        "inttoptr is currently forbidden to preserve future "
                        "precise-GC guarantees",
                        violations);
                } else if (llvm::isa<llvm::AddrSpaceCastInst>(instruction)) {
                    recordViolation(
                        instruction,
                        "addrspacecast is currently forbidden in managed code",
                        violations);
                } else if (auto *callBase =
                               llvm::dyn_cast<llvm::CallBase>(&instruction)) {
                    auto *callee =
                        callBase->getCalledOperand()->stripPointerCasts();
                    if (llvm::isa<llvm::InlineAsm>(callee)) {
                        recordViolation(
                            instruction,
                            "inline assembly calls are currently forbidden in "
                            "managed code",
                            violations);
                    } else if (auto *calleeFunction =
                                   llvm::dyn_cast<llvm::Function>(callee)) {
                        auto name = calleeFunction->getName();
                        if (name == "malloc" || name == "calloc" ||
                            name == "realloc" || name == "free") {
                            recordViolation(
                                instruction,
                                "managed code must use the mvm allocation ABI "
                                "(`__mvm_malloc`, `__mvm_array_malloc`, "
                                "`__mvm_free`, `__mvm_array_free`) instead of "
                                "raw libc allocators",
                                violations);
                        }
                    }
                }
            }
        }
    }

    if (violations.empty()) {
        return llvm::Error::success();
    }

    std::string message;
    llvm::raw_string_ostream out(message);
    out << "managed verifier failed at stage `" << stage << "`:\n";
    for (const auto &violation : violations) {
        out << "  - function `" << violation.functionName << "`: "
            << violation.reason << '\n';
        out << "    " << violation.instructionText << '\n';
    }
    out.flush();
    return makeError(message);
}

}  // namespace mvm
