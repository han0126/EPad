#pragma once

#include "calendar_data.h"

class EpdCanvas;

namespace calendar {

// 将月份数据绘制到画布；发送和刷新由主程序负责。
void drawMonth(EpdCanvas& canvas, const MonthData& month);

}  // namespace calendar
