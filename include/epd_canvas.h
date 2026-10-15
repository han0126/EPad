#pragma once

#include <stddef.h>

#include <Adafruit_GFX.h>

#include "display_config.h"

// 为 Adafruit GFX 提供四色画布；绘图颜色使用 Color4 的整数值。
class EpdCanvas : public Adafruit_GFX {
 public:
  EpdCanvas();

  void drawPixel(int16_t x, int16_t y, uint16_t color) override;
  void clear(Color4 color = Color4::White);
  // 导入按物理坐标、高位像素在前排列的完整两位像素帧。
  bool loadPackedFrame(const uint8_t* data, size_t length);
  // 按屏幕物理坐标取色，不受绘图旋转影响。
  Color4 pixel(uint16_t x, uint16_t y) const;

 private:
  // 400 x 300 两位像素共占 30,000 字节。
  uint8_t frame_[EPD_FRAME_BYTES];
};
