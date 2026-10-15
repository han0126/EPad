#pragma once

#include <stdint.h>

#if __has_include("wifi_credentials.h")
#include "wifi_credentials.h"
#endif

#ifndef CALENDAR_WIFI_SSID
#define CALENDAR_WIFI_SSID ""
#endif

#ifndef CALENDAR_WIFI_PASSWORD
#define CALENDAR_WIFI_PASSWORD ""
#endif

namespace NetworkTimeConfig {

constexpr char SSID[] = CALENDAR_WIFI_SSID;
constexpr char PASSWORD[] = CALENDAR_WIFI_PASSWORD;

// POSIX 时区符号与 UTC 偏移相反：CST-8 表示北京时间 UTC+8。
constexpr char TIMEZONE[] = "CST-8";
constexpr char NTP_SERVER_1[] = "ntp.aliyun.com";
constexpr char NTP_SERVER_2[] = "ntp.tencent.com";
constexpr char NTP_SERVER_3[] = "time.cloudflare.com";

// 快连未关联时三秒回退；普通连接仍等待八秒，NTP 仍等待七秒。
// 快连已关联但 DHCP 超时的最坏等待约 8+8+7 秒，不含接口耗时。
constexpr uint32_t FAST_CONNECT_TIMEOUT_MS = 3000;
constexpr uint32_t WIFI_TIMEOUT_MS = 8000;
constexpr uint32_t NTP_TIMEOUT_MS = 7000;

// 复位时提供限时配网页面，每日定时唤醒不开启热点。
constexpr char PORTAL_SSID[] = "EPD-Calendar";
constexpr char PORTAL_PASSWORD[] = "calendar1234";
constexpr uint32_t PORTAL_TIMEOUT_MS = 5UL * 60 * 1000;
constexpr uint32_t PORTAL_SUCCESS_VIEW_MS = 2UL * 60 * 1000;
// 图片待上传和失败重试使用此窗口；成功刷屏后会提前关闭。
constexpr uint32_t IMAGE_PORTAL_TIMEOUT_MS = 10UL * 60 * 1000;
constexpr uint32_t IMAGE_UPLOAD_TIMEOUT_MS = 20000;

}  // namespace NetworkTimeConfig
