#pragma once
#include "freertos/FreeRTOS.h"

struct QueueDefinition;
typedef struct QueueDefinition* SemaphoreHandle_t;

extern "C" {
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void);
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t mutex, TickType_t ticks);
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t mutex);
void vSemaphoreDelete(SemaphoreHandle_t handle);
}
