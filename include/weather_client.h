#pragma once

#include "weather_data.h"

namespace weather_settings { struct Settings; }

namespace weather_client {

// 必须在专用任务中调用；只发送一次 GET，不跟随重定向、不认证重试。
weather_data::Result fetch(const weather_settings::Settings& settings, uint32_t day);

}  // namespace weather_client
