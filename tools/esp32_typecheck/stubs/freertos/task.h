#pragma once
#include <cstdint>

#include "freertos/FreeRTOS.h"

typedef void* TaskHandle_t;
typedef std::uint32_t UBaseType_t;

extern "C" {
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task);
}

// M17 runs the uploader on its own task; see CloudUploadTask.
typedef std::uint32_t StackType_t;
#define pdPASS 1
#define pdMS_TO_TICKS(ms) (ms)
extern "C" {
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void*), const char* name,
                                   std::uint32_t stack, void* param,
                                   UBaseType_t priority, TaskHandle_t* handle,
                                   BaseType_t core);
void vTaskDelay(TickType_t ticks);
void vTaskDelete(TaskHandle_t task);
}
