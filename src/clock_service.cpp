#include "clock_service.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_sntp.h>

#include <atomic>
#include <cstdlib>

#include "network_time_config.h"
#include "wifi_connection.h"

namespace {

// 网络任务回调只设置原子标志，网页和串口操作均由主任务完成。
std::atomic<bool> receivedNetworkTime{false};
clock_service::SyncStatus status = clock_service::SyncStatus::Idle;
uint32_t stageStartedAt = 0;
bool keepPortal = false;
bool sntpStarted = false;
clock_service::TimeSource source = clock_service::TimeSource::None;

void configureTimezone() {
  setenv("TZ", NetworkTimeConfig::TIMEZONE, 1);
  tzset();
}

void onTimeSynchronized(struct timeval*) {
  receivedNetworkTime.store(true);
}

void stopSntp() {
  if (!sntpStarted) return;
  esp_sntp_stop();
  esp_sntp_set_time_sync_notification_cb(nullptr);
  sntpStarted = false;
}

void finishSync(clock_service::SyncStatus result) {
  stopSntp();
  wifi_connection::end();
  if (result == clock_service::SyncStatus::Success) {
    source = clock_service::TimeSource::Network;
  }
  status = result;
  if (!keepPortal) {
    clock_service::stopNetwork();
  } else if (result != clock_service::SyncStatus::Success) {
    // 失败后停止连接路由器，保留配网热点供用户修改配置。
    WiFi.disconnect(false, false);
  }
}

void startSntp() {
  receivedNetworkTime.store(false);
  esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
  esp_sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
  esp_sntp_set_time_sync_notification_cb(onTimeSynchronized);
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, NetworkTimeConfig::NTP_SERVER_1);
  esp_sntp_setservername(1, NetworkTimeConfig::NTP_SERVER_2);
  esp_sntp_setservername(2, NetworkTimeConfig::NTP_SERVER_3);
  stageStartedAt = millis();
  sntpStarted = true;
  esp_sntp_init();
  status = clock_service::SyncStatus::WaitingForNtp;
}

}  // namespace

namespace clock_service {

void begin(bool retainedTimeValid) {
  configureTimezone();
  source = retainedTimeValid ? TimeSource::Retained : TimeSource::None;
}

TimeSource timeSource() {
  return source;
}

bool isSyncing() {
  return status == SyncStatus::Connecting || status == SyncStatus::WaitingForNtp;
}

SyncStatus syncStatus() {
  return status;
}

bool beginNetworkTime(const char* ssid, const char* password, bool keepAccessPoint) {
  if (isSyncing()) return false;
  configureTimezone();
  stopSntp();
  receivedNetworkTime.store(false);
  keepPortal = keepAccessPoint;
  if (ssid == nullptr || *ssid == '\0') {
    finishSync(SyncStatus::NotConfigured);
    return false;
  }

  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  if (!WiFi.mode(keepPortal ? WIFI_AP_STA : WIFI_STA)) {
    finishSync(SyncStatus::WifiStartFailed);
    return false;
  }
  Serial.println("[clock] Connecting to Wi-Fi...");
  status = SyncStatus::Connecting;
  if (!wifi_connection::begin(ssid, password)) {
    finishSync(SyncStatus::WifiStartFailed);
    return false;
  }
  return true;
}

void pollNetworkTime() {
  if (status == SyncStatus::Connecting) {
    const wifi_connection::State connection = wifi_connection::poll();
    if (connection == wifi_connection::State::Connected) {
      Serial.println("[clock] Wi-Fi connected. Requesting NTP time...");
      startSntp();
    } else if (connection == wifi_connection::State::TimedOut) {
      Serial.println("[clock] Wi-Fi connection timeout.");
      finishSync(SyncStatus::WifiTimeout);
    }
  } else if (status == SyncStatus::WaitingForNtp) {
    // 必须收到本次 SNTP 通知，不能只判断系统里是否已有日期。
    if (receivedNetworkTime.load()) {
      Serial.println("[clock] NTP synchronized.");
      finishSync(SyncStatus::Success);
    } else if (millis() - stageStartedAt >= NetworkTimeConfig::NTP_TIMEOUT_MS) {
      Serial.println("[clock] NTP timeout; existing system time was not cleared.");
      finishSync(SyncStatus::NtpTimeout);
    }
  }
}

bool stopNetwork() {
  stopSntp();
  wifi_connection::end();
  if (isSyncing()) status = SyncStatus::Idle;
  WiFi.setAutoReconnect(false);
  if (WiFi.getMode() != WIFI_OFF) WiFi.disconnect(false, false);
  const bool off = WiFi.mode(WIFI_OFF);
  Serial.println(off ? "[clock] Wi-Fi OFF."
                     : "[clock] ERROR: failed to turn Wi-Fi off.");
  return off;
}

bool readLocalTime(struct tm& result) {
  // 配网页面在尚未联网时也可以读取此前保留的系统时间。
  if (source == TimeSource::None) return false;
  configureTimezone();
  const time_t now = time(nullptr);
  constexpr time_t MIN_VALID_TIME = 1704067200;  // 2024-01-01 UTC。
  return now >= MIN_VALID_TIME && localtime_r(&now, &result) != nullptr;
}

}  // namespace clock_service
