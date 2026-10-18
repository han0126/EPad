#pragma once

#include <Arduino.h>
#include "secure_memory.h"

namespace network_settings {

struct Credentials {
  String ssid;
  String password;
  ~Credentials() { secure_memory::clear(password); }
};

bool isValid(const Credentials& credentials);
// 默认只读取设备 NVS；旧配置文件后备需显式启用兼容宏。
bool load(Credentials& credentials);
bool save(const Credentials& credentials);
// 保存禁用记录，清除后不再回退到旧版编译凭据。
bool clear();

}  // namespace network_settings
