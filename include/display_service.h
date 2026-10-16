#pragma once

#include <stddef.h>

#include "calendar_data.h"
#include "weather_display.h"

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
  weather_display::Data weather;
  bool weatherPreview;
};

bool begin();
void handle();
Status status();
bool isAvailable();
bool isBusy();

// 只恢复深睡前成功显示的画面记录，不传输像素、不刷新墨水屏。
bool restoreRetainedFrame(const calendar::Date& date, uint8_t rows,
                           const weather_display::Data& weather = weather_display::unavailable());

// 仅修改屏幕目标日期，不修改系统时钟；不保存预览模式到 Flash。
bool requestPreview(const calendar::Date& date);
// 地区等非日期内容改变时，可强制重绘同一天；普通校时仍避免重复刷新。
bool requestCurrentDate(bool forceRefresh = false);
// 仅在 RAM 中预览天气，保持当前日期或指定日期预览，不联网、不写 NVS。
bool requestWeatherPreview(const weather_display::Data& weather);
// 退出本地预览，恢复该日期的真实缓存或“待更新”，不额外请求网络。
bool clearWeatherPreview();
// 地区或启用状态改变时延后重绘，等待正在获取的天气结束后只刷新一次。
void requestWeatherRedraw();

// 数据由上传模块持有，在 isBusy() 变为 false 前不得修改或释放。
bool requestImage(const uint8_t* frame, size_t length);

}  // namespace display_service
