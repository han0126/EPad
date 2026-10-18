#include "network_settings.h"

#include <Preferences.h>
#include <cstring>

#include "network_time_config.h"
#include "secure_memory.h"

namespace {

// 使用单个 NVS 记录保存一组凭据，避免掉电时新旧名称、密码混用。
struct StoredNetwork {
  uint32_t version;
  char ssid[33];
  char password[65];
};

bool loadStored(network_settings::Credentials& credentials, bool& exists) {
  Preferences preferences;
  if (!preferences.begin("calendar-net", true)) return false;

  StoredNetwork record = {};
  exists = preferences.isKey("wifi");
  const bool loaded = exists &&
      preferences.getBytesLength("wifi") == sizeof(record) &&
      preferences.getBytes("wifi", &record, sizeof(record)) == sizeof(record);
  preferences.end();
  if (!loaded || record.version != 1 || record.ssid[32] != '\0' ||
      record.password[64] != '\0') {
    secure_memory::wipe(&record, sizeof(record));
    return false;
  }

  credentials.ssid = record.ssid;
  credentials.password = record.password;
  secure_memory::wipe(&record, sizeof(record));
  return network_settings::isValid(credentials);
}

}  // namespace

namespace network_settings {

bool isValid(const Credentials& credentials) {
  if (std::strlen(credentials.ssid.c_str()) != credentials.ssid.length() ||
      std::strlen(credentials.password.c_str()) != credentials.password.length()) return false;
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
  secure_memory::clear(credentials.password);
  credentials.ssid = "";
  bool exists = false;
  if (loadStored(credentials, exists)) return true;
  if (exists) return false;
#if CALENDAR_ENABLE_LEGACY_WIFI_CREDENTIALS
  credentials.ssid = NetworkTimeConfig::SSID;
  credentials.password = NetworkTimeConfig::PASSWORD;
  return isValid(credentials);
#else
  return false;
#endif
}

bool save(const Credentials& credentials) {
  if (!isValid(credentials)) return false;
  Credentials previous;
  bool exists = false;
  if (loadStored(previous, exists) && previous.ssid == credentials.ssid &&
      previous.password == credentials.password) return true;
  StoredNetwork record = {};
  record.version = 1;
  std::memcpy(record.ssid, credentials.ssid.c_str(), credentials.ssid.length());
  std::memcpy(record.password, credentials.password.c_str(),
              credentials.password.length());

  Preferences preferences;
  if (!preferences.begin("calendar-net", false)) {
    secure_memory::wipe(&record, sizeof(record));
    return false;
  }
  const bool saved =
      preferences.putBytes("wifi", &record, sizeof(record)) == sizeof(record);
  preferences.end();
  secure_memory::wipe(&record, sizeof(record));
  return saved;
}

bool clear() {
  // 单条原子写入空记录，阻止删除后再次启用旧配置文件中的网络。
  StoredNetwork record = {};
  record.version = 2;
  Preferences preferences;
  if (!preferences.begin("calendar-net", false)) return false;
  const bool saved = preferences.putBytes("wifi", &record, sizeof(record)) == sizeof(record);
  preferences.end();
  return saved;
}

}  // namespace network_settings
