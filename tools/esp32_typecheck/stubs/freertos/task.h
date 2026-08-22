#pragma once
#include <cstdint>

#include "freertos/FreeRTOS.h"

typedef void* TaskHandle_t;
typedef std::uint32_t UBaseType_t;

extern "C" {
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task);
}
