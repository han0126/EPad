#include "weather_client.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <rom/miniz.h>

#include <cmath>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <initializer_list>
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
    headers.cancelledForDate = headers.day != 0 && (day != headers.day ||
        (valid && local.tm_hour == 23 && local.tm_min == 59));
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

struct HttpResult {
  Error error = Error::Connection;
  int16_t httpStatus = 0;
};

using JsonParser = Error (*)(uint8_t*, size_t, void*);

// 天气和城市查询共用有界 HTTPS/Gzip 传输，均只发送一个 GET。
HttpResult requestJson(const weather_settings::Settings& settings, const char* url,
                       uint32_t day, JsonParser parser, void* context) {
  HttpResult result;
  std::unique_ptr<uint8_t[]> body(new (std::nothrow) uint8_t[WeatherConfig::MAX_BODY_BYTES + 1]);
  if (!body) { result.error = Error::Memory; return result; }
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
        result.error = parser(decoded.get(), decodedLength, context);
      }
    } else {
      result.error = parser(body.get(), used, context);
    }
  } while (false);
  if (client != nullptr) esp_http_client_cleanup(client);
  return result;
}

String encodeQuery(const String& value) {
  constexpr char HEX[] = "0123456789ABCDEF";
  String encoded;
  if (!encoded.reserve(value.length() * 3)) return encoded;
  for (size_t i = 0; i < value.length(); ++i) {
    const uint8_t c = static_cast<uint8_t>(value[i]);
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') encoded += static_cast<char>(c);
    else { encoded += '%'; encoded += HEX[c >> 4]; encoded += HEX[c & 15]; }
  }
  return encoded;
}

bool sameCityName(const char* name, const char* parent) {
  // GeoAPI 的 type=city 也包含区县，结合 adm2 排除它们；市后缀不影响比较。
  size_t nameLength = std::strlen(name), parentLength = std::strlen(parent);
  const size_t suffix = std::strlen("市");
  if (nameLength >= suffix && std::strcmp(name + nameLength - suffix, "市") == 0) nameLength -= suffix;
  if (parentLength >= suffix && std::strcmp(parent + parentLength - suffix, "市") == 0) parentLength -= suffix;
  return nameLength != 0 && nameLength == parentLength && std::memcmp(name, parent, nameLength) == 0;
}

bool geoCoordinate(const char* value, int16_t limit, int16_t& output) {
  if (value == nullptr || *value == '\0' || std::strlen(value) > 24) return false;
  char* end = nullptr;
  errno = 0;
  const double coordinate = std::strtod(value, &end);
  if (end == value || *end != '\0' || errno == ERANGE || !std::isfinite(coordinate) ||
      coordinate * 100 < -limit || coordinate * 100 > limit) return false;
  output = static_cast<int16_t>(std::lround(coordinate * 100));
  return true;
}

Error parseCities(uint8_t* body, size_t length, void* context) {
  auto& result = *static_cast<weather_cities::Result*>(context);
  DynamicJsonDocument filter(1024);
  filter["code"] = true;
  JsonObject fields = filter["location"][0].to<JsonObject>();
  for (const char* key : {"id", "name", "adm1", "adm2", "country", "lat", "lon", "type"}) fields[key] = true;
  filter["refer"]["sources"] = true;
  DynamicJsonDocument document(8192);
  if (filter.overflowed() || deserializeJson(document, reinterpret_cast<char*>(body), length,
      DeserializationOption::Filter(filter), DeserializationOption::NestingLimit(8))) return Error::Json;
  const char* code = document["code"];
  if (code == nullptr || std::strlen(code) != 3) return Error::Json;
  for (const char* c = code; *c; ++c) if (*c < '0' || *c > '9') return Error::Json;
  result.apiCode = static_cast<uint16_t>(std::strtoul(code, nullptr, 10));
  if (result.apiCode == 404) return Error::None;
  if (result.apiCode != 200) return Error::HttpStatus;
  const JsonArrayConst locations = document["location"].as<JsonArrayConst>();
  if (locations.isNull() || locations.size() > 20) return Error::InvalidForecast;
  for (JsonObjectConst location : locations) {
    const char* name = location["name"];
    const char* parent = location["adm2"];
    const char* province = location["adm1"];
    const char* id = location["id"];
    const char* country = location["country"];
    const char* type = location["type"];
    if (!name || !parent || !province || !id || !country || !type ||
        (std::strcmp(country, "中国") != 0 && std::strcmp(country, "China") != 0) ||
        std::strcmp(type, "city") != 0) continue;
    if (std::strlen(name) >= weather_cities::NAME_BYTES || std::strlen(parent) >= weather_cities::NAME_BYTES ||
        std::strlen(province) >= weather_cities::NAME_BYTES || std::strlen(id) >= weather_cities::ID_BYTES ||
        *name == '\0' || *id == '\0' || *province == '\0' || !weather_settings::isValidName(name) ||
        !weather_settings::isValidName(province) || !weather_settings::isValidCityId(id)) return Error::InvalidForecast;
    if (!sameCityName(name, parent)) continue;
    bool duplicate = false;
    for (size_t i = 0; i < result.count; ++i) if (std::strcmp(result.cities[i].id, id) == 0) duplicate = true;
    if (duplicate || result.count == weather_cities::MAX_CITIES) continue;
    weather_cities::City& city = result.cities[result.count];
    if (!geoCoordinate(location["lat"], 9000, city.latitude100) ||
        !geoCoordinate(location["lon"], 18000, city.longitude100)) return Error::InvalidForecast;
    std::strcpy(city.id, id);
    std::strcpy(city.name, name);
    std::strcpy(city.province, province);
    ++result.count;
  }
  const JsonArrayConst sources = document["refer"]["sources"].as<JsonArrayConst>();
  if (sources.size() > weather_data::MAX_ATTRIBUTIONS) return Error::InvalidAttribution;
  for (JsonVariantConst item : sources) {
    const char* source = item.as<const char*>();
    // GeoAPI 的来源也可以是文字说明，不套用天气接口的 HTTPS 链接限制。
    if (!source || *source == '\0' || std::strlen(source) >= weather_data::ATTRIBUTION_BYTES) return Error::InvalidAttribution;
    for (const char* c = source; *c; ++c) {
      const uint8_t byte = static_cast<uint8_t>(*c);
      if (byte < 32 || byte == 127) return Error::InvalidAttribution;
    }
    std::strcpy(result.sources[result.sourceCount++], source);
  }
  return Error::None;
}

}  // namespace

