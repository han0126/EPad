#include "lunar_calendar.h"

#include <sxtwl.h>
#include <memory>

namespace {

const char* const SOLAR_TERMS[] = {
    "冬至", "小寒", "大寒", "立春", "雨水", "惊蛰", "春分", "清明",
    "谷雨", "立夏", "小满", "芒种", "夏至", "小暑", "大暑", "立秋",
    "处暑", "白露", "秋分", "寒露", "霜降", "立冬", "小雪", "大雪"};

}  // namespace

namespace lunar_calendar {

uint8_t daysInMonth(uint16_t year, uint8_t month) {
  static constexpr uint8_t DAYS[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 0;
  const bool leapYear = year % 400 == 0 || (year % 4 == 0 && year % 100 != 0);
  return month == 2 && leapYear ? 29 : DAYS[month - 1];
}

bool convertMonth(uint16_t year, uint8_t month, MonthInfo& result,
                   void (*yieldHook)()) {
  if (year < calendar::MIN_YEAR || year > calendar::MAX_YEAR ||
      month < 1 || month > 12) return false;

  result.dayCount = daysInMonth(year, month);
  std::unique_ptr<Day> first(sxtwl::fromSolar(year, month, 1));
  if (!first) return false;
  result.firstWeekday = first->getWeek();

  for (uint8_t i = 0; i < result.dayCount; ++i) {
    std::unique_ptr<Day> day(first->after(i));
    if (!day) return false;
    DayInfo& info = result.days[i];
    // 农历年份以春节为界，不能直接用公历年替代。
    info.year = day->getLunarYear(true);
    info.month = day->getLunarMonth();
    info.day = day->getLunarDay();
    info.leap = day->isLunarLeap();
    info.solarTerm = day->hasJieQi() ? static_cast<int8_t>(day->getJieQi()) : -1;
    info.newYearsEve = false;
    if (info.month < 1 || info.month > 12 || info.day < 1 || info.day > 30 ||
        info.solarTerm < -1 || info.solarTerm > 23) return false;

    // 除夕可能是腊月廿九或三十，以次日是否春节判断。
    if (!info.leap && info.month == 12 && info.day >= 29) {
      std::unique_ptr<Day> tomorrow(day->after(1));
      if (!tomorrow) return false;
      info.newYearsEve = !tomorrow->isLunarLeap() &&
          tomorrow->getLunarMonth() == 1 && tomorrow->getLunarDay() == 1;
    }
    if (yieldHook != nullptr) yieldHook();
  }
  return true;
}

const char* solarTermName(uint8_t index) {
  return index < 24 ? SOLAR_TERMS[index] : "";
}

}  // namespace lunar_calendar
