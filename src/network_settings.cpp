#include "network_settings.h"

#include <Preferences.h>
#include <cstring>

#include "network_time_config.h"

namespace {

// 使用单个 NVS 记录保存一组凭据，避免掉电时新旧名称、密码混用。
struct StoredNetwork {
  uint32_t version;
  char ssid[33];
  char password[65];
};

bool loadStored(network_settings::Credentials& credentials) {
  Preferences preferences;
  if (!preferences.begin("calendar-net", true)) return false;

  StoredNetwork record = {};
  const bool exists = preferences.isKey("wifi");
  const bool loaded = exists &&
      preferences.getBytesLength("wifi") == sizeof(record) &&
      preferences.getBytes("wifi", &record, sizeof(record)) == sizeof(record);
  preferences.end();
  if (!loaded || record.version != 1 || record.ssid[32] != '\0' ||
      record.password[64] != '\0') {
    return false;
  }

  credentials.ssid = record.ssid;
  credentials.password = record.password;
  return network_settings::isValid(credentials);
}

}  // namespace

namespace network_settings {

bool isValid(const Credentials& credentials) {
  if (credentials.ssid.length() == 0 || credentials.ssid.length() > 32) {
    return false;
  }
  const size_t length = credentials.password.length();
  if (length == 0 || (length >= 8 && length <= 63)) return true;
  if (length != 64) return false;
  // 64 字符密码仅允许十六进制原始 PSK。
  for (size_t i = 0; i < length; ++i) {
    const char c = credentials.password[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
          (c >= 'A' && c <= 'F'))) return false;
  }
  return true;
}

bool load(Credentials& credentials) {
  if (loadStored(credentials)) return true;
  credentials.ssid = NetworkTimeConfig::SSID;
  credentials.password = NetworkTimeConfig::PASSWORD;
  return isValid(credentials);
}

bool save(const Credentials& credentials) {
  if (!isValid(credentials)) return false;
  StoredNetwork record = {};
  record.version = 1;
  std::memcpy(record.ssid, credentials.ssid.c_str(), credentials.ssid.length());
  std::memcpy(record.password, credentials.password.c_str(),
              credentials.password.length());

  Preferences preferences;
  if (!preferences.begin("calendar-net", false)) return false;
  const bool saved =
      preferences.putBytes("wifi", &record, sizeof(record)) == sizeof(record);
  preferences.end();
  return saved;
}

}  // namespace network_settings
