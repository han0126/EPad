#pragma once

#include <stdint.h>

namespace display_mode {

enum class Mode : uint8_t { Calendar, Image };

// 上电先读取 NVS，避免默认日历覆盖原照片。
void begin();
Mode selected();
bool isCalendar();
bool hasRetainedImage();
bool storageFailed();
// 仅主循环切换模式；屏幕刷新期间拒绝切换。
bool select(Mode mode);
// 仅在照片成功刷新后记住图片模式，不保存像素数据。
bool saveDisplayedImage();
// 未成功上传时保留已记住的模式，首次选图则恢复日历。
void cancelPendingImage();

}  // namespace display_mode
