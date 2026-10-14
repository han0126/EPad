#include "text_renderer.h"

#include <U8g2_for_Adafruit_GFX.h>

namespace {

U8G2_FOR_ADAFRUIT_GFX fonts;

void selectFont(text::Font font) {
  switch (font) {
    case text::Font::Chinese12:
      fonts.setFont(u8g2_font_wqy12_t_gb2312);
      break;
    case text::Font::Date17:
      fonts.setFont(u8g2_font_fub17_tn);
      break;
    case text::Font::Title25:
      fonts.setFont(u8g2_font_fub25_tn);
      break;
    case text::Font::Chinese16:
    default:
      fonts.setFont(u8g2_font_wqy16_t_gb2312);
      break;
  }
}

}  // namespace

namespace text {

int16_t textWidth(const char* text, Font font) {
  if (text == nullptr || *text == '\0') return 0;
  selectFont(font);
  return fonts.getUTF8Width(text);
}

int16_t drawText(Adafruit_GFX& canvas, const char* text, int16_t x, int16_t top,
                 Color4 color, Font font) {
  if (text == nullptr || *text == '\0') return 0;

  fonts.begin(canvas);
  selectFont(font);
  fonts.setFontMode(1);  // 透明绘制，只覆盖文字笔画。
  fonts.setFontDirection(0);
  fonts.setForegroundColor(static_cast<uint16_t>(color));
  // U8g2 使用基线坐标，统一在此将顶部坐标转换为基线。
  return fonts.drawUTF8(x, top + fonts.getFontAscent(), text);
}

void drawCenteredText(Adafruit_GFX& canvas, const char* text, int16_t left,
                      int16_t top, int16_t width, Color4 color, Font font) {
  if (text == nullptr || width <= 0) return;
  const int16_t advance = textWidth(text, font);
  drawText(canvas, text, left + (width - advance) / 2, top, color, font);
}

}  // namespace text