namespace weather_client {

weather_data::Result fetch(const weather_settings::Settings& settings, uint32_t day) {
  weather_data::Result result = {};
  if (!settings.enabled || !weather_settings::isValid(settings)) { result.error = Error::InvalidForecast; return result; }
  char latitude[12], longitude[12], url[256];
  std::snprintf(latitude, sizeof(latitude), "%s%u.%02u", settings.latitude100 < 0 ? "-" : "",
      static_cast<unsigned>(std::abs(settings.latitude100) / 100), static_cast<unsigned>(std::abs(settings.latitude100) % 100));
  std::snprintf(longitude, sizeof(longitude), "%s%u.%02u", settings.longitude100 < 0 ? "-" : "",
      static_cast<unsigned>(std::abs(settings.longitude100) / 100), static_cast<unsigned>(std::abs(settings.longitude100) % 100));
  const int length = std::snprintf(url, sizeof(url), "https://%s/weather/v1/daily/%s/%s?days=1&localTime=false&lang=zh",
      settings.apiHost.c_str(), latitude, longitude);
  if (length < 0 || static_cast<size_t>(length) >= sizeof(url)) { result.error = Error::InvalidForecast; return result; }
  struct Context { const weather_settings::Settings& settings; uint32_t day; weather_data::Forecast& forecast; };
  Context context{settings, day, result.forecast};
  const HttpResult response = requestJson(settings, url, day, [](uint8_t* body, size_t size, void* value) {
    const auto& context = *static_cast<Context*>(value);
    return parseForecast(body, size, context.settings, context.day, context.forecast);
  }, &context);
  result.error = response.error;
  result.httpStatus = response.httpStatus;
  return result;
}

weather_cities::Result searchCities(const weather_settings::Settings& settings,
                                   const String& query, const String& province) {
  weather_cities::Result result = {};
  if (!weather_settings::isValidHost(settings.apiHost) || !weather_settings::isValidKey(settings.apiKey) ||
      query.length() == 0 || !weather_settings::isValidName(query) || !weather_settings::isValidName(province)) {
    result.error = Error::InvalidForecast;
    return result;
  }
  const String encodedQuery = encodeQuery(query);
  const String encodedProvince = encodeQuery(province);
  if (encodedQuery.length() == 0 || (province.length() != 0 && encodedProvince.length() == 0)) {
    result.error = Error::Memory;
    return result;
  }
  char url[640];
  const int length = std::snprintf(url, sizeof(url),
      "https://%s/geo/v2/city/lookup?location=%s%s%s&range=cn&number=20&lang=zh",
      settings.apiHost.c_str(), encodedQuery.c_str(), province.length() == 0 ? "" : "&adm=", encodedProvince.c_str());
  if (length < 0 || static_cast<size_t>(length) >= sizeof(url)) { result.error = Error::TooLarge; return result; }
  const HttpResult response = requestJson(settings, url, 0, parseCities, &result);
  result.error = response.error;
  result.httpStatus = response.httpStatus;
  if (result.error != Error::None) { result.count = 0; result.sourceCount = 0; }
  return result;
}

}  // namespace weather_client
