#pragma once

#include <stdint.h>

namespace calendar {

// 应用支持范围，超出范围拒绝生成，避免显示未经约定的日期。
constexpr uint16_t MIN_YEAR = 1901;
constexpr uint16_t MAX_YEAR = 2099;

struct Date {
  uint16_t year;
  uint8_t month;
  uint8_t day;
};

enum class DayLabelKind : uint8_t {
  Lunar,
  SolarTerm,
  Holiday,
};

// 节气和节日使用黄色底色，节日的公历数字另外标红。
struct DayLabel {
  const char* text;
  DayLabelKind kind;
};

// 一个月的显示数据，不负责联网、计时或农历换算。
struct MonthData {
  uint16_t year;
  uint8_t month;
  uint8_t dayCount;
  uint8_t firstWeekday;  // 当月 1 日的位置：周日=0，周六=6。
  uint8_t highlightedDay;  // 当前或预览中需要强调的日期，0 表示不强调。
  const char* lunarYear;
  const char* lunarDate;
  const DayLabel* dayLabels;
  uint8_t dayLabelCount;
};

bool isValidDate(const Date& date);
bool sameDate(const Date& left, const Date& right);

// 仅由显示工作任务调用；结果文字由模块持有，下一次生成时更新。
bool buildMonth(const Date& date, MonthData& result,
                 void (*yieldHook)() = nullptr);

}  // namespace calendar
