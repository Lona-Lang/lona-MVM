#include "mvm/runtime_memory.hh"

#include "mvm/gc.hh"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace {

enum class AllocationKind : std::uint64_t {
    Object = 1,
    Array = 2,
};

enum class AllocationStorageKind : std::uint64_t {
    System = 1,
    Slab = 2,
};

struct SlabClass;
struct SlabPage;

struct AllocationHeader {
    std::uint64_t magic = 0;
    AllocationKind kind = AllocationKind::Object;
    AllocationStorageKind storageKind = AllocationStorageKind::System;
    std::uint64_t elementCount = 0;
    std::size_t payloadSize = 0;
    const mvm::GCTypeDescriptor *typeDescriptor = nullptr;
    void *storageOwner = nullptr;
    AllocationHeader *prev = nullptr;
    AllocationHeader *next = nullptr;
    bool marked = false;
    bool live = false;
};

struct SlabClass {
    std::size_t payloadSize = 0;
    std::size_t alignment = 0;
    std::size_t headerOffset = 0;
    std::size_t slotStride = 0;
    std::size_t slotsPerPage = 0;
    SlabPage *nonFullPages = nullptr;
};

struct SlabPage {
    SlabClass *slabClass = nullptr;
    void *allocationBase = nullptr;
    std::uint8_t *pageStart = nullptr;
    std::size_t nextUnusedSlot = 0;
    std::size_t liveCount = 0;
    AllocationHeader *freeList = nullptr;
    SlabPage *prevNonFull = nullptr;
    SlabPage *nextNonFull = nullptr;
};

constexpr std::uint64_t kAllocationMagic = 0x4D564D4152524159ULL;  // MVMARRAY
constexpr std::uintptr_t kAllocationPageShift = 12;
constexpr std::size_t kAllocationPageBytes = 1ULL << kAllocationPageShift;
constexpr std::size_t kInitialPageBucketCapacity = 1024;
constexpr std::size_t kInitialSlabClassCapacity = 32;
constexpr std::size_t kManagedSlabPageBytes = 64 * 1024;
constexpr std::size_t kMaxSlabPayloadBytes = 512;
constexpr std::size_t kMaxSlabAlignment = 64;

std::mutex allocationRegistryMutex;
AllocationHeader *activeAllocationHead = nullptr;
llvm::DenseMap<std::uint64_t, SlabClass *> slabClassByKey;
std::vector<std::unique_ptr<SlabClass>> slabClassStorage;
std::vector<std::unique_ptr<SlabPage>> slabPageStorage;
llvm::DenseMap<std::uintptr_t, SlabPage *> slabPagesByAddressPage;
llvm::DenseMap<std::uintptr_t, llvm::SmallVector<AllocationHeader *, 4>>
    systemHeadersByPage;
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

std::uintptr_t allocationPageForAddress(std::uintptr_t address) {
    return address >> kAllocationPageShift;
}

bool isReservedDenseMapKey(std::uintptr_t value) {
    using KeyInfo = llvm::DenseMapInfo<std::uintptr_t>;
    return KeyInfo::isEqual(value, KeyInfo::getEmptyKey()) ||
           KeyInfo::isEqual(value, KeyInfo::getTombstoneKey());
}

std::uintptr_t allocationLastAddress(std::uintptr_t payloadAddress,
                                     std::size_t payloadSize) {
    auto offset = payloadSize - 1;
    if (payloadAddress >
        std::numeric_limits<std::uintptr_t>::max() - offset) {
        return std::numeric_limits<std::uintptr_t>::max();
    }
    return payloadAddress + offset;
}

