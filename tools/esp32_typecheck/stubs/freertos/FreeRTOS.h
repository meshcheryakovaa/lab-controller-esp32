#pragma once
// Enough FreeRTOS for the host typecheck.  See stubs/README in check.sh: this
// proves the ESP32 sources parse and agree on signatures, not that they run.
#include <cstdint>

#define portMAX_DELAY 0xffffffffUL
typedef std::uint32_t TickType_t;
typedef int BaseType_t;
#define pdTRUE 1
#define pdFALSE 0
