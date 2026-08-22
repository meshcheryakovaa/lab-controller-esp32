#pragma once
#include <cstddef>
#define MALLOC_CAP_8BIT (1 << 2)
#define MALLOC_CAP_INTERNAL (1 << 11)
#define MALLOC_CAP_SPIRAM (1 << 10)
#define MALLOC_CAP_DEFAULT (1 << 12)
extern "C" {
std::size_t heap_caps_get_free_size(std::uint32_t caps);
std::size_t heap_caps_get_largest_free_block(std::uint32_t caps);
std::size_t heap_caps_get_minimum_free_size(std::uint32_t caps);
}

// LC_MEM_DIAGNOSTICS (esp32dev-debug) uses this one.
extern "C" {
bool heap_caps_check_integrity_all(bool print_errors);
}
