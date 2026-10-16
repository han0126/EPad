#include "weather_cache.h"

#include <nvs.h>

#include <cstring>

#include "calendar_data.h"

namespace {

constexpr char STORAGE_NAMESPACE[] = "epd-weather";
constexpr char RECORD_KEY[] = "daily";
constexpr uint32_t VERSION = 1;

bool validDay(uint32_t day) {
  const calendar::Date date = {static_cast<uint16_t>(day / 10000),
      static_cast<uint8_t>((day / 100) % 100), static_cast<uint8_t>(day % 100)};
  return day >= 20240101 && day <= 20991231 && calendar::isValidDate(date);
}

bool isValid(const weather_cache::Record& record) {
  if (record.version != VERSION || record.forecastValid > 1 ||
      (record.lastAttemptDay != 0 && !validDay(record.lastAttemptDay)) ||
      static_cast<uint8_t>(record.lastError) > static_cast<uint8_t>(weather_data::Error::Interrupted) ||
      record.lastHttpStatus < 0 || record.lastHttpStatus > 599) return false;
  if (!record.forecastValid) return true;
  const weather_data::Forecast& forecast = record.forecast;
  if (!validDay(forecast.day) || forecast.day > record.lastAttemptDay ||
      forecast.latitude100 < -9000 || forecast.latitude100 > 9000 ||
      forecast.longitude100 < -18000 || forecast.longitude100 > 18000 ||
      forecast.validFrom < 1703894400LL || forecast.validUntil > 4133980800LL ||
      forecast.validUntil <= forecast.validFrom ||
      forecast.validUntil - forecast.validFrom < 20 * 60 * 60 ||
      forecast.validUntil - forecast.validFrom > 28 * 60 * 60 || forecast.fetchedAt < forecast.validFrom ||
      forecast.fetchedAt >= forecast.validUntil ||
      forecast.data.source != weather_display::Source::Network ||
      !weather_display::isValidPreview(forecast.data) ||
      forecast.attributionCount == 0 || forecast.attributionCount > weather_data::MAX_ATTRIBUTIONS) return false;
  for (size_t i = 0; i < forecast.attributionCount; ++i) {
    if (forecast.attributions[i][weather_data::ATTRIBUTION_BYTES - 1] != '\0' ||
        std::strncmp(forecast.attributions[i], "https://", 8) != 0) return false;
  }
  return true;
}

}  // namespace

namespace weather_cache {

LoadResult load(Record& record) {
  record = {};
  record.version = VERSION;
  nvs_handle_t handle;
  const esp_err_t opened = nvs_open(STORAGE_NAMESPACE, NVS_READONLY, &handle);
  if (opened == ESP_ERR_NVS_NOT_FOUND) return LoadResult::Empty;
  if (opened != ESP_OK) return LoadResult::Error;
  size_t length = 0;
  const esp_err_t found = nvs_get_blob(handle, RECORD_KEY, nullptr, &length);
  if (found == ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(handle);
    return LoadResult::Empty;
  }
  if (found != ESP_OK || length != sizeof(Record)) {
    nvs_close(handle);
    return LoadResult::Error;
  }
  Record candidate = {};
  const esp_err_t read = nvs_get_blob(handle, RECORD_KEY, &candidate, &length);
  nvs_close(handle);
  if (read != ESP_OK || !isValid(candidate)) return LoadResult::Error;
  record = candidate;
  return LoadResult::Loaded;
}

bool save(const Record& record) {
  if (!isValid(record)) return false;
  nvs_handle_t handle;
  if (nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;
  const bool saved = nvs_set_blob(handle, RECORD_KEY, &record, sizeof(record)) == ESP_OK &&
      nvs_commit(handle) == ESP_OK;
  nvs_close(handle);
  return saved;
}

}  // namespace weather_cache
