#pragma once

#include <stddef.h>
#include <stdint.h>

// 屏幕物理尺寸，画布和驱动共用。
constexpr uint16_t EPD_WIDTH = 400;
constexpr uint16_t EPD_HEIGHT = 300;
constexpr size_t EPD_FRAME_BYTES = static_cast<size_t>(EPD_WIDTH) * EPD_HEIGHT / 4;

// 数值直接对应 JD79668 的两位像素编码，不是 RGB565 颜色。
enum class Color4 : uint8_t {
  Black = 0,
  White = 1,
  Yellow = 2,
  Red = 3,
};

static_assert(EPD_WIDTH % 4 == 0, "The packed 2-bit frame requires a width divisible by 4");
static_assert(sizeof(Color4) == 1, "Color4 must remain one byte wide");
