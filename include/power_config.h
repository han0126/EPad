#pragma once

#include <stdint.h>

namespace PowerConfig {

// 零点后留出两秒余量；网络连接、校时和墨水屏刷新另需时间。
constexpr uint8_t MIDNIGHT_OFFSET_SECONDS = 2;
constexpr uint32_t MIN_AWAKE_MS = 1500;
// 图片成功后给网页三秒显示结果，随后关闭无线并取消所有自动唤醒。
constexpr uint32_t IMAGE_SLEEP_NOTICE_MS = 3000;

// 无有效时间或刷屏失败时限频重试，不在主循环反复联网或刷屏。
constexpr uint32_t NO_TIME_RETRY_SECONDS = 60UL * 60;
constexpr uint32_t DISPLAY_ERROR_RETRY_SECONDS = 60UL * 60;
constexpr uint32_t SLEEP_ERROR_RETRY_MS = 30000;

// 0：正常零点唤醒。实物评估时可暂改为 60，观察一分钟唤醒一次。
constexpr uint32_t WAKE_INTERVAL_OVERRIDE_SECONDS = 0;

}  // namespace PowerConfig
