#pragma once
#include <cstdint>
#include <cstddef>
extern "C" {
std::uint32_t esp_random(void);
void esp_fill_random(void* buf, std::size_t len);
}
