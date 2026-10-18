#pragma once

namespace wifi_connection {

enum class State { Connecting, Connected, TimedOut };

// 调用前由校时服务开启 STA；缓存快连失败后仅回退一次普通扫描。
bool begin(const char* ssid, const char* password);
State poll();
// 只清除本次连接使用的凭据，不断开网络、不删除 RTC 快连缓存。
void end();
// 用户清除网络凭据时同步擦除 RTC 中的 SSID/BSSID 快连信息。
void forgetHint();

}  // namespace wifi_connection
