#include "weather_display.h"

#include <cstdio>

#include "epd_canvas.h"
#include "text_renderer.h"
#include "weather_icons.h"

namespace weather_display {

Data placeholder() {
  return {100, 28, 18, true, Source::Placeholder};
}

Data unavailable() {
  return {999, 0, 0, false, Source::Unavailable};
}

bool sameData(const Data& left, const Data& right) {
  return left.code == right.code && left.maxTemperature == right.maxTemperature &&
      left.minTemperature == right.minTemperature && left.temperaturesValid == right.temperaturesValid &&
      left.source == right.source;
}

const char* sourceName(Source source) {
  switch (source) {
    case Source::Preview: return "preview";
    case Source::Network: return "network";
    case Source::Unavailable: return "unavailable";
    case Source::Disabled: return "disabled";
    default: return "placeholder";
  }
}

bool isValidPreview(const Data& data) {
  return data.temperaturesValid && data.minTemperature >= -99 &&
      data.maxTemperature <= 99 && data.minTemperature <= data.maxTemperature;
}

void draw(EpdCanvas& canvas, const Data& data, const char* regionName) {
  constexpr int16_t TEMPERATURE_X = 342;
  // 最长天气标签限制为四个汉字，避开左侧农历和右侧地区。
  weather_icons::draw(canvas, data.code, 294, 4);
  const char* description = data.source == Source::Disabled ? "未启用" :
      (data.source == Source::Unavailable ? "待更新" : weather_icons::condition(data.code).shortText);
  text::drawCenteredText(canvas, description,
                         280, 37, 58, Color4::Black, text::Font::Chinese12);

  char temperature[20];
  if (data.temperaturesValid) {
    std::snprintf(temperature, sizeof(temperature), "高 %d℃", data.maxTemperature);
  } else {
    std::snprintf(temperature, sizeof(temperature), "高 --℃");
  }
  text::drawText(canvas, temperature, TEMPERATURE_X, 10,
                 Color4::Red, text::Font::Chinese12);
  if (data.temperaturesValid) {
    std::snprintf(temperature, sizeof(temperature), "低 %d℃", data.minTemperature);
  } else {
    std::snprintf(temperature, sizeof(temperature), "低 --℃");
  }
  text::drawText(canvas, temperature, TEMPERATURE_X, 25,
                 Color4::Black, text::Font::Chinese12);
  String city = regionName != nullptr && *regionName != '\0' ? regionName : "未设置";
  // 长城市名只裁掉末尾完整字符，不改变天气区布局或截断 UTF-8 编码。
  while (city.length() != 0 && text::textWidth(city.c_str(), text::Font::Chinese12) > 56) {
    size_t end = city.length() - 1;
    while (end > 0 && (static_cast<uint8_t>(city[end]) & 0xC0) == 0x80) --end;
    city.remove(end);
  }
  text::drawCenteredText(canvas, city.c_str(),
                         340, 41, 56, Color4::Black, text::Font::Chinese12);
}

}  // namespace weather_display
