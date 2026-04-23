#include "mvm/runtime_memory.hh"

#include "mvm/gc.hh"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

namespace {

enum class AllocationKind : std::uint64_t {
    Object = 1,
    Array = 2,
};

struct AllocationHeader {
    std::uint64_t magic = 0;
    std::uint64_t kind = 0;
    std::uint64_t alignment = 0;
    std::uint64_t elementCount = 0;
    std::uint64_t payloadSize = 0;
    void *allocationBase = nullptr;
};

struct AllocationRecord {
    AllocationKind kind = AllocationKind::Object;
    std::uintptr_t payloadAddress = 0;
    std::size_t payloadSize = 0;
    std::size_t elementCount = 0;
    void *allocationBase = nullptr;
    bool marked = false;
};

constexpr std::uint64_t kAllocationMagic = 0x4D564D4152524159ULL;  // MVMARRAY

std::mutex allocationRegistryMutex;
std::map<std::uintptr_t, AllocationRecord> allocationRegistry;
std::uint64_t collectionCount = 0;
mvm::GCCollectionStats lastCollectionStats;

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

AllocationHeader *checkedHeader(const void *payload, const char *operation,
                                std::optional<AllocationKind> expectedKind) {
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

    if (expectedKind &&
        header->kind != static_cast<std::uint64_t>(*expectedKind)) {
        char buffer[128];
        auto expectedName = *expectedKind == AllocationKind::Array
                                ? "an mvm-managed array allocation"
                                : "an mvm-managed object allocation";
        std::snprintf(buffer, sizeof(buffer), "%s requires %s", operation,
                      expectedName);
        runtimeAbort(buffer);
    }

    return header;
}

void *allocateTracked(AllocationKind kind, std::size_t payloadSize,
                      std::size_t elementCount,
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
    header->elementCount = elementCount;
    header->payloadSize = payloadSize;
    header->allocationBase = rawAllocation;

    return reinterpret_cast<void *>(alignedAddress);
}

void registerAllocation(void *payload, AllocationKind kind, std::size_t payloadSize,
                        std::size_t elementCount, void *allocationBase) {
    auto payloadAddress = reinterpret_cast<std::uintptr_t>(payload);
    auto trackedSize = std::max<std::size_t>(payloadSize, 1);
    std::lock_guard lock(allocationRegistryMutex);
    allocationRegistry[payloadAddress] = AllocationRecord{
        kind,
        payloadAddress,
        trackedSize,
        elementCount,
        allocationBase,
        false,
    };
}

AllocationRecord *findOwningAllocation(std::uintptr_t address) {
    auto it = allocationRegistry.upper_bound(address);
    if (it == allocationRegistry.begin()) {
        return nullptr;
    }
    --it;

    auto start = it->second.payloadAddress;
    auto end = start + it->second.payloadSize;
    if (address < start || address >= end) {
        return nullptr;
    }
    return &it->second;
}

void markAllocation(AllocationRecord &record,
                    std::vector<std::uintptr_t> &worklist) {
    if (record.marked) {
        return;
    }
    record.marked = true;
    worklist.push_back(record.payloadAddress);
}

void markAddress(std::uintptr_t address, std::vector<std::uintptr_t> &worklist) {
    if (address == 0) {
        return;
    }

    if (auto *record = findOwningAllocation(address)) {
        markAllocation(*record, worklist);
    }
}

void scanAllocationPayload(const AllocationRecord &record,
                           std::vector<std::uintptr_t> &worklist) {
    for (std::size_t offset = 0;
         offset + sizeof(std::uintptr_t) <= record.payloadSize;
         offset += sizeof(std::uintptr_t)) {
        std::uintptr_t candidate = 0;
        std::memcpy(&candidate,
                    reinterpret_cast<const void *>(record.payloadAddress + offset),
                    sizeof(candidate));
        markAddress(candidate, worklist);
    }
}

}  // namespace

namespace mvm {

GCCollectionStats collectManagedHeap(const std::vector<std::uintptr_t> &roots) {
    std::lock_guard lock(allocationRegistryMutex);

    GCCollectionStats stats;
    stats.collectionCount = ++collectionCount;
    stats.heapObjectCountBefore = allocationRegistry.size();

    std::vector<std::uintptr_t> worklist;
    worklist.reserve(roots.size());
    for (auto root : roots) {
        if (root == 0) {
            continue;
        }
        ++stats.rootCount;
        markAddress(root, worklist);
    }

    while (!worklist.empty()) {
        auto address = worklist.back();
        worklist.pop_back();

        auto it = allocationRegistry.find(address);
        if (it == allocationRegistry.end()) {
            continue;
        }
        scanAllocationPayload(it->second, worklist);
    }

    std::vector<std::uintptr_t> garbage;
    garbage.reserve(allocationRegistry.size());
    for (auto &[payloadAddress, record] : allocationRegistry) {
        if (record.marked) {
            ++stats.liveObjectCount;
            record.marked = false;
            continue;
        }

        ++stats.sweptObjectCount;
        stats.sweptBytes += record.payloadSize;
        std::free(record.allocationBase);
        garbage.push_back(payloadAddress);
    }

    for (auto payloadAddress : garbage) {
        allocationRegistry.erase(payloadAddress);
    }

    stats.heapObjectCountAfter = allocationRegistry.size();
    lastCollectionStats = stats;
    return stats;
}

void clearLastGCCollectionStats() {
    std::lock_guard lock(allocationRegistryMutex);
    lastCollectionStats = {};
}

GCCollectionStats getLastGCCollectionStats() {
    std::lock_guard lock(allocationRegistryMutex);
    return lastCollectionStats;
}

}  // namespace mvm

extern "C" void *__mvm_malloc(std::uint64_t size, std::uint64_t alignment) {
    std::size_t payloadSize = 0;
    std::size_t requestedAlignment = 0;
    if (!toSize(size, payloadSize) || !toSize(alignment, requestedAlignment)) {
        return nullptr;
    }

    auto *payload = allocateTracked(AllocationKind::Object, payloadSize, 1,
                                   requestedAlignment);
    if (payload) {
        auto *header = reinterpret_cast<AllocationHeader *>(payload) - 1;
        registerAllocation(payload, AllocationKind::Object, payloadSize, 1,
                           header->allocationBase);
        mvm::recordManagedAllocation(payloadSize);
    }
    return payload;
}

extern "C" void *__mvm_array_malloc(std::uint64_t element_size,
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

    auto *payload = allocateTracked(AllocationKind::Array, payloadSize, elementCount,
                                    requestedAlignment);
    if (payload) {
        auto *header = reinterpret_cast<AllocationHeader *>(payload) - 1;
        registerAllocation(payload, AllocationKind::Array, payloadSize, elementCount,
                           header->allocationBase);
        mvm::recordManagedAllocation(payloadSize);
    }
    return payload;
}

extern "C" void __mvm_free(void *payload) {
    // Managed deallocation is moving to GC ownership. Keep the symbol as a
    // temporary no-op until callers are migrated away from explicit free.
    (void)payload;
}

extern "C" void __mvm_array_free(void *payload) {
    // Managed deallocation is moving to GC ownership. Keep the symbol as a
    // temporary no-op until callers are migrated away from explicit free.
    (void)payload;
}

extern "C" std::uint64_t __mvm_array_length(const void *payload) {
    mvm::handlePendingRuntimeGCSafepoint(
        reinterpret_cast<std::uintptr_t *>(__builtin_frame_address(0)));
    auto *header =
        checkedHeader(payload, "__mvm_array_length", AllocationKind::Array);
    return header->elementCount;
}
