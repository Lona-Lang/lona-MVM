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
    void *object = __mvm_malloc(32, 16);
    require(object != nullptr, "object allocation returned null");
    require(isAligned(object, 16), "object allocation alignment mismatch");
    require(__mvm_allocation_size(object) == 32,
            "object allocation size metadata mismatch");
    __mvm_free(object);

    auto *array = static_cast<std::uint32_t *>(
        __mvm_malloc_array(sizeof(std::uint32_t), 4, alignof(std::uint32_t)));
    require(array != nullptr, "array allocation returned null");
    require(isAligned(array, alignof(std::uint32_t)),
            "array allocation alignment mismatch");
    require(__mvm_array_length(array) == 4, "array length metadata mismatch");
    require(__mvm_array_element_size(array) == sizeof(std::uint32_t),
            "array element size metadata mismatch");
    require(__mvm_allocation_size(array) == sizeof(std::uint32_t) * 4,
            "array allocation size metadata mismatch");

    __mvm_bounds_check(array, 0, 1);
    __mvm_bounds_check(array, 3, 1);

    array[0] = 7;
    array[3] = 9;
    require(array[0] == 7, "array write/read mismatch at index 0");
    require(array[3] == 9, "array write/read mismatch at index 3");

    __mvm_free(array);
    return 0;
}
