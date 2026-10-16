#pragma once

#include <stdint.h>
#include <time.h>

namespace clock_service {

enum class TimeSource : uint8_t { None, Retained, Network };

enum class SyncStatus : uint8_t {
  Idle,
  Connecting,
  WaitingForNtp,
  Success,
  NotConfigured,
  WifiStartFailed,
  WifiTimeout,
  NtpTimeout,
};

// 仅深睡唤醒且 RTC 记录有效时，允许在本次联网失败后沿用保留时间。
void begin(bool retainedTimeValid);
TimeSource timeSource();

// 非阻塞校时，由 loop() 持续调用 pollNetworkTime() 推进。
// 配网时保留热点；普通校时成功后先复用连接完成当天天气，再关闭无线。
bool beginNetworkTime(const char* ssid, const char* password,
                      bool keepAccessPoint = false);
void pollNetworkTime();
bool isSyncing();
SyncStatus syncStatus();

// 停止 SNTP 并关闭 Wi-Fi；关闭失败时返回 false，由电源服务限频重试。
bool stopNetwork();

// 读取北京时间；没有有效系统时间时返回 false，不触发联网。
bool readLocalTime(struct tm& result);

}  // namespace clock_service
