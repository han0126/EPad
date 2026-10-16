#include "weather_cities.h"

#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstring>

#include "clock_service.h"
#include "display_mode.h"
#include "weather_client.h"
#include "weather_config.h"
#include "weather_service.h"
#include "weather_settings.h"
#include "wifi_portal.h"

namespace {

weather_cities::State searchState = weather_cities::State::Idle;
weather_cities::Result searchResult = {};
weather_cities::Result completed = {};
std::atomic<bool> resultReady{false};
uint32_t searchRevision = 0;
uint32_t completedAt = 0;

struct Job {
  weather_settings::Settings credentials;
  String query;
  String province;
};

Job job;

void searchTask(void*) {
  completed = weather_client::searchCities(job.credentials, job.query, job.province);
  job.credentials.apiKey = "";
  resultReady.store(true, std::memory_order_release);
  vTaskDelete(nullptr);
}

}  // namespace

namespace weather_cities {

bool start(const String& query, const String& province, const weather_settings::Settings& credentials) {
  struct tm now = {};
  if (!wifi_portal::isActive() || !display_mode::isCalendar() || weather_service::isBusy() ||
      clock_service::isSyncing() || !clock_service::readLocalTime(now) || WiFi.status() != WL_CONNECTED ||
      query.length() == 0 || !weather_settings::isValidName(query) || !weather_settings::isValidName(province) ||
      !weather_settings::isValidHost(credentials.apiHost) || !weather_settings::isValidKey(credentials.apiKey)) return false;
  // 同一查询短期复用结果，只缓存城市信息，不缓存失败结果或密钥。
  if (searchState == State::Ready && millis() - completedAt < CACHE_MS &&
      query == job.query && province == job.province && credentials.apiHost == job.credentials.apiHost) return true;
  job.credentials = credentials;
  job.query = query;
  job.province = province;
  if (++searchRevision == 0) ++searchRevision;
  searchResult = {};
  resultReady.store(false, std::memory_order_relaxed);
  searchState = State::Searching;
  if (xTaskCreatePinnedToCore(searchTask, "city-search", WeatherConfig::TASK_STACK_BYTES,
                              nullptr, 1, nullptr, 0) != pdPASS) {
    searchState = State::Error;
    searchResult.error = weather_data::Error::TaskStart;
    job.credentials.apiKey = "";
  }
  return true;
}

void handle() {
  if (!isBusy() || !resultReady.load(std::memory_order_acquire)) return;
  searchResult = completed;
  resultReady.store(false, std::memory_order_relaxed);
  completedAt = millis();
  searchState = searchResult.error == weather_data::Error::None ? State::Ready : State::Error;
  Serial.printf("[weather-city] Search finished: HTTP %d, API %u, cities=%u, %s.\n",
      static_cast<int>(searchResult.httpStatus), static_cast<unsigned>(searchResult.apiCode),
      static_cast<unsigned>(searchResult.count), weather_service::errorName(searchResult.error));
}

bool isBusy() { return searchState == State::Searching; }
State state() { return searchState; }
uint32_t revision() { return searchRevision; }
const Result& result() { return searchResult; }

bool find(const String& id, uint32_t searchRevision, int16_t latitude, int16_t longitude, City& city) {
  if (searchState != State::Ready || searchRevision != revision() || millis() - completedAt >= CACHE_MS) return false;
  for (size_t i = 0; i < searchResult.count; ++i) {
    const City& candidate = searchResult.cities[i];
    if (id == candidate.id && latitude == candidate.latitude100 && longitude == candidate.longitude100) {
      city = candidate;
      return true;
    }
  }
  return false;
}

}  // namespace weather_cities
