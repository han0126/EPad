#pragma once

#include "calendar_data.h"

namespace lunar_calendar {

struct DayInfo {
  uint16_t year;
  uint8_t month;
  uint8_t day;
  bool leap;
  bool newYearsEve;
  int8_t solarTerm;  // -1 表示无节气，0 为冬至。
};

struct MonthInfo {
  uint8_t dayCount;
  uint8_t firstWeekday;
  DayInfo days[31];
};

uint8_t daysInMonth(uint16_t year, uint8_t month);
bool convertMonth(uint16_t year, uint8_t month, MonthInfo& result,
                   void (*yieldHook)() = nullptr);
const char* solarTermName(uint8_t index);

}  // namespace lunar_calendar
