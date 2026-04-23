#pragma once

#include <cstdint>
#include <cstddef>

#include <vector>

namespace mvm {

struct GCCollectionStats {
    std::uint64_t collectionCount = 0;
    std::size_t rootCount = 0;
    std::size_t liveObjectCount = 0;
    std::size_t sweptObjectCount = 0;
    std::uint64_t sweptBytes = 0;
    std::size_t heapObjectCountBefore = 0;
    std::size_t heapObjectCountAfter = 0;
};

GCCollectionStats collectManagedHeap(const std::vector<std::uintptr_t> &roots);
void clearLastGCCollectionStats();
GCCollectionStats getLastGCCollectionStats();

}  // namespace mvm

extern "C" {

void *__mvm_malloc(std::uint64_t size, std::uint64_t alignment);
void *__mvm_array_malloc(std::uint64_t element_size,
                         std::uint64_t element_count,
                         std::uint64_t alignment);
void __mvm_free(void *payload);
void __mvm_array_free(void *payload);
std::uint64_t __mvm_array_length(const void *payload);

}  // extern "C"
