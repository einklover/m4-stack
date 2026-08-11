#pragma once

#include <cstdint>

typedef int gpio_num_t;
typedef int esp_err_t;

#ifndef ESP_OK
#define ESP_OK 0
#endif

inline esp_err_t gpio_hold_dis(gpio_num_t) { return ESP_OK; }
inline esp_err_t gpio_hold_en(gpio_num_t) { return ESP_OK; }
