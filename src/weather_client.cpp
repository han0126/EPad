#include "weather_client.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <rom/miniz.h>

#include <cerrno>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <strings.h>
#include <time.h>

#include "weather_config.h"
#include "weather_settings.h"

// 避开 Arduino 的同名包装头，明确使用 IDF 内置的完整根证书包。
extern "C" esp_err_t esp_crt_bundle_attach(void* conf);

namespace {

using weather_data::Error;

struct ResponseHeaders {
  bool gzip = false;
  bool unsupported = false;
  bool cancelledForDate = false;
  uint32_t day = 0;
  uint32_t startedAt = 0;
};

esp_err_t onHttpEvent(esp_http_client_event_t* event) {
  ResponseHeaders& headers = *static_cast<ResponseHeaders*>(event->user_data);
  if (event->event_id == HTTP_EVENT_ON_CONNECTED) {
    const time_t now = time(nullptr);
    struct tm local = {};
    const bool valid = localtime_r(&now, &local) != nullptr;
    const uint32_t day = valid ? static_cast<uint32_t>(local.tm_year + 1900) * 10000 +
        (local.tm_mon + 1) * 100 + local.tm_mday : 0;
    // TLS 完成后、发送 GET 前再次核对日期；零点前一分钟不再发送。
    headers.cancelledForDate = day != headers.day ||
        (valid && local.tm_hour == 23 && local.tm_min == 59);
    if (headers.cancelledForDate || millis() - headers.startedAt >= WeatherConfig::REQUEST_BUDGET_MS) {
      esp_http_client_close(event->client);
      return ESP_FAIL;
    }
  }
  if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key != nullptr &&
      event->header_value != nullptr && strcasecmp(event->header_key, "Content-Encoding") == 0) {
    headers.gzip = strcasecmp(event->header_value, "gzip") == 0;
    headers.unsupported = !headers.gzip && strcasecmp(event->header_value, "identity") != 0;
  }
  return ESP_OK;
}

