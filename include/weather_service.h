#pragma once

#include "calendar_data.h"
#include "weather_data.h"

namespace weather_service {

enum class State : uint8_t {
  Disabled, WaitingForTime, WaitingForNetwork, Fetching, Success, Cached, DailyLimit, Error
};

struct Status {
  State state;
  weather_data::Error error;
  int16_t httpStatus;
  uint32_t attemptDay;
  uint32_t dataDay;
};

void begin();
void handle();
bool isBusy();
// 主任务在本次 NTP 成功后调用；只复用当前连接，不自行启动 Wi-Fi。
void onTimeSynchronized();
void settingsChanged();
weather_display::Data dataForDate(const calendar::Date& date);
Status status();
const char* stateName(State state);
const char* errorName(weather_data::Error error);
size_t attributionCount();
const char* attribution(size_t index);

}  // namespace weather_service
