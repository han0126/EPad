#pragma once

#include <time.h>
#include <stdint.h>

namespace power_service {

struct Status {
  bool timerWake;
  time_t nextWake;
  bool imageSleepPending;
  uint32_t sleepInSeconds;
};

// 在屏幕引脚和显示任务初始化后调用；复位进入配网，定时唤醒单次校时。
void begin();
void handle();
bool openConfiguration();
// 仅成功显示且屏幕已休眠的图片可进入此状态；一旦确认便不再接受换图。
bool requestImageSleep();
bool imageSleepPending();
Status status();

}  // namespace power_service
