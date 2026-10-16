#pragma once

#include "calendar_data.h"
#include "calendar_events.h"
#include "weather_display.h"

class EpdCanvas;

namespace calendar {

// 将月份数据绘制到画布；发送和刷新由主程序负责。
void drawMonth(EpdCanvas& canvas, const MonthData& month, const char* regionName = nullptr,
               const weather_display::Data& weather = weather_display::unavailable(),
               const calendar_events::DisplayData& events = calendar_events::DisplayData{});

}  // namespace calendar
