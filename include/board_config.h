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

// 1：使用已验证的硬件 SPI；改为 0 可回退到软件 SPI，不改变接线。
#ifndef EPD_USE_HARDWARE_SPI
#define EPD_USE_HARDWARE_SPI 1
#endif

// 仅像素数据按行连续发送；改为 0 可恢复上一阶段的逐字节硬件 SPI。
#ifndef EPD_SPI_BURST_PIXELS
#define EPD_SPI_BURST_PIXELS 1
#endif

// 保持已验证的低速时钟，本轮只减少传输事务数量。
constexpr uint32_t EPD_HARDWARE_SPI_HZ = 250000;
static_assert(EPD_USE_HARDWARE_SPI == 0 || EPD_USE_HARDWARE_SPI == 1,
              "EPD_USE_HARDWARE_SPI must be 0 or 1");
static_assert(EPD_SPI_BURST_PIXELS == 0 || EPD_SPI_BURST_PIXELS == 1,
              "EPD_SPI_BURST_PIXELS must be 0 or 1");
static_assert(!EPD_USE_HARDWARE_SPI || (EPD_HARDWARE_SPI_HZ > 0 && EPD_HARDWARE_SPI_HZ <= 500000),
              "Keep the initial hardware SPI trial at or below 500 kHz");
