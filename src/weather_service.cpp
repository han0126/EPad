#include "weather_service.h"

#include <Arduino.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>

#include "clock_service.h"
#include "display_mode.h"
#include "weather_cache.h"
#include "weather_client.h"
#include "weather_config.h"
#include "weather_settings.h"
#include "wifi_portal.h"

namespace {

weather_settings::Settings settings;
weather_cache::Record cache = {};
bool storageReady = false;
bool storageFailed = false;
bool active = false;
bool fetchedThisBoot = false;
std::atomic<bool> resultReady{false};
weather_data::Result completed = {};

struct Job {
  weather_settings::Settings settings;
  uint32_t day;
};

Job job;

uint32_t dayNumber(const calendar::Date& date) {
  return static_cast<uint32_t>(date.year) * 10000 + date.month * 100 + date.day;
}

bool currentDate(calendar::Date& date) {
  struct tm local = {};
  if (!clock_service::readLocalTime(local)) return false;
  date = {static_cast<uint16_t>(local.tm_year + 1900),
      static_cast<uint8_t>(local.tm_mon + 1), static_cast<uint8_t>(local.tm_mday)};
  return calendar::isValidDate(date);
}

bool forecastMatches(const calendar::Date& date) {
  if (!settings.enabled || !cache.forecastValid) return false;
  calendar::Date today = {};
  if (!currentDate(today) || !calendar::sameDate(today, date)) return false;
  const weather_data::Forecast& forecast = cache.forecast;
  const int64_t now = static_cast<int64_t>(time(nullptr));
  return forecast.day == dayNumber(date) && forecast.latitude100 == settings.latitude100 &&
      forecast.longitude100 == settings.longitude100 &&
      now >= forecast.validFrom && now < forecast.validUntil;
}

void fetchTask(void*) {
  completed = weather_client::fetch(job.settings, job.day);
  job.settings.apiKey = "";
  // 结果写完后再通知主任务，缓存和显示状态只由主任务修改。
  resultReady.store(true, std::memory_order_release);
  vTaskDelete(nullptr);
}

void tryStart() {
  if (active || !settings.enabled || !display_mode::isCalendar() || !storageReady || storageFailed) return;
  calendar::Date today = {};
  if (!currentDate(today) || clock_service::timeSource() != clock_service::TimeSource::Network ||
      clock_service::syncStatus() != clock_service::SyncStatus::Success || WiFi.status() != WL_CONNECTED) return;
  const uint32_t day = dayNumber(today);
  // 高水位也阻止时钟回拨后重复消耗已过去日期的次数。
  if (day <= cache.lastAttemptDay) {
    Serial.println("[weather] Daily limit retained; no additional API request.");
    return;
  }
  cache.lastAttemptDay = day;
  cache.lastHttpStatus = 0;
  cache.lastError = weather_data::Error::Interrupted;
  // 必须先提交 NVS，再启动网络任务；任何掉电位置都不会造成同日第二次 GET。
  if (!weather_cache::save(cache)) {
    storageFailed = true;
    Serial.println("[weather] Cannot reserve daily attempt in NVS; no API request sent.");
    return;
  }
  job.settings = settings;
  job.day = day;
  resultReady.store(false, std::memory_order_relaxed);
  active = true;
  Serial.printf("[weather] Daily attempt reserved for %lu; one forecast request.\n", static_cast<unsigned long>(day));
  if (xTaskCreatePinnedToCore(fetchTask, "weather-fetch", WeatherConfig::TASK_STACK_BYTES,
                              nullptr, 1, nullptr, 0) != pdPASS) {
    active = false;
    job.settings.apiKey = "";
    cache.lastError = weather_data::Error::TaskStart;
    storageFailed = !weather_cache::save(cache);
    Serial.println("[weather] Task start failed; today's attempt will not be repeated.");
  }
}

}  // namespace

