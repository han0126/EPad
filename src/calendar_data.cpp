#include "calendar_data.h"

#include <cstdio>

#include "lunar_calendar.h"

namespace {

const char* const MONTH_NAMES[] = {
    "正", "二", "三", "四", "五", "六", "七", "八", "九", "十", "冬", "腊"};
const char* const DAY_NAMES[] = {
    "初一", "初二", "初三", "初四", "初五", "初六", "初七", "初八",
    "初九", "初十", "十一", "十二", "十三", "十四", "十五", "十六",
    "十七", "十八", "十九", "二十", "廿一", "廿二", "廿三", "廿四",
    "廿五", "廿六", "廿七", "廿八", "廿九", "三十"};
const char* const STEMS[] = {"甲", "乙", "丙", "丁", "戊", "己", "庚", "辛", "壬", "癸"};
const char* const BRANCHES[] = {"子", "丑", "寅", "卯", "辰", "巳", "午", "未", "申", "酉", "戌", "亥"};
const char* const ANIMALS[] = {"鼠", "牛", "虎", "兔", "龙", "蛇", "马", "羊", "猴", "鸡", "狗", "猪"};

struct Festival {
  uint8_t month;
  uint8_t day;
  const char* name;
};

const Festival SOLAR_FESTIVALS[] = {
    {1, 1, "元旦"}, {2, 14, "情人节"}, {3, 8, "妇女节"}, {3, 12, "植树节"},
    {5, 1, "劳动节"}, {5, 4, "青年节"}, {6, 1, "儿童节"}, {7, 1, "建党节"},
    {8, 1, "建军节"}, {9, 10, "教师节"}, {10, 1, "国庆节"},
    {12, 24, "平安夜"}, {12, 25, "圣诞节"}};
const Festival LUNAR_FESTIVALS[] = {
    {1, 1, "春节"}, {1, 15, "元宵节"}, {5, 5, "端午节"},
    {7, 7, "七夕节"}, {8, 15, "中秋节"}, {9, 9, "重阳节"}, {12, 8, "腊八节"}};

// 生成后的字符串长期有效，仅由屏幕工作任务访问。
char labelText[31][16];
calendar::DayLabel labels[31];
char lunarYearText[24];
char lunarDateText[32];

template <size_t Count>
const char* festivalName(const Festival (&festivals)[Count], uint8_t month,
                         uint8_t day) {
  for (const Festival& festival : festivals) {
    if (festival.month == month && festival.day == day) return festival.name;
  }
  return nullptr;
}

}  // namespace

namespace calendar {

bool isValidDate(const Date& date) {
  return date.year >= MIN_YEAR && date.year <= MAX_YEAR && date.day >= 1 &&
      date.day <= lunar_calendar::daysInMonth(date.year, date.month);
}

bool sameDate(const Date& left, const Date& right) {
  return left.year == right.year && left.month == right.month && left.day == right.day;
}

bool buildMonth(const Date& date, MonthData& result, void (*yieldHook)()) {
  if (!isValidDate(date)) return false;
  lunar_calendar::MonthInfo lunar = {};
  if (!lunar_calendar::convertMonth(date.year, date.month, lunar, yieldHook)) {
    return false;
  }

  for (uint8_t i = 0; i < lunar.dayCount; ++i) {
    const lunar_calendar::DayInfo& day = lunar.days[i];
    const char* special = nullptr;
    // 优先级：农历节日、公历节日、节气、普通农历；闰月不重复过节。
    if (!day.leap) {
      special = day.newYearsEve ? "除夕" :
          festivalName(LUNAR_FESTIVALS, day.month, day.day);
    }
    if (special == nullptr) {
      special = festivalName(SOLAR_FESTIVALS, date.month, i + 1);
    }
    labels[i].kind = DayLabelKind::Lunar;
    if (special != nullptr) {
      labels[i].kind = DayLabelKind::Holiday;
      std::snprintf(labelText[i], sizeof(labelText[i]), "%s", special);
    } else if (day.solarTerm >= 0) {
      labels[i].kind = DayLabelKind::SolarTerm;
      std::snprintf(labelText[i], sizeof(labelText[i]), "%s",
                    lunar_calendar::solarTermName(day.solarTerm));
    } else if (day.day == 1) {
      std::snprintf(labelText[i], sizeof(labelText[i]), "%s%s月",
                    day.leap ? "闰" : "", MONTH_NAMES[day.month - 1]);
    } else {
      std::snprintf(labelText[i], sizeof(labelText[i]), "%s", DAY_NAMES[day.day - 1]);
    }
    labels[i].text = labelText[i];
  }

  const lunar_calendar::DayInfo& today = lunar.days[date.day - 1];
  const uint16_t cycleYear = today.year - 4;
  std::snprintf(lunarYearText, sizeof(lunarYearText), "%s%s年 %s",
                STEMS[cycleYear % 10], BRANCHES[cycleYear % 12], ANIMALS[cycleYear % 12]);
  std::snprintf(lunarDateText, sizeof(lunarDateText), "%s%s月%s",
                today.leap ? "闰" : "", MONTH_NAMES[today.month - 1],
                DAY_NAMES[today.day - 1]);
  result = {date.year, date.month, lunar.dayCount, lunar.firstWeekday, date.day,
            lunarYearText, lunarDateText, labels, lunar.dayCount};
  return true;
}

}  // namespace calendar
