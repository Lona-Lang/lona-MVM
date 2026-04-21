#include "mvm/runtime_memory.hh"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace {

enum class AllocationKind : std::uint64_t {
    Object = 1,
    Array = 2,
};

struct AllocationHeader {
    std::uint64_t magic = 0;
    std::uint64_t kind = 0;
    std::uint64_t alignment = 0;
    std::uint64_t payloadSize = 0;
    std::uint64_t elementSize = 0;
    std::uint64_t elementCount = 0;
    void *allocationBase = nullptr;
};

constexpr std::uint64_t kAllocationMagic = 0x4D564D4152524159ULL;  // MVMARRAY

std::size_t defaultAlignment() { return alignof(std::max_align_t); }

bool isPowerOfTwo(std::size_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

bool toSize(std::uint64_t value, std::size_t &out) {
    if (value > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    out = static_cast<std::size_t>(value);
    return true;
}

bool checkedAdd(std::size_t lhs, std::size_t rhs, std::size_t &out) {
    if (lhs > std::numeric_limits<std::size_t>::max() - rhs) {
        return false;
    }
    out = lhs + rhs;
    return true;
}

bool checkedMul(std::size_t lhs, std::size_t rhs, std::size_t &out) {
    if (lhs != 0 && rhs > std::numeric_limits<std::size_t>::max() / lhs) {
        return false;
    }
    out = lhs * rhs;
    return true;
}

std::uintptr_t alignUp(std::uintptr_t value, std::size_t alignment) {
    return (value + (alignment - 1)) & ~std::uintptr_t(alignment - 1);
}

[[noreturn]] void runtimeAbort(const char *message) {
    std::fputs("mvm runtime error: ", stderr);
    std::fputs(message, stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
    std::_Exit(134);
}

[[noreturn]] void runtimeAbortBounds(std::size_t index, std::size_t width,
                                     std::size_t count) {
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer),
                  "array bounds check failed: index=%zu width=%zu length=%zu",
                  index, width, count);
    runtimeAbort(buffer);
}

void checkedBounds(std::uint64_t start_index, std::uint64_t access_width,
                   std::size_t count, const char *operation) {
    if (access_width == 0) {
        return;
    }

    std::size_t index = 0;
    std::size_t width = 0;
    if (!toSize(start_index, index) || !toSize(access_width, width)) {
        char buffer[160];
        std::snprintf(buffer, sizeof(buffer),
                      "%s received an out-of-range index", operation);
        runtimeAbort(buffer);
    }

    if (index > count || width > count - index) {
        runtimeAbortBounds(index, width, count);
    }
}

AllocationHeader *checkedHeader(const void *payload, const char *operation,
                                bool requireArray) {
    if (!payload) {
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer),
                      "%s received a null managed pointer", operation);
        runtimeAbort(buffer);
    }

    auto *header =
        reinterpret_cast<AllocationHeader *>(const_cast<void *>(payload)) - 1;
    if (header->magic != kAllocationMagic) {
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer),
                      "%s received a pointer that is not managed by mvm",
                      operation);
        runtimeAbort(buffer);
    }

    if (requireArray &&
        header->kind != static_cast<std::uint64_t>(AllocationKind::Array)) {
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer),
                      "%s requires an mvm-managed array allocation", operation);
        runtimeAbort(buffer);
    }

    return header;
}

void *allocateTracked(AllocationKind kind, std::size_t payloadSize,
                      std::size_t elementSize, std::size_t elementCount,
                      std::size_t alignment) {
    if (alignment == 0) {
        alignment = defaultAlignment();
    }

    if (!isPowerOfTwo(alignment)) {
        return nullptr;
    }

    if (alignment < alignof(AllocationHeader)) {
        alignment = alignof(AllocationHeader);
    }

    std::size_t headerAndPadding = 0;
    if (!checkedAdd(sizeof(AllocationHeader), alignment - 1, headerAndPadding)) {
        return nullptr;
    }

    std::size_t totalBytes = 0;
    if (!checkedAdd(payloadSize, headerAndPadding, totalBytes)) {
        return nullptr;
    }

    if (totalBytes == 0) {
        totalBytes = 1;
    }

    void *rawAllocation = std::malloc(totalBytes);
    if (!rawAllocation) {
        return nullptr;
    }

    auto alignedAddress = alignUp(
        reinterpret_cast<std::uintptr_t>(rawAllocation) + sizeof(AllocationHeader),
        alignment);
    auto *header = reinterpret_cast<AllocationHeader *>(alignedAddress) - 1;
    header->magic = kAllocationMagic;
    header->kind = static_cast<std::uint64_t>(kind);
    header->alignment = alignment;
    header->payloadSize = payloadSize;
    header->elementSize = elementSize;
    header->elementCount = elementCount;
    header->allocationBase = rawAllocation;

    return reinterpret_cast<void *>(alignedAddress);
}

}  // namespace

extern "C" void *__mvm_malloc(std::uint64_t size, std::uint64_t alignment) {
    std::size_t payloadSize = 0;
    std::size_t requestedAlignment = 0;
    if (!toSize(size, payloadSize) || !toSize(alignment, requestedAlignment)) {
        return nullptr;
    }

    return allocateTracked(AllocationKind::Object, payloadSize, payloadSize, 1,
                           requestedAlignment);
}

extern "C" void *__mvm_malloc_array(std::uint64_t element_size,
                                    std::uint64_t element_count,
                                    std::uint64_t alignment) {
    std::size_t elementSize = 0;
    std::size_t elementCount = 0;
    std::size_t requestedAlignment = 0;
    if (!toSize(element_size, elementSize) || !toSize(element_count, elementCount) ||
        !toSize(alignment, requestedAlignment)) {
        return nullptr;
    }

    if (elementSize == 0) {
        return nullptr;
    }

    std::size_t payloadSize = 0;
    if (!checkedMul(elementSize, elementCount, payloadSize)) {
        return nullptr;
    }

    return allocateTracked(AllocationKind::Array, payloadSize, elementSize,
                           elementCount, requestedAlignment);
}

extern "C" void __mvm_free(void *payload) {
    if (!payload) {
        return;
    }

    auto *header = checkedHeader(payload, "__mvm_free", false);
    std::free(header->allocationBase);
}

extern "C" std::uint64_t __mvm_allocation_size(const void *payload) {
    auto *header = checkedHeader(payload, "__mvm_allocation_size", false);
    return header->payloadSize;
}

extern "C" std::uint64_t __mvm_array_length(const void *payload) {
    auto *header = checkedHeader(payload, "__mvm_array_length", true);
    return header->elementCount;
}

extern "C" std::uint64_t __mvm_array_element_size(const void *payload) {
    auto *header = checkedHeader(payload, "__mvm_array_element_size", true);
    return header->elementSize;
}

extern "C" void __mvm_bounds_check(const void *payload,
                                   std::uint64_t start_index,
                                   std::uint64_t access_width) {
    auto *header = checkedHeader(payload, "__mvm_bounds_check", true);
    checkedBounds(start_index, access_width,
                  static_cast<std::size_t>(header->elementCount),
                  "__mvm_bounds_check");
}

extern "C" void __mvm_bounds_check_static(std::uint64_t start_index,
                                          std::uint64_t access_width,
                                          std::uint64_t length) {
    std::size_t count = 0;
    if (!toSize(length, count)) {
        runtimeAbort("__mvm_bounds_check_static received an out-of-range length");
    }

    checkedBounds(start_index, access_width, count, "__mvm_bounds_check_static");
}
