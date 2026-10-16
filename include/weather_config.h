#pragma once

#include <stddef.h>
#include <stdint.h>

namespace WeatherConfig {

constexpr uint32_t IO_TIMEOUT_MS = 6000;
constexpr uint32_t REQUEST_BUDGET_MS = 20000;
constexpr size_t MAX_BODY_BYTES = 16384;
constexpr size_t MAX_JSON_BYTES = 16384;
constexpr uint32_t TASK_STACK_BYTES = 16384;

}  // namespace WeatherConfig
