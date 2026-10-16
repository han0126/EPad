#pragma once

#include <Arduino.h>

#include "weather_data.h"

namespace weather_settings { struct Settings; }

namespace weather_cities {

constexpr size_t MAX_CITIES = 10;
constexpr size_t NAME_BYTES = 64;
constexpr size_t ID_BYTES = 24;
constexpr uint32_t CACHE_MS = 10 * 60 * 1000UL;

struct City {
  char id[ID_BYTES];
  char name[NAME_BYTES];
  char province[NAME_BYTES];
  int16_t latitude100;
  int16_t longitude100;
};

struct Result {
  weather_data::Error error;
  int16_t httpStatus;
  uint16_t apiCode;
  uint8_t count;
  City cities[MAX_CITIES];
  uint8_t sourceCount;
  char sources[weather_data::MAX_ATTRIBUTIONS][weather_data::ATTRIBUTION_BYTES];
};

enum class State : uint8_t { Idle, Searching, Ready, Error };

// 只由网页显式发起；搜索结果不修改天气配置，也不消费每日天气次数。
bool start(const String& query, const String& province, const weather_settings::Settings& credentials);
void handle();
bool isBusy();
State state();
uint32_t revision();
const Result& result();
// 只允许保存本次候选中的坐标，或由网页保留已有的已保存城市。
bool find(const String& id, uint32_t searchRevision, int16_t latitude, int16_t longitude, City& city);

}  // namespace weather_cities
