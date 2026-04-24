#pragma once

#include <cstdint>
#include <cstddef>

#include <vector>

namespace mvm {

inline constexpr std::uint64_t kGCTypeDescriptorABIVersion = 1;

struct GCTypeDescriptor {
    std::uint64_t abiVersion = kGCTypeDescriptorABIVersion;
    std::uint64_t instanceSize = 0;
    std::uint64_t instanceAlignment = 0;
    std::uint64_t referenceSlotCount = 0;
    const std::uint64_t *referenceSlotOffsets = nullptr;
};

struct GCCollectionStats {
    std::uint64_t collectionCount = 0;
    std::size_t rootCount = 0;
    std::size_t liveObjectCount = 0;
    std::size_t sweptObjectCount = 0;
    std::uint64_t sweptBytes = 0;
    std::uint64_t heapBytesBefore = 0;
    std::uint64_t heapBytesAfter = 0;
    std::size_t heapObjectCountBefore = 0;
    std::size_t heapObjectCountAfter = 0;
};

GCCollectionStats collectManagedHeap(const std::vector<std::uintptr_t> &roots);
void clearLastGCCollectionStats();
GCCollectionStats getLastGCCollectionStats();

}  // namespace mvm

extern "C" {

void *__mvm_malloc();
void *__mvm_malloc_typed(const void *type_descriptor);
void *__mvm_array_malloc(std::uint64_t element_count);
void *__mvm_array_malloc_typed(std::uint64_t element_count,
                               const void *type_descriptor);
std::uint64_t __mvm_array_length(const void *payload);

}  // extern "C"
