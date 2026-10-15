#include "wifi_connection.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_attr.h>
#include <esp_wifi.h>

#include <cstring>

#include "network_time_config.h"

namespace {

constexpr uint32_t HINT_MAGIC = 0x57494601;

struct AccessPointHint {
  uint32_t magic;
  char ssid[33];
  uint8_t bssid[6];
  uint8_t channel;
};

// 只保存接入点信息，不保存密码；跨深睡保留，断电后重新学习。
RTC_DATA_ATTR AccessPointHint hint = {};
char requestedSsid[33] = {};
char requestedPassword[65] = {};
bool active = false;
bool fastAttempt = false;
uint32_t attemptStartedAt = 0;
uint32_t connectionStartedAt = 0;

bool canUseHint() {
  if (hint.magic != HINT_MAGIC || hint.ssid[32] != '\0' ||
      hint.channel < 1 || hint.channel > 14 ||
      std::strcmp(hint.ssid, requestedSsid) != 0 || (hint.bssid[0] & 1) != 0) {
    return false;
  }
  uint8_t nonzero = 0;
  for (uint8_t value : hint.bssid) nonzero |= value;
  return nonzero != 0;
}

bool readAssociatedAccessPoint(wifi_ap_record_t& accessPoint) {
  return esp_wifi_sta_get_ap_info(&accessPoint) == ESP_OK &&
      std::strcmp(reinterpret_cast<const char*>(accessPoint.ssid), requestedSsid) == 0;
}

void rememberAccessPoint(const wifi_ap_record_t& accessPoint) {
  // 关联完成且已获得 IP 后更新，避免把尚未连接成功的扫描结果当成缓存。
  hint.magic = 0;
  std::memcpy(hint.ssid, requestedSsid, sizeof(hint.ssid));
  std::memcpy(hint.bssid, accessPoint.bssid, sizeof(hint.bssid));
  hint.channel = accessPoint.primary;
  hint.magic = HINT_MAGIC;
}

void startAttempt(bool fast) {
  fastAttempt = fast;
  WiFi.disconnect(false, false);
  attemptStartedAt = millis();
  if (fast) {
    Serial.printf("[wifi] Fast connection on cached channel %u.\n",
                  static_cast<unsigned>(hint.channel));
    WiFi.begin(requestedSsid, requestedPassword, hint.channel, hint.bssid);
  } else {
    // channel=0 且 BSSID=nullptr，解除旧接入点限制，兼容换信道和 Mesh。
    Serial.println("[wifi] Connecting with normal scan.");
    WiFi.begin(requestedSsid, requestedPassword, 0, nullptr);
  }
}

}  // namespace

namespace wifi_connection {

bool begin(const char* ssid, const char* password) {
  end();
  if (ssid == nullptr || *ssid == '\0' || std::strlen(ssid) > 32 ||
      (password != nullptr && std::strlen(password) > 64)) return false;
  std::memcpy(requestedSsid, ssid, std::strlen(ssid));
  if (password != nullptr) {
    std::memcpy(requestedPassword, password, std::strlen(password));
  }
  active = true;
  connectionStartedAt = millis();
  startAttempt(canUseHint());
  return true;
}

State poll() {
  if (!active) return State::TimedOut;
  wifi_ap_record_t accessPoint = {};
  const bool associated = readAssociatedAccessPoint(accessPoint);
  if (associated && WiFi.status() == WL_CONNECTED) {
    rememberAccessPoint(accessPoint);
    Serial.printf("[wifi] Connected in %lu ms (%s).\n",
                  static_cast<unsigned long>(millis() - connectionStartedAt),
                  fastAttempt ? "cached access point" : "normal scan");
    end();
    return State::Connected;
  }

  const uint32_t elapsed = millis() - attemptStartedAt;
  if (fastAttempt && elapsed >= NetworkTimeConfig::FAST_CONNECT_TIMEOUT_MS) {
    // 已关联时继续给 DHCP 原有的等待时间，不因快连预算短而误断开。
    if (associated && elapsed < NetworkTimeConfig::WIFI_TIMEOUT_MS) {
      return State::Connecting;
    }
    hint = {};
    Serial.println("[wifi] Cached connection unavailable; falling back once.");
    startAttempt(false);
    return State::Connecting;
  }
  if (!fastAttempt && elapsed >= NetworkTimeConfig::WIFI_TIMEOUT_MS) {
    end();
    return State::TimedOut;
  }
  return State::Connecting;
}

void end() {
  // 使用 volatile 写入，避免密码清理被编译器当作无用写入移除。
  volatile char* password = requestedPassword;
  for (size_t i = 0; i < sizeof(requestedPassword); ++i) password[i] = '\0';
  std::memset(requestedSsid, 0, sizeof(requestedSsid));
  active = false;
  fastAttempt = false;
}

}  // namespace wifi_connection
