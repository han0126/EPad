#include "weather_icons.h"

#include <pgmspace.h>

#include "epd_canvas.h"

namespace {

using weather_icons::Condition;
constexpr size_t STRIDE = (weather_icons::SIZE + 7) / 8;
constexpr size_t ICON_BYTES = STRIDE * weather_icons::SIZE;

#include "weather_icon_assets.inc"

static_assert(sizeof(CONDITIONS) / sizeof(CONDITIONS[0]) ==
                  sizeof(ICON_BITMAPS) / sizeof(ICON_BITMAPS[0]),
              "Weather descriptors and masks must match");

size_t indexOf(uint16_t code) {
  size_t fallback = 0;
  for (size_t i = 0; i < weather_icons::count(); ++i) {
    if (CONDITIONS[i].code == code) return i;
    if (CONDITIONS[i].code == 999) fallback = i;
  }
  return fallback;
}

bool pixel(const uint8_t* bitmap, int16_t x, int16_t y) {
  const uint8_t bits = pgm_read_byte(bitmap + y * STRIDE + x / 8);
  return (bits & (0x80 >> (x % 8))) != 0;
}

}  // namespace

namespace weather_icons {

size_t count() {
  return sizeof(CONDITIONS) / sizeof(CONDITIONS[0]);
}

bool isKnown(uint16_t code) {
  return CONDITIONS[indexOf(code)].code == code;
}

const Condition& condition(uint16_t code) {
  return CONDITIONS[indexOf(code)];
}

void draw(EpdCanvas& canvas, uint16_t code, int16_t left, int16_t top) {
  const size_t index = indexOf(code);
  const uint8_t* bitmap = ICON_BITMAPS[index];
  const Color4 color = CONDITIONS[index].color;
  // 黄色笔画先加黑边，保证白底墨水屏上仍有足够对比度。
  if (color == Color4::Yellow) {
    for (int16_t y = 0; y < SIZE; ++y) {
      for (int16_t x = 0; x < SIZE; ++x) {
        if (!pixel(bitmap, x, y)) continue;
        for (int16_t dy = -1; dy <= 1; ++dy) {
          for (int16_t dx = -1; dx <= 1; ++dx) {
            if (x + dx >= 0 && x + dx < SIZE && y + dy >= 0 && y + dy < SIZE) {
              canvas.drawPixel(left + x + dx, top + y + dy,
                               static_cast<uint16_t>(Color4::Black));
            }
          }
        }
      }
    }
  }
  for (int16_t y = 0; y < SIZE; ++y) {
    for (int16_t x = 0; x < SIZE; ++x) {
      if (pixel(bitmap, x, y)) canvas.drawPixel(left + x, top + y, static_cast<uint16_t>(color));
    }
  }
}

}  // namespace weather_icons
