#pragma once

#include <stdint.h>

class EpdCanvas;

namespace weather_display {

enum class Source : uint8_t { Placeholder, Preview, Network, Unavailable, Disabled };

struct Data {
  uint16_t code;
  int16_t maxTemperature;
  int16_t minTemperature;
  bool temperaturesValid;
  Source source;
};

Data placeholder();
Data unavailable();
bool sameData(const Data& left, const Data& right);
const char* sourceName(Source source);
bool isValidPreview(const Data& data);
void draw(EpdCanvas& canvas, const Data& data, const char* regionName);

}  // namespace weather_display
