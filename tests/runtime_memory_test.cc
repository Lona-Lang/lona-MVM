#include "mvm/runtime_memory.hh"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

void require(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "runtime_memory_test: %s\n", message);
        std::exit(1);
    }
}

bool isAligned(const void *pointer, std::size_t alignment) {
    return reinterpret_cast<std::uintptr_t>(pointer) % alignment == 0;
}

}  // namespace

int main() {
    mvm::GCLayoutDescriptor objectLayout{
        32,
        alignof(std::max_align_t),
        0,
        nullptr,
    };
    void *object = __mvm_malloc_typed(&objectLayout);
    require(object != nullptr, "object allocation returned null");
    require(isAligned(object, alignof(std::max_align_t)),
            "object allocation alignment mismatch");

    mvm::GCLayoutDescriptor scalarArrayLayout{
        sizeof(std::uint32_t),
        alignof(std::uint32_t),
        0,
        nullptr,
    };
    auto *array = static_cast<std::uint32_t *>(
        __mvm_array_malloc_typed(4, &scalarArrayLayout));
    require(array != nullptr, "array allocation returned null");
    require(isAligned(array, alignof(std::uint32_t)),
            "array allocation alignment mismatch");
    require(__mvm_array_length(array) == 4, "array length metadata mismatch");

    array[0] = 7;
    array[3] = 9;
    require(array[0] == 7, "array write/read mismatch at index 0");
    require(array[3] == 9, "array write/read mismatch at index 3");

    return 0;
}
