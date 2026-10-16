#include "weather_settings.h"

#include <Preferences.h>

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace {

constexpr char STORAGE_NAMESPACE[] = "epd-weather";
constexpr char SETTINGS_KEY[] = "settings";
constexpr uint32_t RECORD_VERSION = 1;
constexpr size_t HOST_CAPACITY = 128;
constexpr size_t KEY_CAPACITY = 129;

struct StoredSettings {
  uint32_t version;
  int16_t latitude100;
  int16_t longitude100;
  uint8_t enabled;
  char apiHost[HOST_CAPACITY];
  char apiKey[KEY_CAPACITY];
};

bool sameSettings(const weather_settings::Settings& left,
                  const weather_settings::Settings& right) {
  return left.enabled == right.enabled && left.apiHost == right.apiHost &&
      left.apiKey == right.apiKey && left.latitude100 == right.latitude100 &&
      left.longitude100 == right.longitude100;
}

}  // namespace

namespace weather_settings {

bool isValidHost(const String& host) {
  // 仅接受专属和风域名，不接受 URL、端口或任意外部地址。
  if (host.length() == 0 || host.length() >= HOST_CAPACITY ||
      !host.endsWith(".qweatherapi.com")) return false;
  size_t labelLength = 0;
  for (size_t i = 0; i < host.length(); ++i) {
    const char c = host[i];
    if (c == '.') {
      if (labelLength == 0 || host[i - 1] == '-') return false;
      labelLength = 0;
      continue;
    }
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    if ((labelLength == 0 && c == '-') || ++labelLength > 63) return false;
  }
  return labelLength > 0 && host[host.length() - 1] != '-';
}

bool isValidKey(const String& key) {
  if (key.length() == 0 || key.length() >= KEY_CAPACITY) return false;
  for (size_t i = 0; i < key.length(); ++i) {
    // 禁止控制字符和空格，避免密钥影响后续 HTTPS 请求头。
    const uint8_t c = static_cast<uint8_t>(key[i]);
    if (c < 33 || c > 126) return false;
  }
  return true;
}

bool isValid(const Settings& settings) {
  if (settings.latitude100 < -9000 || settings.latitude100 > 9000 ||
      settings.longitude100 < -18000 || settings.longitude100 > 18000) return false;
  if (settings.apiHost.length() != 0 && !isValidHost(settings.apiHost)) return false;
  if (settings.apiKey.length() != 0 && !isValidKey(settings.apiKey)) return false;
  return !settings.enabled ||
      (isValidHost(settings.apiHost) && isValidKey(settings.apiKey));
}

bool parseCoordinate(const String& value, int16_t limit100, int16_t& result) {
  if (value.length() == 0 || value.length() > 12 || limit100 <= 0 ||
      std::strlen(value.c_str()) != value.length()) return false;
  char* end = nullptr;
  errno = 0;
  const double parsed = std::strtod(value.c_str(), &end);
  if (end == value.c_str() || *end != '\0' || errno == ERANGE ||
      !std::isfinite(parsed)) return false;
  const double scaled = parsed * 100.0;
  if (scaled < -limit100 || scaled > limit100) return false;
  const double rounded = std::round(scaled);
  if (std::fabs(scaled - rounded) > 0.000001) return false;
  result = static_cast<int16_t>(rounded);
  return true;
}

const char* regionName(const Settings& settings) {
  struct Region {
    int16_t latitude100;
    int16_t longitude100;
    const char* name;
  };
  static const Region regions[] = {
      {3991, 11641, "北京"}, {3123, 12147, "上海"},
      {3913, 11719, "天津"}, {2956, 10655, "重庆"},
      {2313, 11328, "广州"}, {2255, 11409, "深圳"},
      {3057, 10407, "成都"}, {3025, 12021, "杭州"}};
  for (const Region& region : regions) {
    if (region.latitude100 == settings.latitude100 &&
        region.longitude100 == settings.longitude100) return region.name;
  }
  return "自定义";
}

bool load(Settings& settings) {
  settings = Settings{};
  Preferences preferences;
  if (!preferences.begin(STORAGE_NAMESPACE, true)) return false;
  StoredSettings record = {};
  const bool loaded = preferences.isKey(SETTINGS_KEY) &&
      preferences.getBytesLength(SETTINGS_KEY) == sizeof(record) &&
      preferences.getBytes(SETTINGS_KEY, &record, sizeof(record)) == sizeof(record);
  preferences.end();
  if (!loaded || record.version != RECORD_VERSION || record.enabled > 1 ||
      record.apiHost[HOST_CAPACITY - 1] != '\0' ||
      record.apiKey[KEY_CAPACITY - 1] != '\0') return false;

  Settings candidate;
  candidate.enabled = record.enabled != 0;
  candidate.apiHost = record.apiHost;
  candidate.apiKey = record.apiKey;
  candidate.latitude100 = record.latitude100;
  candidate.longitude100 = record.longitude100;
  if (!isValid(candidate)) return false;
  settings = candidate;
  return true;
}

bool save(const Settings& settings) {
  if (!isValid(settings)) return false;
  Settings previous;
  if (load(previous) && sameSettings(previous, settings)) return true;

  StoredSettings record = {};
  record.version = RECORD_VERSION;
  record.enabled = settings.enabled ? 1 : 0;
  record.latitude100 = settings.latitude100;
  record.longitude100 = settings.longitude100;
  if (settings.apiHost.length() != 0) {
    std::memcpy(record.apiHost, settings.apiHost.c_str(), settings.apiHost.length());
  }
  if (settings.apiKey.length() != 0) {
    std::memcpy(record.apiKey, settings.apiKey.c_str(), settings.apiKey.length());
  }

  Preferences preferences;
  if (!preferences.begin(STORAGE_NAMESPACE, false)) return false;
  const bool saved = preferences.putBytes(SETTINGS_KEY, &record, sizeof(record)) == sizeof(record);
  preferences.end();
  return saved;
}

}  // namespace weather_settings
