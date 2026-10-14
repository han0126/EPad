#pragma once

#include <stdint.h>

namespace calendar {

// 一个月的显示数据，不负责联网、计时或农历换算。
struct MonthData {
  uint16_t year;
  uint8_t month;
  uint8_t dayCount;
  uint8_t firstWeekday;  // 当月 1 日的位置：周日=0，周六=6。
  uint8_t highlightedDay;  // 需要标红的日期，0 表示不标红。
  const char* const* lunarLabels;
  uint8_t lunarLabelCount;
};

// 返回固定演示数据，农历标签仅用于字体展示。
const MonthData& demoMonth();

}  // namespace calendar
