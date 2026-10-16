#pragma once

#include <stddef.h>
#include <stdint.h>

#include "weather_display.h"

namespace weather_data {

constexpr size_t MAX_ATTRIBUTIONS = 4;
constexpr size_t ATTRIBUTION_BYTES = 192;

enum class Error : uint8_t {
  None, Connection, HttpStatus, TooLarge, Encoding, Json,
  InvalidForecast, InvalidAttribution, Memory, Storage, TaskStart, Interrupted
};

struct Forecast {
  uint32_t day;
  int16_t latitude100;
  int16_t longitude100;
  int64_t validFrom;
  int64_t validUntil;
  int64_t fetchedAt;
  weather_display::Data data;
  uint8_t attributionCount;
  char attributions[MAX_ATTRIBUTIONS][ATTRIBUTION_BYTES];
};

struct Result {
  Forecast forecast;
  Error error;
  int16_t httpStatus;
};

}  // namespace weather_data
