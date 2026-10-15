#pragma once

#include <Arduino.h>

namespace network_settings {

struct Credentials {
  String ssid;
  String password;
};

bool isValid(const Credentials& credentials);
// 优先读取设备 NVS，旧版本地配置文件仅作为后备。
bool load(Credentials& credentials);
bool save(const Credentials& credentials);

}  // namespace network_settings
