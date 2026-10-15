#pragma once

#include <stddef.h>

#include "calendar_data.h"

// 日历和图片共用显示任务，统一排队并串行操作画布及屏幕。
namespace display_service {

enum class State : uint8_t { WaitingForTime, Queued, Drawing, Ready, Error };
enum class Error : uint8_t {
  None, TaskStartFailed, CalendarFailed, PanelFailed, InvalidFrame
};

struct Status {
  State state;
  Error error;
  calendar::Date displayedDate;
  calendar::Date requestedDate;
  bool preview;
  uint8_t rows;
  bool image;         // 最后一次成功显示的画面是否为图片。
  bool imageRequest;  // 当前或最后一次刷新请求是否为图片。
};

bool begin();
void handle();
Status status();
bool isBusy();

// 只恢复深睡前成功显示的画面记录，不传输像素、不刷新墨水屏。
bool restoreRetainedFrame(const calendar::Date& date, uint8_t rows);

// 仅修改屏幕目标日期，不修改系统时钟；不保存预览模式到 Flash。
bool requestPreview(const calendar::Date& date);
bool requestCurrentDate();

// 数据由上传模块持有，在 isBusy() 变为 false 前不得修改或释放。
bool requestImage(const uint8_t* frame, size_t length);

}  // namespace display_service
