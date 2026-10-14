#pragma once

#include <stdint.h>

// 屏幕物理尺寸，画布和驱动共用。
constexpr uint16_t EPD_WIDTH = 400;
constexpr uint16_t EPD_HEIGHT = 300;

// 数值直接对应 JD79668 的两位像素编码，不是 RGB565 颜色。
enum class Color4 : uint8_t {
  Black = 0,
  White = 1,
  Yellow = 2,
  Red = 3,
};

using PixelProvider = Color4 (*)(uint16_t x, uint16_t y);

static_assert(EPD_WIDTH % 4 == 0, "The packed 2-bit frame requires a width divisible by 4");
static_assert(sizeof(Color4) == 1, "Color4 must remain one byte wide");