namespace weather_service {

void begin() {
  weather_settings::load(settings);
  storageReady = weather_cache::load(cache) != weather_cache::LoadResult::Error;
  storageFailed = !storageReady;
  if (!storageReady) Serial.println("[weather] Daily record unreadable; API requests blocked to protect daily limit.");
}

bool isBusy() {
  return active;
}

void onTimeSynchronized() {
  tryStart();
}

void settingsChanged() {
  if (active) return;
  weather_settings::load(settings);
  // 初次配置可以复用刚校时成功的连接；已尝试过的日期仍不能重新请求。
  tryStart();
}

void handle() {
  if (!active || !resultReady.load(std::memory_order_acquire)) return;
  cache.lastError = completed.error;
  cache.lastHttpStatus = completed.httpStatus;
  if (completed.error == weather_data::Error::None) {
    cache.forecast = completed.forecast;
    cache.forecastValid = 1;
    fetchedThisBoot = true;
  }
  storageFailed = !weather_cache::save(cache);
  Serial.printf("[weather] Finished: HTTP %d, %s%s; no further attempt today.\n",
      static_cast<int>(completed.httpStatus), errorName(completed.error),
      storageFailed ? ", cache save failed" : "");
  resultReady.store(false, std::memory_order_relaxed);
  active = false;
  // 普通定时唤醒立即关无线；配网页面仍按原倒计时保留热点。
  if (!wifi_portal::isActive() && !clock_service::isSyncing()) clock_service::stopNetwork();
}

weather_display::Data dataForDate(const calendar::Date& date) {
  if (!settings.enabled) {
    weather_display::Data data = weather_display::unavailable();
    data.source = weather_display::Source::Disabled;
    return data;
  }
  return forecastMatches(date) ? cache.forecast.data : weather_display::unavailable();
}

Status status() {
  Status summary = {State::WaitingForTime, cache.lastError, cache.lastHttpStatus,
      cache.lastAttemptDay, cache.forecastValid ? cache.forecast.day : uint32_t{0}};
  if (!settings.enabled) { summary.state = State::Disabled; summary.error = weather_data::Error::None; return summary; }
  if (active) { summary.state = State::Fetching; summary.error = weather_data::Error::None; return summary; }
  if (!storageReady || storageFailed) { summary.state = State::Error; summary.error = weather_data::Error::Storage; return summary; }
  calendar::Date today = {};
  if (!currentDate(today)) return summary;
  if (forecastMatches(today)) {
    summary.state = fetchedThisBoot ? State::Success : State::Cached;
    summary.error = weather_data::Error::None;
  } else if (dayNumber(today) <= cache.lastAttemptDay) {
    summary.state = cache.lastError == weather_data::Error::None ? State::DailyLimit : State::Error;
  } else {
    summary.state = State::WaitingForNetwork;
    summary.error = weather_data::Error::None;
    summary.httpStatus = 0;
  }
  return summary;
}

const char* stateName(State state) {
  switch (state) {
    case State::Disabled: return "disabled";
    case State::WaitingForTime: return "waiting_time";
    case State::WaitingForNetwork: return "waiting_network";
    case State::Fetching: return "fetching";
    case State::Success: return "success";
    case State::Cached: return "cached";
    case State::DailyLimit: return "daily_limit";
    default: return "error";
  }
}

const char* errorName(weather_data::Error error) {
  switch (error) {
    case weather_data::Error::None: return "none";
    case weather_data::Error::Connection: return "connection";
    case weather_data::Error::HttpStatus: return "http";
    case weather_data::Error::TooLarge: return "too_large";
    case weather_data::Error::Encoding: return "encoding";
    case weather_data::Error::Json: return "json";
    case weather_data::Error::InvalidForecast: return "forecast";
    case weather_data::Error::InvalidAttribution: return "attribution";
    case weather_data::Error::Memory: return "memory";
    case weather_data::Error::Storage: return "storage";
    case weather_data::Error::TaskStart: return "task";
    default: return "interrupted";
  }
}

size_t attributionCount() {
  // 保留最后一次返回的来源声明，不因换日或暂时断网丢失归因信息。
  return cache.forecastValid ? cache.forecast.attributionCount : 0;
}

const char* attribution(size_t index) {
  return index < attributionCount() ? cache.forecast.attributions[index] : "";
}

}  // namespace weather_service
