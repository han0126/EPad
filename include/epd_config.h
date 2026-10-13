#pragma once

#include <Arduino.h>

namespace EpdPin {
constexpr uint8_t BUSY = 13;
constexpr uint8_t CS = 10;
constexpr uint8_t RST = 14;
constexpr uint8_t DC = 9;
constexpr uint8_t SCK = 12;
constexpr uint8_t SDA = 11;  // DIN on the display adapter
}  // namespace EpdPin

constexpr uint16_t EPD_WIDTH = 400;
constexpr uint16_t EPD_HEIGHT = 300;
constexpr uint32_t BUSY_TIMEOUT_MS = 180000;
constexpr uint8_t SPI_HALF_PERIOD_US = 1;

enum class Color4 : uint8_t {
  Black = 0,
  White = 1,
  Yellow = 2,
  Red = 3,
};

using PixelProvider = Color4 (*)(uint16_t x, uint16_t y);
using TextLength = uint16_t;

static_assert(EPD_WIDTH % 4 == 0, "The packed 2-bit frame requires a width divisible by 4");
static_assert(sizeof(Color4) == 1, "Color4 must remain one byte wide");
