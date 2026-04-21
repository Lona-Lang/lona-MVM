#pragma once

#include <cstdint>

extern "C" {

void *__mvm_malloc(std::uint64_t size, std::uint64_t alignment);
void *__mvm_malloc_array(std::uint64_t element_size,
                         std::uint64_t element_count,
                         std::uint64_t alignment);
void __mvm_free(void *payload);
std::uint64_t __mvm_allocation_size(const void *payload);
std::uint64_t __mvm_array_length(const void *payload);
std::uint64_t __mvm_array_element_size(const void *payload);
void __mvm_bounds_check(const void *payload, std::uint64_t start_index,
                        std::uint64_t access_width);
void __mvm_bounds_check_static(std::uint64_t start_index,
                               std::uint64_t access_width,
                               std::uint64_t length);

}  // extern "C"
