#include "mvm/bitcode.hh"

#include "mvm/error.hh"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Support/MemoryBuffer.h"

namespace mvm {

llvm::Expected<LoadedModule> loadBitcodeModule(const std::string &path) {
    auto bufferOrErr = llvm::MemoryBuffer::getFileOrSTDIN(path);
    if (!bufferOrErr) {
        return llvm::errorCodeToError(bufferOrErr.getError());
    }

    auto context = std::make_unique<llvm::LLVMContext>();
    auto moduleOrErr =
        llvm::parseBitcodeFile((*bufferOrErr)->getMemBufferRef(), *context);
    if (!moduleOrErr) {
        return moduleOrErr.takeError();
    }

    LoadedModule loaded;
    loaded.context = std::move(context);
    loaded.module = std::move(*moduleOrErr);
    return loaded;
}

}  // namespace mvm
