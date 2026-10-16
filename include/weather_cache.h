#pragma once

#include "weather_data.h"

namespace weather_cache {

struct Record {
  uint32_t version;
  uint32_t lastAttemptDay;
  weather_data::Error lastError;
  int16_t lastHttpStatus;
  uint8_t forecastValid;
  weather_data::Forecast forecast;
};

enum class LoadResult : uint8_t { Empty, Loaded, Error };

// 读取失败和“从未保存”必须区分，避免损坏记录被误当成当天尚未请求。
LoadResult load(Record& record);
bool save(const Record& record);

}  // namespace weather_cache
