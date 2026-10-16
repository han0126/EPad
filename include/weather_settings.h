#pragma once

#include <Arduino.h>

namespace weather_settings {

struct Settings {
  bool enabled = false;
  String apiHost;
  String apiKey;
  // 经纬度放大一百倍保存，对应和风接口的两位小数精度。
  int16_t latitude100 = 3991;
  int16_t longitude100 = 11641;
};

bool isValidHost(const String& host);
bool isValidKey(const String& key);
bool isValid(const Settings& settings);
bool parseCoordinate(const String& value, int16_t limit100, int16_t& result);
// 与网页预设坐标对应；自定义坐标尚无地名，使用简短占位名称。
const char* regionName(const Settings& settings);
// 未保存或记录无效时返回 false，并填入关闭天气的默认配置。
bool load(Settings& settings);
// 配置存为一条 NVS 记录，内容未变不重复写 Flash。
bool save(const Settings& settings);

}  // namespace weather_settings
