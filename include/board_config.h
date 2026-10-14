#pragma once

#include <stdint.h>

// 驱动板接线；SDA 对应墨水屏适配板的 DIN。
namespace EpdPin {
constexpr uint8_t BUSY = 13;
constexpr uint8_t CS = 10;
constexpr uint8_t RST = 14;
constexpr uint8_t DC = 9;
constexpr uint8_t SCK = 12;
constexpr uint8_t SDA = 11;
}  // namespace EpdPin

// 保留当前已正常工作的 BUSY 超时和软件 SPI 时序。
constexpr uint32_t BUSY_TIMEOUT_MS = 180000;
constexpr uint8_t SPI_HALF_PERIOD_US = 1;
