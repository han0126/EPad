#include "calendar_view.h"

#include <cstdio>

#include "epd_canvas.h"
#include "text_renderer.h"

namespace {

// 按确认的预览图排版：白底、紧凑标题、星期色条、无网格日期区。
constexpr int16_t GRID_X = 4;
constexpr int16_t HEADER_Y = 57;
constexpr int16_t HEADER_HEIGHT = 22;
constexpr int16_t GRID_Y = HEADER_Y + HEADER_HEIGHT;
constexpr int16_t CELL_WIDTH = 56;
constexpr int16_t GRID_WIDTH = CELL_WIDTH * 7;
constexpr int16_t BOTTOM_MARGIN = 5;
constexpr int16_t TODAY_RADIUS = 6;
constexpr int16_t TODAY_STROKE = 2;
constexpr int16_t TODAY_INSET_X = 4;
// 按实际上屏效果微调，避免中文笔画贴近色条或标签的上沿。
constexpr int16_t WEEKDAY_TEXT_OFFSET_Y = 5;
constexpr int16_t DAY_LABEL_TEXT_OFFSET_Y = 4;

static_assert(GRID_X + CELL_WIDTH * 7 < EPD_WIDTH,
              "Calendar grid must fit inside the panel width");
static_assert(GRID_Y < EPD_HEIGHT - BOTTOM_MARGIN,
              "Calendar dates require space below the weekday strip");

const char* const WEEK_HEADERS[] = {
    "日", "一", "二", "三", "四", "五", "六"};

constexpr uint16_t BLACK = static_cast<uint16_t>(Color4::Black);
constexpr uint16_t WHITE = static_cast<uint16_t>(Color4::White);
constexpr uint16_t RED = static_cast<uint16_t>(Color4::Red);
constexpr uint16_t YELLOW = static_cast<uint16_t>(Color4::Yellow);

void drawTitle(EpdCanvas& canvas, const calendar::MonthData& month) {
  char number[6];
  int16_t x = 10;
  constexpr int16_t NUMBER_TOP = 15;
  constexpr int16_t UNIT_TOP = 26;
  constexpr int16_t META_X = 254;

  // 大号数字与小号“年月日”共用底线，数字红色、单位黑色。
  std::snprintf(number, sizeof(number), "%u",
                static_cast<unsigned int>(month.year));
  x += text::drawText(canvas, number, x, NUMBER_TOP, Color4::Red,
                      text::Font::Title25);
  x += text::drawText(canvas, "年", x + 2, UNIT_TOP) + 4;
  std::snprintf(number, sizeof(number), "%u",
                static_cast<unsigned int>(month.month));
  x += text::drawText(canvas, number, x, NUMBER_TOP, Color4::Red,
                      text::Font::Title25);
  x += text::drawText(canvas, "月", x + 2, UNIT_TOP) + 4;

  if (month.highlightedDay >= 1 && month.highlightedDay <= month.dayCount) {
    std::snprintf(number, sizeof(number), "%u",
                  static_cast<unsigned int>(month.highlightedDay));
    x += text::drawText(canvas, number, x, NUMBER_TOP, Color4::Red,
                        text::Font::Title25);
    text::drawText(canvas, "日", x + 2, UNIT_TOP);
  }

  text::drawText(canvas, month.lunarYear, META_X, 12);
  text::drawText(canvas, month.lunarDate, META_X, 33);
}

void drawWeekHeader(EpdCanvas& canvas) {
  canvas.fillRect(GRID_X, HEADER_Y, GRID_WIDTH, HEADER_HEIGHT, BLACK);
  canvas.fillRect(GRID_X, HEADER_Y, CELL_WIDTH, HEADER_HEIGHT, RED);
  canvas.fillRect(GRID_X + CELL_WIDTH * 6, HEADER_Y,
                  CELL_WIDTH, HEADER_HEIGHT, RED);
  for (uint8_t column = 0; column < 7; ++column) {
    text::drawCenteredText(canvas, WEEK_HEADERS[column],
                           GRID_X + column * CELL_WIDTH,
                           HEADER_Y + WEEKDAY_TEXT_OFFSET_Y,
                           CELL_WIDTH, Color4::White);
  }
}

void drawTodayOutline(EpdCanvas& canvas, int16_t left, int16_t top,
                       int16_t cellHeight) {
  // 外红内白形成两像素边线，内外圆角同步缩小以保持线宽。
  const int16_t verticalInset = cellHeight < 40 ? 0 : 1;
  const int16_t outlineTop = top + verticalInset;
  const int16_t outlineHeight = cellHeight - 2 * verticalInset;
  canvas.fillRoundRect(left + TODAY_INSET_X, outlineTop,
                       CELL_WIDTH - 2 * TODAY_INSET_X, outlineHeight,
                       TODAY_RADIUS, RED);
  canvas.fillRoundRect(left + TODAY_INSET_X + TODAY_STROKE,
                       outlineTop + TODAY_STROKE,
                       CELL_WIDTH - 2 * TODAY_INSET_X - 2 * TODAY_STROKE,
                       outlineHeight - 2 * TODAY_STROKE,
                       TODAY_RADIUS - TODAY_STROKE, WHITE);
}

void drawDayLabel(EpdCanvas& canvas, const calendar::DayLabel& label,
                   int16_t left, int16_t top) {
  if (label.text == nullptr || *label.text == '\0') return;

  constexpr text::Font FONT = text::Font::Chinese12;
  const int16_t width = text::textWidth(label.text, FONT);
  const int16_t x = left + (CELL_WIDTH - width) / 2;
  const int16_t textTop = top + DAY_LABEL_TEXT_OFFSET_Y;
  if (label.kind != calendar::DayLabelKind::Lunar) {
    // 底色顶部向上扩展两像素，底边、宽度和文字位置保持不变。
    canvas.fillRect(x - 3, textTop - 4, width + 6, 15, YELLOW);
  }
  // 农历、节气、节日文字统一黑色，黄色只作为特殊标签底色。
  text::drawText(canvas, label.text, x, textTop,
                 Color4::Black, FONT);
}

void drawDates(EpdCanvas& canvas, const calendar::MonthData& month) {
  const uint8_t rows = (month.firstWeekday + month.dayCount + 6) / 7;
  const int16_t cellHeight = (EPD_HEIGHT - GRID_Y - BOTTOM_MARGIN) / rows;
  // 六行时压缩内部留白，保留数字和农历字号及之前的中文下移修正。
  const int16_t numberOffset = rows == 6 ? 2 : 5;
  const int16_t lunarOffset = rows == 6 ? 19 : 25;

  for (uint8_t day = 1; day <= month.dayCount; ++day) {
    const uint8_t index = month.firstWeekday + day - 1;
    const uint8_t column = index % 7;
    const int16_t left = GRID_X + column * CELL_WIDTH;
    const int16_t top = GRID_Y + (index / 7) * cellHeight;
    const calendar::DayLabel* label =
        month.dayLabels != nullptr && day <= month.dayLabelCount
            ? &month.dayLabels[day - 1]
            : nullptr;
    const bool isHoliday =
        label != nullptr && label->kind == calendar::DayLabelKind::Holiday;
    const Color4 color =
        column == 0 || column == 6 || day == month.highlightedDay || isHoliday
            ? Color4::Red
            : Color4::Black;
    if (day == month.highlightedDay) {
      drawTodayOutline(canvas, left, top, cellHeight);
    }

    char number[3];
    std::snprintf(number, sizeof(number), "%u", static_cast<unsigned int>(day));
    text::drawCenteredText(canvas, number, left, top + numberOffset, CELL_WIDTH,
                           color, text::Font::Date17);
    if (label != nullptr) {
      drawDayLabel(canvas, *label, left, top + lunarOffset);
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

  drawTitle(canvas, month);
  drawWeekHeader(canvas);
  drawDates(canvas, month);
}

}  // namespace calendar
