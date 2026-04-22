#pragma once

#include <cstdint>

extern "C" {

void *__mvm_malloc(std::uint64_t size, std::uint64_t alignment);
void *__mvm_array_malloc(std::uint64_t element_size,
                         std::uint64_t element_count,
                         std::uint64_t alignment);
void __mvm_free(void *payload);
void __mvm_array_free(void *payload);
std::uint64_t __mvm_array_length(const void *payload);

}  // extern "C"
