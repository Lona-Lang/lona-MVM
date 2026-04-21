#pragma once

#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"
#include <memory>
#include <string>
#include <vector>

namespace mvm {

enum class EntrySignature {
    NoArgsInt32,
    NoArgsVoid,
    ArgvInt32,
    ArgvVoid,
};

struct EntryPoint {
    std::string symbol;
    EntrySignature signature;
};

llvm::Expected<EntryPoint> resolveEntryPoint(const llvm::Module &module,
                                             const std::string &requestedSymbol);

class JitExecutor {
public:
    static llvm::Expected<std::unique_ptr<JitExecutor>> create();

    explicit JitExecutor(std::unique_ptr<llvm::orc::LLJIT> jit);

    const llvm::DataLayout &getDataLayout() const;
    const llvm::Triple &getTargetTriple() const;

    llvm::Error addModule(std::unique_ptr<llvm::Module> module,
                          std::unique_ptr<llvm::LLVMContext> context);
    llvm::Expected<int> invoke(const EntryPoint &entry, llvm::StringRef argv0,
                               llvm::ArrayRef<std::string> args) const;

private:
    std::unique_ptr<llvm::orc::LLJIT> jit_;
};

}  // namespace mvm
