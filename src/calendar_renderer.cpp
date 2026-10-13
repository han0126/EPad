#include "calendar_renderer.h"

#include <Arduino.h>

#include "epd_driver.h"
#include "label_font.h"

namespace {

constexpr uint16_t HEADER_Y = 60;
constexpr uint16_t HEADER_HEIGHT = 24;
constexpr uint16_t GRID_Y = HEADER_Y + HEADER_HEIGHT;
constexpr uint16_t CELL_WIDTH = 56;
constexpr uint16_t CELL_HEIGHT = 36;
constexpr uint8_t DAYS_IN_MONTH = 30;
constexpr uint8_t FIRST_DAY_COLUMN = 0;  // Fixed demo: June 2025 starts on Sunday.
constexpr uint8_t TODAY = 18;

// The static calendar only needs these Chinese characters. Each glyph is a
// 16x16 monochrome bitmap; bit 15 is the left-most pixel of a row.
enum class Glyph : uint8_t {
  Year,
  Month,
  Day,
  Star,
  Period,
  Sun,
  Mon,
  Tue,
  Wed,
  Thu,
  Fri,
  Sat,
  Lunar,
  History,
  Count,
};

const uint16_t GLYPHS[static_cast<uint8_t>(Glyph::Count)][16] PROGMEM = {
    // 年
    {0x07E0, 0x0100, 0x07F0, 0x0100, 0x1FF8, 0x1110, 0x1110, 0x1FF0,
     0x0110, 0x0110, 0x1FF0, 0x0110, 0x0110, 0x0110, 0x07F0, 0x0000},
    // 月
    {0x0FF0, 0x0810, 0x0810, 0x0810, 0x0FF0, 0x0810, 0x0810, 0x0810,
     0x0FF0, 0x0810, 0x0810, 0x0810, 0x0810, 0x0810, 0x1FF8, 0x0000},
    // 日
    {0x0FF0, 0x0810, 0x0810, 0x0810, 0x0810, 0x0810, 0x0810, 0x0810,
     0x0810, 0x0810, 0x0810, 0x0810, 0x0810, 0x0810, 0x0FF0, 0x0000},
    // 星
    {0x0100, 0x07F0, 0x0100, 0x1FF8, 0x1110, 0x07E0, 0x0100, 0x1FF8,
     0x1110, 0x1110, 0x1FF8, 0x0100, 0x0100, 0x07F0, 0x0000, 0x0000},
    // 期
    {0x1FF8, 0x1010, 0x1010, 0x1FF0, 0x1010, 0x1010, 0x1FF0, 0x1010,
     0x1010, 0x1FF0, 0x1010, 0x1010, 0x1010, 0x1010, 0x1FF8, 0x0000},
    // 日
    {0x0FF0, 0x0810, 0x0810, 0x0810, 0x0810, 0x0810, 0x0810, 0x0810,
     0x0810, 0x0810, 0x0810, 0x0810, 0x0810, 0x0810, 0x0FF0, 0x0000},
    // 一
    {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x1FF8,
     0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // 二
    {0x0000, 0x0000, 0x07E0, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
     0x0000, 0x1FF8, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // 三
    {0x0000, 0x07E0, 0x0000, 0x0000, 0x0000, 0x1FF8, 0x0000, 0x0000,
     0x0000, 0x0000, 0x0000, 0x1FF8, 0x0000, 0x0000, 0x0000, 0x0000},
    // 四
    {0x1FF8, 0x1010, 0x1110, 0x1210, 0x1410, 0x1810, 0x1110, 0x1110,
     0x1110, 0x1110, 0x1110, 0x1110, 0x1010, 0x1FF8, 0x0000, 0x0000},
    // 五
    {0x1FF8, 0x0100, 0x0100, 0x07F0, 0x0100, 0x0100, 0x0100, 0x1FF8,
     0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // 六
    {0x0100, 0x0100, 0x07F0, 0x0100, 0x0100, 0x1FF8, 0x0100, 0x0300,
     0x0500, 0x0900, 0x1100, 0x2100, 0x4100, 0x0100, 0x0000, 0x0000},
    // 农
    {0x0100, 0x07F0, 0x0100, 0x1FF8, 0x1110, 0x07E0, 0x0500, 0x0900,
     0x1110, 0x1FF8, 0x0110, 0x0210, 0x0410, 0x0810, 0x1010, 0x0000},
    // 历
    {0x1FF8, 0x1000, 0x1000, 0x17F0, 0x1010, 0x1010, 0x1FF0, 0x1010,
     0x1010, 0x1FF0, 0x1010, 0x1010, 0x1010, 0x1010, 0x1FF8, 0x0000},
};

struct GlyphText {
  const Glyph* glyphs;
  uint8_t length;
};

constexpr Glyph WEEK_LABEL[] = {Glyph::Star, Glyph::Period, Glyph::Wed};
constexpr Glyph LUNAR_LABEL[] = {Glyph::Lunar, Glyph::History};
constexpr Glyph WEEK_HEADERS[] = {Glyph::Sun, Glyph::Mon, Glyph::Tue,
                                  Glyph::Wed, Glyph::Thu, Glyph::Fri,
                                  Glyph::Sat};

bool inRectangle(uint16_t x, uint16_t y, uint16_t left, uint16_t top,
                 uint16_t width, uint16_t height) {
  return x >= left && x < left + width && y >= top && y < top + height;
}

bool glyphPixel(Glyph glyph, uint16_t originX, uint16_t originY, uint16_t x,
                uint16_t y) {
  if (x < originX || x >= originX + 16 || y < originY || y >= originY + 16) {
    return false;
  }
  const uint16_t row = pgm_read_word(&GLYPHS[static_cast<uint8_t>(glyph)][y - originY]);
  return (row & (1U << (15 - (x - originX)))) != 0;
}

bool glyphTextPixel(GlyphText text, uint16_t originX, uint16_t originY,
                    uint16_t x, uint16_t y) {
  constexpr uint16_t ADVANCE = 18;
  if (text.glyphs == nullptr) return false;

  for (uint8_t index = 0; index < text.length; ++index) {
    if (glyphPixel(text.glyphs[index], originX + index * ADVANCE, originY, x,
                   y)) {
      return true;
    }
  }
  return false;
}

bool dateNumberPixel(uint8_t day, uint16_t originX, uint16_t originY,
                     uint16_t x, uint16_t y) {
  char value[3];
  if (day >= 10) {
    value[0] = static_cast<char>('0' + day / 10);
    value[1] = static_cast<char>('0' + day % 10);
    value[2] = '\0';
  } else {
    value[0] = static_cast<char>('0' + day);
    value[1] = '\0';
  }
  const uint8_t length = day >= 10 ? 2 : 1;
  return textPixel(value, length, originX, originY, 2, x, y);
}

bool gridPixel(uint16_t x, uint16_t y) {
  if (x >= EPD_WIDTH || y < GRID_Y) return false;
  const uint16_t relativeY = y - GRID_Y;
  const uint16_t column = x / CELL_WIDTH;
  const uint16_t row = relativeY / CELL_HEIGHT;
  if (column >= 7 || row >= 6) return false;

  const uint8_t day = static_cast<uint8_t>(row * 7 + column - FIRST_DAY_COLUMN + 1);
  if (day > DAYS_IN_MONTH) return false;

  const uint16_t cellX = column * CELL_WIDTH;
  const uint16_t cellY = GRID_Y + row * CELL_HEIGHT;


  if (day == TODAY &&
      (inRectangle(x, y, cellX + 2, cellY + 2, CELL_WIDTH - 4, CELL_HEIGHT - 4) &&
       (x == cellX + 2 || x == cellX + CELL_WIDTH - 3 ||
        y == cellY + 2 || y == cellY + CELL_HEIGHT - 3))) {
    return true;
  }
  if (dateNumberPixel(day, cellX + 19, cellY + 8, x, y)) {
    return true;
  }
  return false;
}

}  // namespace

Color4 calendarPixel(uint16_t x, uint16_t y) {
  if (x >= EPD_WIDTH || y >= EPD_HEIGHT) return Color4::White;

  // A clean white base with a red outer frame.
  if (x < 3 || x >= EPD_WIDTH - 3 || y < 3 || y >= EPD_HEIGHT - 3) {
    return Color4::Red;
  }

  // Yellow test swatch. It is deliberately isolated from the text and grid
  // so the yellow channel can be verified without changing the layout.
  if (inRectangle(x, y, 376, 8, 18, 18)) {
    return Color4::Yellow;
  }

  // Header background and white weekday labels.
  if (y >= HEADER_Y && y < GRID_Y) {
    for (uint8_t column = 0; column < 7; ++column) {
      if (glyphPixel(WEEK_HEADERS[column], column * CELL_WIDTH + 20,
                     HEADER_Y + 4, x, y)) {
        return Color4::White;
      }
    }
    return Color4::Red;
  }

  // Calendar grid lines.
  if (y >= GRID_Y &&
      ((y - GRID_Y) % CELL_HEIGHT == 0 ||
       (y - GRID_Y) % CELL_HEIGHT == CELL_HEIGHT - 1) &&
      x < 7 * CELL_WIDTH) {
    return Color4::Black;
  }
  if (y >= GRID_Y && x % CELL_WIDTH == 0) return Color4::Black;

  // Fixed title: 2025年6月.
  if (textPixel("2025", 4, 12, 12, 2, x, y) ||
      glyphPixel(Glyph::Year, 92, 12, x, y) ||
      textPixel("6", 1, 116, 12, 2, x, y) ||
      glyphPixel(Glyph::Month, 132, 12, x, y)) {
    return Color4::Black;
  }

  // Fixed supplementary labels.
  if (glyphTextPixel({WEEK_LABEL, 3}, 220, 12, x, y) ||
      glyphTextPixel({LUNAR_LABEL, 2}, 310, 12, x, y)) {
    return Color4::Black;
  }



  // Date numbers and today's red outline are rendered after the background.
  if (y >= GRID_Y && gridPixel(x, y)) {
    if (x >= (TODAY - 1) % 7 * CELL_WIDTH &&
        x < ((TODAY - 1) % 7 + 1) * CELL_WIDTH &&
        y >= GRID_Y + ((TODAY - 1) / 7) * CELL_HEIGHT &&
        y < GRID_Y + ((TODAY - 1) / 7 + 1) * CELL_HEIGHT) {
      return Color4::Red;
    }
    return Color4::Black;
  }

  return Color4::White;
}

void sendCalendarFrame() {
  epd::sendFrame(calendarPixel);
}
