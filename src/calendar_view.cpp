#include "calendar_view.h"

#include <cstdio>

#include "epd_canvas.h"
#include "text_renderer.h"

namespace {

// 400 x 300 屏幕的固定布局，给外边框预留空间。
constexpr int16_t GRID_X = 4;
constexpr int16_t HEADER_Y = 60;
constexpr int16_t HEADER_HEIGHT = 24;
constexpr int16_t GRID_Y = HEADER_Y + HEADER_HEIGHT;
constexpr int16_t CELL_WIDTH = 56;
constexpr int16_t CELL_HEIGHT = 35;
constexpr uint8_t GRID_ROWS = 6;

static_assert(GRID_X + CELL_WIDTH * 7 < EPD_WIDTH,
              "Calendar grid must fit inside the panel width");
static_assert(GRID_Y + CELL_HEIGHT * GRID_ROWS < EPD_HEIGHT - 3,
              "Calendar grid must fit above the bottom border");

const char* const WEEK_HEADERS[] = {
    "日", "一", "二", "三", "四", "五", "六"};

constexpr uint16_t BLACK = static_cast<uint16_t>(Color4::Black);
constexpr uint16_t RED = static_cast<uint16_t>(Color4::Red);
constexpr uint16_t YELLOW = static_cast<uint16_t>(Color4::Yellow);

void drawTitle(EpdCanvas& canvas, const calendar::MonthData& month) {
  char number[6];
  int16_t x = 12;
  std::snprintf(number, sizeof(number), "%u",
                static_cast<unsigned int>(month.year));
  x += text::drawText(canvas, number, x, 10, Color4::Black,
                      text::Font::Title25);
  x += text::drawText(canvas, "年", x + 4, 21) + 8;
  std::snprintf(number, sizeof(number), "%u",
                static_cast<unsigned int>(month.month));
  x += text::drawText(canvas, number, x, 10, Color4::Black,
                      text::Font::Title25);
  text::drawText(canvas, "月", x + 4, 21);

  if (month.highlightedDay >= 1 && month.highlightedDay <= month.dayCount) {
    const uint8_t weekday =
        (month.firstWeekday + month.highlightedDay - 1) % 7;
    char weekLabel[10];  // “星期三”占 9 个 UTF-8 字节，另加结束符。
    std::snprintf(weekLabel, sizeof(weekLabel), "星期%s", WEEK_HEADERS[weekday]);
    text::drawText(canvas, weekLabel, 222, 12);
  }
  text::drawText(canvas, "农历", 222, 34, Color4::Black,
                 text::Font::Chinese12);
}

void drawWeekHeader(EpdCanvas& canvas) {
  canvas.fillRect(GRID_X, HEADER_Y, CELL_WIDTH * 7, HEADER_HEIGHT, RED);
  for (uint8_t column = 0; column < 7; ++column) {
    text::drawCenteredText(canvas, WEEK_HEADERS[column],
                           GRID_X + column * CELL_WIDTH, HEADER_Y + 3,
                           CELL_WIDTH, Color4::White);
  }
}

void drawGridLines(EpdCanvas& canvas) {
  for (uint8_t row = 0; row <= GRID_ROWS; ++row) {
    canvas.drawFastHLine(GRID_X, GRID_Y + row * CELL_HEIGHT,
                        CELL_WIDTH * 7 + 1, BLACK);
  }
  for (uint8_t column = 0; column <= 7; ++column) {
    canvas.drawFastVLine(GRID_X + column * CELL_WIDTH, GRID_Y,
                        CELL_HEIGHT * GRID_ROWS + 1, BLACK);
  }
}

void drawDates(EpdCanvas& canvas, const calendar::MonthData& month) {
  for (uint8_t day = 1; day <= month.dayCount; ++day) {
    const uint8_t index = month.firstWeekday + day - 1;
    const int16_t left = GRID_X + (index % 7) * CELL_WIDTH;
    const int16_t top = GRID_Y + (index / 7) * CELL_HEIGHT;
    const Color4 color =
        day == month.highlightedDay ? Color4::Red : Color4::Black;
    if (day == month.highlightedDay) {
      canvas.drawRect(left + 2, top + 2, CELL_WIDTH - 3, CELL_HEIGHT - 3, RED);
    }

    char number[3];
    std::snprintf(number, sizeof(number), "%u", static_cast<unsigned int>(day));
    text::drawCenteredText(canvas, number, left, top + 2, CELL_WIDTH,
                           color, text::Font::Date17);
    if (month.lunarLabels != nullptr && day <= month.lunarLabelCount) {
      text::drawCenteredText(canvas, month.lunarLabels[day - 1], left,
                             top + 21, CELL_WIDTH, color,
                             text::Font::Chinese12);
    }
  }
}

}  // namespace

namespace calendar {

void drawMonth(EpdCanvas& canvas, const MonthData& month) {
  canvas.setRotation(0);
  canvas.clear();
  if (month.month < 1 || month.month > 12 || month.dayCount < 1 ||
      month.dayCount > 31 || month.firstWeekday > 6) {
    return;
  }

  canvas.drawRect(0, 0, EPD_WIDTH, EPD_HEIGHT, RED);
  canvas.drawRect(1, 1, EPD_WIDTH - 2, EPD_HEIGHT - 2, RED);
  canvas.drawRect(2, 2, EPD_WIDTH - 4, EPD_HEIGHT - 4, RED);
  canvas.fillRect(376, 8, 18, 18, YELLOW);  // 保留黄色色块，展示四色通道。
  drawTitle(canvas, month);
  drawWeekHeader(canvas);
  drawGridLines(canvas);
  drawDates(canvas, month);
}

}  // namespace calendar