uint32_t littleEndian32(const uint8_t* bytes) {
  return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
      (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

Error inflateGzip(const uint8_t* input, size_t length, uint8_t* output, size_t& outputLength) {
  // 仅处理 Gzip 封装；DEFLATE 使用 ESP32-S3 ROM 中的 miniz 实现。
  if (length < 18 || input[0] != 0x1F || input[1] != 0x8B || input[2] != 8 ||
      (input[3] & 0xE0) != 0) return Error::Encoding;
  const size_t trailer = length - 8;
  size_t offset = 10;
  const uint8_t flags = input[3];
  if ((flags & 4) != 0) {
    if (offset + 2 > trailer) return Error::Encoding;
    const size_t extra = input[offset] | (static_cast<size_t>(input[offset + 1]) << 8);
    offset += 2;
    if (extra > trailer - offset) return Error::Encoding;
    offset += extra;
  }
  constexpr uint8_t OPTIONAL_FIELDS[] = {8, 16};
  for (uint8_t flag : OPTIONAL_FIELDS) {
    if ((flags & flag) == 0) continue;
    while (offset < trailer && input[offset] != 0) ++offset;
    if (offset == trailer) return Error::Encoding;
    ++offset;
  }
  if ((flags & 2) != 0) {
    if (offset + 2 > trailer) return Error::Encoding;
    const uint16_t expected = input[offset] | (static_cast<uint16_t>(input[offset + 1]) << 8);
    if ((mz_crc32(0, input, offset) & 0xFFFF) != expected) return Error::Encoding;
    offset += 2;
  }
  if (offset >= trailer) return Error::Encoding;
  // 解码器包含较大的 Huffman 表，放在堆上，避免挤占任务栈。
  std::unique_ptr<tinfl_decompressor> decoder(new (std::nothrow) tinfl_decompressor);
  if (!decoder) return Error::Memory;
  tinfl_init(decoder.get());
  size_t inputLength = trailer - offset;
  outputLength = WeatherConfig::MAX_JSON_BYTES;
  const tinfl_status decoded = tinfl_decompress(decoder.get(), input + offset, &inputLength,
      output, output, &outputLength, TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  if (decoded == TINFL_STATUS_HAS_MORE_OUTPUT) return Error::TooLarge;
  if (decoded != TINFL_STATUS_DONE || inputLength != trailer - offset ||
      outputLength != littleEndian32(input + trailer + 4) ||
      mz_crc32(0, output, outputLength) != littleEndian32(input + trailer)) return Error::Encoding;
  return Error::None;
}

bool parseUtc(const char* value, int64_t& result) {
  if (value == nullptr || std::strlen(value) > 40) return false;
  struct tm parsed = {};
  char* end = strptime(value, "%Y-%m-%dT%H:%M:%S", &parsed);
  if (end == nullptr) {
    parsed = {};
    end = strptime(value, "%Y-%m-%dT%H:%M", &parsed);
  }
  if (end == nullptr) return false;
  if (*end == '.') {
    ++end;
    size_t digits = 0;
    while (*end >= '0' && *end <= '9') { ++end; ++digits; }
    if (digits == 0 || digits > 9) return false;
  }
  int offsetMinutes = 0;
  if ((*end == 'Z' || *end == 'z') && end[1] == '\0') {
    offsetMinutes = 0;
  } else {
    // 同时兼容 RFC 3339 的显式时区偏移，不依赖字符串截取来判断预报日期。
    if (std::strlen(end) != 6 || (*end != '+' && *end != '-') || end[3] != ':' ||
        end[1] < '0' || end[1] > '9' || end[2] < '0' || end[2] > '9' ||
        end[4] < '0' || end[4] > '9' || end[5] < '0' || end[5] > '9') return false;
    const int hours = (end[1] - '0') * 10 + end[2] - '0';
    const int minutes = (end[4] - '0') * 10 + end[5] - '0';
    if (hours > 23 || minutes > 59) return false;
    offsetMinutes = (hours * 60 + minutes) * (*end == '-' ? -1 : 1);
  }
  const struct tm original = parsed;
  parsed.tm_isdst = 0;
  // 系统固定为 UTC+8；请求 UTC 返回，转换时补回八小时且不切换全局时区。
  const time_t local = mktime(&parsed);
  if (local == static_cast<time_t>(-1) || parsed.tm_year != original.tm_year ||
      parsed.tm_mon != original.tm_mon || parsed.tm_mday != original.tm_mday ||
      parsed.tm_hour != original.tm_hour || parsed.tm_min != original.tm_min ||
      parsed.tm_sec != original.tm_sec) return false;
  result = static_cast<int64_t>(local) + 8 * 60 * 60 - offsetMinutes * 60;
  return true;
}

bool readTemperature(JsonVariantConst field, double& value) {
  const char* unit = field["unit"];
  if (unit == nullptr || std::strcmp(unit, "°C") != 0 || !field["value"].is<double>()) return false;
  value = field["value"].as<double>();
  return std::isfinite(value) && value >= -99.0 && value <= 99.0;
}

Error parseForecast(uint8_t* body, size_t length, const weather_settings::Settings& settings,
                     uint32_t day, weather_data::Forecast& forecast) {
  DynamicJsonDocument filter(768);
  filter["metadata"]["attributions"] = true;
  JsonObject dateFilter = filter["days"][0].to<JsonObject>();
  dateFilter["forecastStartTime"] = true;
  dateFilter["forecastEndTime"] = true;
  dateFilter["temperatureMax"]["value"] = true;
  dateFilter["temperatureMax"]["unit"] = true;
  dateFilter["temperatureMin"]["value"] = true;
  dateFilter["temperatureMin"]["unit"] = true;
  dateFilter["daytime"]["condition"]["code"] = true;
  DynamicJsonDocument document(3072);
  if (filter.overflowed() || deserializeJson(document, reinterpret_cast<char*>(body), length,
      DeserializationOption::Filter(filter), DeserializationOption::NestingLimit(12))) return Error::Json;
  const JsonArrayConst days = document["days"].as<JsonArrayConst>();
  if (days.size() != 1) return Error::InvalidForecast;
  const JsonObjectConst daily = days[0];
  double maximum = 0, minimum = 0;
  if (!readTemperature(daily["temperatureMax"], maximum) ||
      !readTemperature(daily["temperatureMin"], minimum) || minimum > maximum ||
      !parseUtc(daily["forecastStartTime"], forecast.validFrom) ||
      !parseUtc(daily["forecastEndTime"], forecast.validUntil)) return Error::InvalidForecast;
  forecast.fetchedAt = static_cast<int64_t>(time(nullptr));
  const int64_t interval = forecast.validUntil - forecast.validFrom;
  if (interval < 20 * 60 * 60 || interval > 28 * 60 * 60 ||
      forecast.fetchedAt < forecast.validFrom || forecast.fetchedAt >= forecast.validUntil) return Error::InvalidForecast;
  const char* code = daily["daytime"]["condition"]["code"];
  if (code == nullptr || *code == '\0' || std::strlen(code) > 5) return Error::InvalidForecast;
  for (const char* cursor = code; *cursor != '\0'; ++cursor) {
    if (*cursor < '0' || *cursor > '9') return Error::InvalidForecast;
  }
  char* end = nullptr;
  const unsigned long icon = std::strtoul(code, &end, 10);
  if (*end != '\0' || icon > 65535) return Error::InvalidForecast;
  forecast.day = day;
  forecast.latitude100 = settings.latitude100;
  forecast.longitude100 = settings.longitude100;
  forecast.data = {static_cast<uint16_t>(icon), static_cast<int16_t>(std::lround(maximum)),
      static_cast<int16_t>(std::lround(minimum)), true, weather_display::Source::Network};
  const JsonArrayConst attributions = document["metadata"]["attributions"].as<JsonArrayConst>();
  if (attributions.size() == 0 || attributions.size() > weather_data::MAX_ATTRIBUTIONS) return Error::InvalidAttribution;
  for (JsonVariantConst item : attributions) {
    const char* link = item.as<const char*>();
    if (link == nullptr || std::strlen(link) >= weather_data::ATTRIBUTION_BYTES ||
        std::strncmp(link, "https://", 8) != 0) return Error::InvalidAttribution;
    for (const char* cursor = link; *cursor != '\0'; ++cursor) {
      if (static_cast<uint8_t>(*cursor) < 33 || static_cast<uint8_t>(*cursor) > 126) return Error::InvalidAttribution;
    }
    std::strcpy(forecast.attributions[forecast.attributionCount++], link);
  }
  return Error::None;
}

}  // namespace

namespace weather_client {

weather_data::Result fetch(const weather_settings::Settings& settings, uint32_t day) {
  weather_data::Result result = {};
  result.error = Error::Connection;
  if (!settings.enabled || !weather_settings::isValid(settings)) {
    result.error = Error::InvalidForecast;
    return result;
  }
  std::unique_ptr<uint8_t[]> body(new (std::nothrow) uint8_t[WeatherConfig::MAX_BODY_BYTES + 1]);
  if (!body) { result.error = Error::Memory; return result; }
  char latitude[12], longitude[12], url[256];
  std::snprintf(latitude, sizeof(latitude), "%s%u.%02u", settings.latitude100 < 0 ? "-" : "",
      static_cast<unsigned>(std::abs(settings.latitude100) / 100), static_cast<unsigned>(std::abs(settings.latitude100) % 100));
  std::snprintf(longitude, sizeof(longitude), "%s%u.%02u", settings.longitude100 < 0 ? "-" : "",
      static_cast<unsigned>(std::abs(settings.longitude100) / 100), static_cast<unsigned>(std::abs(settings.longitude100) % 100));
  const int urlLength = std::snprintf(url, sizeof(url), "https://%s/weather/v1/daily/%s/%s?days=1&localTime=false&lang=zh",
      settings.apiHost.c_str(), latitude, longitude);
  if (urlLength < 0 || static_cast<size_t>(urlLength) >= sizeof(url)) {
    result.error = Error::InvalidForecast;
    return result;
  }
  ResponseHeaders headers;
  headers.day = day;
  headers.startedAt = millis();
  esp_http_client_config_t config = {};
  config.url = url;
  config.method = HTTP_METHOD_GET;
  config.timeout_ms = WeatherConfig::IO_TIMEOUT_MS;
  config.disable_auto_redirect = true;
  config.max_authorization_retries = -1;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.skip_cert_common_name_check = false;
  config.event_handler = onHttpEvent;
  config.user_data = &headers;
  config.buffer_size = 1024;
  config.buffer_size_tx = 1024;
  // 避免调试级别 HTTP 日志打印包含 API KEY 的请求头。
  esp_log_level_set("HTTP_CLIENT", ESP_LOG_WARN);
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) { result.error = Error::Memory; return result; }
  const uint32_t startedAt = headers.startedAt;
  size_t used = 0;
  do {
    if (esp_http_client_set_header(client, "X-QW-Api-Key", settings.apiKey.c_str()) != ESP_OK ||
        esp_http_client_set_header(client, "Accept-Encoding", "gzip") != ESP_OK ||
        esp_http_client_set_header(client, "Accept", "application/json") != ESP_OK) break;
    // 分步 API 只发送一次 GET，不使用 perform() 的认证或重定向循环。
    if (esp_http_client_open(client, 0) != ESP_OK) {
      if (headers.cancelledForDate) result.error = Error::InvalidForecast;
      break;
    }
    const uint32_t headerElapsed = millis() - startedAt;
    if (headerElapsed >= WeatherConfig::REQUEST_BUDGET_MS) break;
    // TLS 已消耗的时间也计入预算，响应头阶段不重新获得完整超时。
    const uint32_t headerRemaining = WeatherConfig::REQUEST_BUDGET_MS - headerElapsed;
    esp_http_client_set_timeout_ms(client,
        headerRemaining < WeatherConfig::IO_TIMEOUT_MS ? headerRemaining : WeatherConfig::IO_TIMEOUT_MS);
    const int64_t length = esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    result.httpStatus = status >= 100 && status <= 599 ? status : 0;
    if (length < 0) break;
    if (result.httpStatus != 200) { result.error = Error::HttpStatus; break; }
    if (length > static_cast<int64_t>(WeatherConfig::MAX_BODY_BYTES)) { result.error = Error::TooLarge; break; }
    if (headers.unsupported) { result.error = Error::Encoding; break; }
    bool complete = false;
    for (;;) {
      const uint32_t elapsed = millis() - startedAt;
      if (elapsed >= WeatherConfig::REQUEST_BUDGET_MS) break;
      const uint32_t remaining = WeatherConfig::REQUEST_BUDGET_MS - elapsed;
      esp_http_client_set_timeout_ms(client, remaining < WeatherConfig::IO_TIMEOUT_MS ? remaining : WeatherConfig::IO_TIMEOUT_MS);
      uint8_t extra;
      const size_t capacity = WeatherConfig::MAX_BODY_BYTES - used;
      const int received = esp_http_client_read(client,
          reinterpret_cast<char*>(capacity != 0 ? body.get() + used : &extra),
          capacity != 0 ? static_cast<int>(capacity < 1024 ? capacity : 1024) : 1);
      if (received < 0) break;
      if (received == 0) {
        if (esp_http_client_is_complete_data_received(client)) { complete = true; break; }
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }
      if (capacity == 0) { result.error = Error::TooLarge; break; }
      used += static_cast<size_t>(received);
    }
    if (!complete || used == 0) break;
    // 响应收完立即释放 TLS 内存，再解压和解析，降低峰值堆占用。
    esp_http_client_cleanup(client);
    client = nullptr;
    body[used] = 0;
    const bool gzip = headers.gzip || (used >= 2 && body[0] == 0x1F && body[1] == 0x8B);
    if (gzip) {
      std::unique_ptr<uint8_t[]> decoded(new (std::nothrow) uint8_t[WeatherConfig::MAX_JSON_BYTES + 1]);
      if (!decoded) { result.error = Error::Memory; break; }
      size_t decodedLength = 0;
      result.error = inflateGzip(body.get(), used, decoded.get(), decodedLength);
      if (result.error == Error::None) {
        decoded[decodedLength] = 0;
        result.error = parseForecast(decoded.get(), decodedLength, settings, day, result.forecast);
      }
    } else {
      result.error = parseForecast(body.get(), used, settings, day, result.forecast);
    }
  } while (false);
  if (client != nullptr) esp_http_client_cleanup(client);
  return result;
}

}  // namespace weather_client