void ensureAllocationRegistryStorage() {
    if (!slabClassStorage.empty() || !slabPageStorage.empty() ||
        !slabPagesByAddressPage.empty() || !systemHeadersByPage.empty()) {
        return;
    }

    slabClassByKey.reserve(kInitialSlabClassCapacity);
    slabClassStorage.reserve(kInitialSlabClassCapacity);
    slabPageStorage.reserve(kInitialPageBucketCapacity);
    slabPagesByAddressPage.reserve(kInitialPageBucketCapacity);
    systemHeadersByPage.reserve(kInitialPageBucketCapacity);
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
    if (header->magic != kAllocationMagic || !header->live) {
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer),
                      "%s received a pointer that is not managed by mvm",
                      operation);
        runtimeAbort(buffer);
    }

    if (expectedKind && header->kind != *expectedKind) {
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

std::uintptr_t payloadAddress(const AllocationHeader &header) {
    return reinterpret_cast<std::uintptr_t>(&header + 1);
}

void linkActiveAllocation(AllocationHeader *header) {
    header->prev = nullptr;
    header->next = activeAllocationHead;
    if (activeAllocationHead) {
        activeAllocationHead->prev = header;
    }
    activeAllocationHead = header;
}

void unlinkActiveAllocation(AllocationHeader *header) {
    if (header->prev) {
        header->prev->next = header->next;
    } else if (activeAllocationHead == header) {
        activeAllocationHead = header->next;
    }

    if (header->next) {
        header->next->prev = header->prev;
    }

    header->prev = nullptr;
    header->next = nullptr;
}

bool slabPageHasFreeSlots(const SlabPage &page) {
    return page.freeList != nullptr ||
           page.nextUnusedSlot < page.slabClass->slotsPerPage;
}

bool isSlabPageLinked(const SlabPage *page) {
    if (!page || !page->slabClass) {
        return false;
    }
    return page->prevNonFull != nullptr || page->nextNonFull != nullptr ||
           page->slabClass->nonFullPages == page;
}

void linkNonFullSlabPage(SlabPage *page) {
    if (!page || isSlabPageLinked(page) || !slabPageHasFreeSlots(*page)) {
        return;
    }

    auto *&head = page->slabClass->nonFullPages;
    page->prevNonFull = nullptr;
    page->nextNonFull = head;
    if (head) {
        head->prevNonFull = page;
    }
    head = page;
}

void unlinkNonFullSlabPage(SlabPage *page) {
    if (!page || !isSlabPageLinked(page)) {
        return;
    }

    auto *&head = page->slabClass->nonFullPages;
    if (page->prevNonFull) {
        page->prevNonFull->nextNonFull = page->nextNonFull;
    } else if (head == page) {
        head = page->nextNonFull;
    }

    if (page->nextNonFull) {
        page->nextNonFull->prevNonFull = page->prevNonFull;
    }

    page->prevNonFull = nullptr;
    page->nextNonFull = nullptr;
}

std::uint64_t slabClassKey(std::size_t payloadSize, std::size_t alignment) {
    return (static_cast<std::uint64_t>(alignment) << 32) |
           static_cast<std::uint64_t>(payloadSize);
}

AllocationHeader *slabHeaderAt(const SlabPage &page, std::size_t slotIndex) {
    auto slotStart = reinterpret_cast<std::uintptr_t>(page.pageStart) +
                     (slotIndex * page.slabClass->slotStride);
    auto headerAddress = slotStart + page.slabClass->headerOffset;
    return reinterpret_cast<AllocationHeader *>(headerAddress);
}

void registerSystemAllocationPages(AllocationHeader *header) {
    auto firstPage = allocationPageForAddress(payloadAddress(*header));
    auto lastPage = allocationPageForAddress(
        allocationLastAddress(payloadAddress(*header), header->payloadSize));

    for (auto page = firstPage; page <= lastPage; ++page) {
        if (isReservedDenseMapKey(page)) {
            runtimeAbort("managed allocation page key collided with DenseMap sentinel");
        }
        systemHeadersByPage[page].push_back(header);
    }
}

void unregisterSystemAllocationPages(AllocationHeader *header) {
    auto firstPage = allocationPageForAddress(payloadAddress(*header));
    auto lastPage = allocationPageForAddress(
        allocationLastAddress(payloadAddress(*header), header->payloadSize));

    for (auto page = firstPage; page <= lastPage; ++page) {
        auto pageIt = systemHeadersByPage.find(page);
        if (pageIt == systemHeadersByPage.end()) {
            continue;
        }

        auto &bucket = pageIt->second;
        auto bucketIt = std::find(bucket.begin(), bucket.end(), header);
        if (bucketIt != bucket.end()) {
            *bucketIt = bucket.back();
            bucket.pop_back();
        }
        if (bucket.empty()) {
            systemHeadersByPage.erase(pageIt);
        }
    }
}

void registerSlabPageMappings(SlabPage *page) {
    auto firstPage =
        allocationPageForAddress(reinterpret_cast<std::uintptr_t>(page->pageStart));
    auto lastPage = allocationPageForAddress(
        reinterpret_cast<std::uintptr_t>(page->pageStart) + kManagedSlabPageBytes - 1);

    for (auto currentPage = firstPage; currentPage <= lastPage; ++currentPage) {
        if (isReservedDenseMapKey(currentPage)) {
            runtimeAbort("managed slab page key collided with DenseMap sentinel");
        }
        slabPagesByAddressPage[currentPage] = page;
    }
}

void unregisterSlabPageMappings(const SlabPage &page) {
    auto firstPage =
        allocationPageForAddress(reinterpret_cast<std::uintptr_t>(page.pageStart));
    auto lastPage = allocationPageForAddress(
        reinterpret_cast<std::uintptr_t>(page.pageStart) + kManagedSlabPageBytes - 1);

    for (auto currentPage = firstPage; currentPage <= lastPage; ++currentPage) {
        slabPagesByAddressPage.erase(currentPage);
    }
}

SlabClass *getOrCreateSlabClass(std::size_t payloadSize, std::size_t alignment) {
    auto key = slabClassKey(payloadSize, alignment);
    auto it = slabClassByKey.find(key);
    if (it != slabClassByKey.end()) {
        return it->second;
    }

    auto payloadOffset = static_cast<std::size_t>(
        alignUp(sizeof(AllocationHeader), alignment));
    auto headerOffset = payloadOffset - sizeof(AllocationHeader);
    auto slotStride = static_cast<std::size_t>(
        alignUp(payloadOffset + std::max<std::size_t>(payloadSize, 1), alignment));
    if (slotStride == 0 || slotStride > kManagedSlabPageBytes) {
        return nullptr;
    }

    auto slotsPerPage = kManagedSlabPageBytes / slotStride;
    if (slotsPerPage == 0) {
        return nullptr;
    }

    auto slabClass = std::unique_ptr<SlabClass>(new SlabClass);
    slabClass->payloadSize = payloadSize;
    slabClass->alignment = alignment;
    slabClass->headerOffset = headerOffset;
    slabClass->slotStride = slotStride;
    slabClass->slotsPerPage = slotsPerPage;

    auto *slabClassPtr = slabClass.get();
    slabClassStorage.push_back(std::move(slabClass));
    slabClassByKey[key] = slabClassPtr;
    return slabClassPtr;
}

SlabPage *createSlabPage(SlabClass *slabClass) {
    auto totalBytes = kManagedSlabPageBytes + kAllocationPageBytes - 1;
    void *rawAllocation = std::malloc(totalBytes);
    if (!rawAllocation) {
        return nullptr;
    }

    auto alignedPage = alignUp(reinterpret_cast<std::uintptr_t>(rawAllocation),
                               kAllocationPageBytes);
    auto slabPage = std::unique_ptr<SlabPage>(new SlabPage);
    slabPage->slabClass = slabClass;
    slabPage->allocationBase = rawAllocation;
    slabPage->pageStart = reinterpret_cast<std::uint8_t *>(alignedPage);

    auto *slabPagePtr = slabPage.get();
    slabPageStorage.push_back(std::move(slabPage));
    registerSlabPageMappings(slabPagePtr);
    linkNonFullSlabPage(slabPagePtr);
    return slabPagePtr;
}

void releaseSlabPage(SlabPage *page) {
    unlinkNonFullSlabPage(page);
    unregisterSlabPageMappings(*page);
    std::free(page->allocationBase);

    auto it = std::find_if(
        slabPageStorage.begin(), slabPageStorage.end(),
        [page](const std::unique_ptr<SlabPage> &candidate) {
            return candidate.get() == page;
        });
    if (it != slabPageStorage.end()) {
        slabPageStorage.erase(it);
    }
}

bool shouldUseSlabAllocator(AllocationKind kind, std::size_t payloadSize,
                            std::size_t alignment) {
    return kind == AllocationKind::Object &&
           payloadSize <= kMaxSlabPayloadBytes &&
           alignment <= kMaxSlabAlignment;
}

void initializeHeader(AllocationHeader *header, AllocationKind kind,
                      AllocationStorageKind storageKind,
                      std::size_t payloadSize, std::size_t elementCount,
                      const mvm::GCTypeDescriptor *typeDescriptor,
                      void *storageOwner) {
    header->magic = kAllocationMagic;
    header->kind = kind;
    header->storageKind = storageKind;
    header->elementCount = elementCount;
    header->payloadSize = std::max<std::size_t>(payloadSize, 1);
    header->typeDescriptor = typeDescriptor;
    header->storageOwner = storageOwner;
    header->marked = false;
    header->live = true;
    header->prev = nullptr;
    header->next = nullptr;
}

void *allocateSystemTrackedLocked(AllocationKind kind, std::size_t payloadSize,
                                  std::size_t elementCount,
                                  std::size_t alignment,
                                  const mvm::GCTypeDescriptor *typeDescriptor) {
    std::size_t headerAndPadding = 0;
    if (!checkedAdd(sizeof(AllocationHeader), alignment - 1, headerAndPadding)) {
        return nullptr;
    }

    std::size_t totalBytes = 0;
    if (!checkedAdd(std::max<std::size_t>(payloadSize, 1), headerAndPadding,
                    totalBytes)) {
        return nullptr;
    }

    void *rawAllocation = std::malloc(totalBytes);
    if (!rawAllocation) {
        return nullptr;
    }

    auto alignedAddress = alignUp(
        reinterpret_cast<std::uintptr_t>(rawAllocation) + sizeof(AllocationHeader),
        alignment);
    auto *header = reinterpret_cast<AllocationHeader *>(alignedAddress) - 1;
    initializeHeader(header, kind, AllocationStorageKind::System, payloadSize,
                     elementCount, typeDescriptor, rawAllocation);
    registerSystemAllocationPages(header);
    linkActiveAllocation(header);
    return reinterpret_cast<void *>(alignedAddress);
}

void *allocateSlabTrackedLocked(std::size_t payloadSize, std::size_t alignment,
                                const mvm::GCTypeDescriptor *typeDescriptor) {
    auto *slabClass = getOrCreateSlabClass(payloadSize, alignment);
    if (!slabClass) {
        return nullptr;
    }

    auto *page = slabClass->nonFullPages;
    if (!page) {
        page = createSlabPage(slabClass);
        if (!page) {
            return nullptr;
        }
    }

    AllocationHeader *header = nullptr;
    if (page->freeList) {
        header = page->freeList;
        page->freeList = header->next;
    } else {
        header = slabHeaderAt(*page, page->nextUnusedSlot);
        ++page->nextUnusedSlot;
    }

    initializeHeader(header, AllocationKind::Object, AllocationStorageKind::Slab,
                     payloadSize, 1, typeDescriptor, page);
    ++page->liveCount;
    if (!slabPageHasFreeSlots(*page)) {
        unlinkNonFullSlabPage(page);
    }
    linkActiveAllocation(header);
    return reinterpret_cast<void *>(payloadAddress(*header));
}

void *allocateTrackedLocked(AllocationKind kind, std::size_t payloadSize,
                            std::size_t elementCount, std::size_t alignment,
                            const mvm::GCTypeDescriptor *typeDescriptor) {
    ensureAllocationRegistryStorage();

    if (alignment == 0) {
        alignment = defaultAlignment();
    }

    if (!isPowerOfTwo(alignment)) {
        return nullptr;
    }

    if (alignment < alignof(AllocationHeader)) {
        alignment = alignof(AllocationHeader);
    }

    if (shouldUseSlabAllocator(kind, payloadSize, alignment)) {
        if (auto *payload =
                allocateSlabTrackedLocked(payloadSize, alignment, typeDescriptor)) {
            return payload;
        }
    }

    return allocateSystemTrackedLocked(kind, payloadSize, elementCount, alignment,
                                       typeDescriptor);
}

AllocationHeader *findOwningSlabAllocation(std::uintptr_t address,
                                           SlabPage *page) {
    auto pageStart = reinterpret_cast<std::uintptr_t>(page->pageStart);
    auto pageOffset = address - pageStart;
    auto slotIndex = pageOffset / page->slabClass->slotStride;
    if (slotIndex >= page->slabClass->slotsPerPage) {
        return nullptr;
    }

    auto *header = slabHeaderAt(*page, slotIndex);
    if (!header->live || header->magic != kAllocationMagic ||
        header->storageKind != AllocationStorageKind::Slab) {
        return nullptr;
    }

    auto start = payloadAddress(*header);
    auto last = allocationLastAddress(start, header->payloadSize);
    if (address < start || address > last) {
        return nullptr;
    }
    return header;
}

AllocationHeader *findOwningAllocation(std::uintptr_t address) {
    auto page = allocationPageForAddress(address);
    if (isReservedDenseMapKey(page)) {
        return nullptr;
    }

    auto slabIt = slabPagesByAddressPage.find(page);
    if (slabIt != slabPagesByAddressPage.end()) {
        if (auto *header = findOwningSlabAllocation(address, slabIt->second)) {
            return header;
        }
    }

    auto pageIt = systemHeadersByPage.find(page);
    if (pageIt == systemHeadersByPage.end()) {
        return nullptr;
    }

    for (auto *header : pageIt->second) {
        if (!header->live) {
            continue;
        }

        auto start = payloadAddress(*header);
        auto last = allocationLastAddress(start, header->payloadSize);
        if (address >= start && address <= last) {
            return header;
        }
    }

    return nullptr;
}

void reclaimAllocation(AllocationHeader *header) {
    unlinkActiveAllocation(header);

    auto storageKind = header->storageKind;
    auto *storageOwner = header->storageOwner;
    header->live = false;
    header->marked = false;

    if (storageKind == AllocationStorageKind::System) {
        unregisterSystemAllocationPages(header);
        std::free(storageOwner);
        return;
    }

    auto *page = static_cast<SlabPage *>(storageOwner);
    auto wasFull = !slabPageHasFreeSlots(*page);
    header->next = page->freeList;
    page->freeList = header;

    if (page->liveCount == 0) {
        runtimeAbort("slab allocator observed an invalid live object count");
    }
    --page->liveCount;

    if (page->liveCount == 0) {
        releaseSlabPage(page);
        return;
    }

    if (wasFull) {
        linkNonFullSlabPage(page);
    }
}

void markAllocation(AllocationHeader &header,
                    std::vector<std::uintptr_t> &worklist) {
    if (header.marked) {
        return;
    }
    header.marked = true;
    worklist.push_back(payloadAddress(header));
}

void markAddress(std::uintptr_t address, std::vector<std::uintptr_t> &worklist) {
    if (address == 0) {
        return;
    }

    if (auto *header = findOwningAllocation(address)) {
        markAllocation(*header, worklist);
    }
}

void scanAllocationPayload(const AllocationHeader &header,
                           std::vector<std::uintptr_t> &worklist) {
    auto start = payloadAddress(header);
    if (header.typeDescriptor) {
        auto stride =
            static_cast<std::size_t>(header.typeDescriptor->instanceSize);
        if (stride != 0) {
            auto elementCount =
                header.kind == AllocationKind::Array ? header.elementCount : 1;
            for (std::size_t elementIndex = 0; elementIndex < elementCount;
                 ++elementIndex) {
                auto elementBase = start + elementIndex * stride;
                for (std::uint64_t slotIndex = 0;
                     slotIndex < header.typeDescriptor->referenceSlotCount;
                     ++slotIndex) {
                    if (!header.typeDescriptor->referenceSlotOffsets) {
                        break;
                    }
                    auto offset = static_cast<std::size_t>(
                        header.typeDescriptor->referenceSlotOffsets[slotIndex]);
                    if (offset + sizeof(std::uintptr_t) > stride) {
                        continue;
                    }

                    std::uintptr_t candidate = 0;
                    std::memcpy(&candidate,
                                reinterpret_cast<const void *>(elementBase + offset),
                                sizeof(candidate));
                    markAddress(candidate, worklist);
                }
            }
            return;
        }
    }

    for (std::size_t offset = 0;
         offset + sizeof(std::uintptr_t) <= header.payloadSize;
         offset += sizeof(std::uintptr_t)) {
        std::uintptr_t candidate = 0;
        std::memcpy(&candidate, reinterpret_cast<const void *>(start + offset),
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
    for (auto *header = activeAllocationHead; header != nullptr;
         header = header->next) {
        ++stats.heapObjectCountBefore;
        stats.heapBytesBefore += header->payloadSize;
    }

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

        auto *header = reinterpret_cast<AllocationHeader *>(address) - 1;
        if (!header->live || !header->marked || header->magic != kAllocationMagic) {
            continue;
        }
        scanAllocationPayload(*header, worklist);
    }

    for (auto *header = activeAllocationHead; header != nullptr;) {
        auto *next = header->next;
        if (header->marked) {
            ++stats.liveObjectCount;
            stats.heapBytesAfter += header->payloadSize;
            header->marked = false;
        } else {
            ++stats.sweptObjectCount;
            stats.sweptBytes += header->payloadSize;
            reclaimAllocation(header);
        }
        header = next;
    }

    for (auto *header = activeAllocationHead; header != nullptr;
         header = header->next) {
        ++stats.heapObjectCountAfter;
    }

    lastCollectionStats = stats;
    mvm::updateManagedHeapUsage(stats.heapBytesAfter);
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

extern "C" void *__mvm_malloc() {
    runtimeAbort(
        "__mvm_malloc must be rewritten to __mvm_malloc_typed with alloc type metadata");
}

extern "C" void *__mvm_malloc_typed(const void *type_descriptor) {
    auto *descriptor =
        static_cast<const mvm::GCTypeDescriptor *>(type_descriptor);
    if (!descriptor ||
        descriptor->abiVersion != mvm::kGCTypeDescriptorABIVersion) {
        runtimeAbort(
            "__mvm_malloc_typed received an incompatible GC type descriptor");
    }
    if (descriptor->instanceSize == 0) {
        runtimeAbort("__mvm_malloc_typed requires alloc type metadata");
    }

    std::size_t payloadSize = 0;
    if (!toSize(descriptor->instanceSize, payloadSize)) {
        return nullptr;
    }

    std::size_t alignment = 0;
    if (descriptor->instanceAlignment != 0 &&
        !toSize(descriptor->instanceAlignment, alignment)) {
        return nullptr;
    }

    void *payload = nullptr;
    {
        std::lock_guard lock(allocationRegistryMutex);
        payload = allocateTrackedLocked(AllocationKind::Object, payloadSize, 1,
                                       alignment, descriptor);
    }
    if (payload) {
        mvm::recordManagedAllocation(payloadSize);
    }
    return payload;
}

extern "C" void *__mvm_array_malloc(std::uint64_t element_count) {
    (void)element_count;
    runtimeAbort(
        "__mvm_array_malloc must be rewritten to __mvm_array_malloc_typed with alloc type metadata");
}

extern "C" void *__mvm_array_malloc_typed(std::uint64_t element_count,
                                          const void *type_descriptor) {
    std::size_t elementCount = 0;
    if (!toSize(element_count, elementCount)) {
        return nullptr;
    }

    auto *descriptor =
        static_cast<const mvm::GCTypeDescriptor *>(type_descriptor);
    if (!descriptor ||
        descriptor->abiVersion != mvm::kGCTypeDescriptorABIVersion) {
        runtimeAbort(
            "__mvm_array_malloc_typed received an incompatible GC type descriptor");
    }
    if (descriptor->instanceSize == 0) {
        runtimeAbort("__mvm_array_malloc_typed requires alloc type metadata");
    }

    std::size_t elementSize = 0;
    if (!toSize(descriptor->instanceSize, elementSize)) {
        return nullptr;
    }

    std::size_t payloadSize = 0;
    if (!checkedMul(elementSize, elementCount, payloadSize)) {
        return nullptr;
    }

    std::size_t alignment = 0;
    if (descriptor->instanceAlignment != 0 &&
        !toSize(descriptor->instanceAlignment, alignment)) {
        return nullptr;
    }

    void *payload = nullptr;
    {
        std::lock_guard lock(allocationRegistryMutex);
        payload = allocateTrackedLocked(AllocationKind::Array, payloadSize,
                                       elementCount, alignment, descriptor);
    }
    if (payload) {
        mvm::recordManagedAllocation(payloadSize);
    }
    return payload;
}

extern "C" std::uint64_t __mvm_array_length(const void *payload) {
    auto *header =
        checkedHeader(payload, "__mvm_array_length", AllocationKind::Array);
    return header->elementCount;
}
