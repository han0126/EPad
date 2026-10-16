#pragma once

#include "display_config.h"

class Adafruit_GFX;

namespace text {

// 中文使用文泉驿字库；数字字体仅用于日期和标题数字。
enum class Font : uint8_t {
  Chinese16,
  Chinese12,
  Date17,
  Title20,
};

int16_t textWidth(const char* text, Font font = Font::Chinese16);

// 输入 UTF-8 字符串；top 为文字顶部，letterSpacing 为额外字间距。
// 返回文字的横向排版步进，默认间距保持原有布局。
int16_t drawText(Adafruit_GFX& canvas, const char* text, int16_t x, int16_t top,
                 Color4 color = Color4::Black,
                 Font font = Font::Chinese16, uint8_t letterSpacing = 0);

// 在指定宽度内按真实字宽水平居中。
void drawCenteredText(Adafruit_GFX& canvas, const char* text, int16_t left,
                      int16_t top, int16_t width,
                      Color4 color = Color4::Black,
                      Font font = Font::Chinese16);

}  // namespace text
