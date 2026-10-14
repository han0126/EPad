#include "calendar_data.h"

namespace {

// 初一至三十为字库示例，不代表下方公历日期的真实农历。
const char* const LUNAR_DAY_SAMPLES[] = {
    "初一", "初二", "初三", "初四", "初五", "初六", "初七", "初八",
    "初九", "初十", "十一", "十二", "十三", "十四", "十五", "十六",
    "十七", "十八", "十九", "二十", "廿一", "廿二", "廿三", "廿四",
    "廿五", "廿六", "廿七", "廿八", "廿九", "三十"};

constexpr uint8_t LUNAR_SAMPLE_COUNT =
    sizeof(LUNAR_DAY_SAMPLES) / sizeof(LUNAR_DAY_SAMPLES[0]);

const calendar::MonthData DEMO_MONTH = {
    2025, 6, 30, 0, 18, LUNAR_DAY_SAMPLES, LUNAR_SAMPLE_COUNT};

}  // namespace

namespace calendar {

const MonthData& demoMonth() {
  return DEMO_MONTH;
}

}  // namespace calendar
