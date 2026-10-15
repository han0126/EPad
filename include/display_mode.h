#pragma once

#include <stdint.h>

namespace display_mode {

enum class Mode : uint8_t { Calendar, Image };

Mode selected();
bool isCalendar();
// 仅主循环切换模式；屏幕刷新期间拒绝切换。
bool select(Mode mode);
// 尚未成功显示图片时，关闭配置页恢复日历流程。
void cancelPendingImage();

}  // namespace display_mode
