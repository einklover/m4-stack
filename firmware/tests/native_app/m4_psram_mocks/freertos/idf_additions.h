#pragma once

#include <esp_heap_caps.h>

BaseType_t xTaskCreateWithCaps(TaskFunction_t fn, const char* name, uint32_t stackBytes,
                               void* arg, UBaseType_t priority, TaskHandle_t* out,
                               uint32_t caps);
void vTaskDeleteWithCaps(TaskHandle_t handle);
