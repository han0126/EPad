#pragma once

#include "weather_data.h"
#include "weather_cities.h"

namespace weather_settings { struct Settings; }

namespace weather_client {

// 必须在专用任务中调用；只发送一次 GET，不跟随重定向、不认证重试。
weather_data::Result fetch(const weather_settings::Settings& settings, uint32_t day);
weather_cities::Result searchCities(const weather_settings::Settings& settings,
                                   const String& query, const String& province);

}  // namespace weather_client
