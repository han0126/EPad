#include "wifi_portal.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>

#include <cstdio>
#include <cstring>
#include <cerrno>
#include <cstdlib>

#include "calendar_events.h"
#include "calendar_events_portal.h"
#include "display_service.h"
#include "clock_service.h"
#include "display_mode.h"
#include "image_transfer.h"
#include "network_settings.h"
#include "wifi_connection.h"
#include "network_time_config.h"
#include "power_service.h"
#include "weather_settings.h"
#include "weather_icons.h"
#include "weather_service.h"
#include "weather_cities.h"
#include "portal_http_server.h"
#include "portal_security.h"
#include "secure_memory.h"
#include "portal_assets.h"

namespace {

constexpr char PORTAL_URL[] = "http://192.168.4.1/";

PortalHttpServer server(IPAddress(192, 168, 4, 1));
DNSServer dns;
bool active = false;
bool routesReady = false;
bool closeRequested = false;
uint32_t closeRequestedAt = 0;
bool savePending = false;
bool hasSavedNetwork = false;
bool storageFailed = false;
bool successWindow = false;
uint32_t openedAt = 0;
uint32_t synchronizedAt = 0;
clock_service::SyncStatus previousStatus = clock_service::SyncStatus::Idle;
network_settings::Credentials pendingCredentials;
uint32_t writeWindowAt = 0;
uint8_t writeCount = 0;
char lastSync[24] = {};
uint32_t imageOpenedAt = 0;
bool imageWorkPending = false;
bool rawAccepted = false;
bool rawComplete = false;
uint32_t uploadStartedAt = 0;
int uploadHttpStatus = 400;
const char* uploadError = "image_header";

uint32_t remainingMs() {
  if (power_service::imageSleepPending()) {
    return power_service::status().sleepInSeconds * 1000UL;
  }
  if (!display_mode::isCalendar()) {
    const uint32_t elapsed = millis() - imageOpenedAt;
    return elapsed < NetworkTimeConfig::IMAGE_PORTAL_TIMEOUT_MS
        ? NetworkTimeConfig::IMAGE_PORTAL_TIMEOUT_MS - elapsed : 0;
  }
  const uint32_t elapsed = millis() - openedAt;
  uint32_t remaining = elapsed < NetworkTimeConfig::PORTAL_TIMEOUT_MS
      ? NetworkTimeConfig::PORTAL_TIMEOUT_MS - elapsed : 0;
  if (successWindow) {
    const uint32_t sinceSync = millis() - synchronizedAt;
    const uint32_t view = sinceSync < NetworkTimeConfig::PORTAL_SUCCESS_VIEW_MS
        ? NetworkTimeConfig::PORTAL_SUCCESS_VIEW_MS - sinceSync : 0;
    if (view < remaining) remaining = view;
  }
  return remaining;
}

const char* stateName() {
  if (storageFailed) return "save_failed";
  switch (clock_service::syncStatus()) {
    case clock_service::SyncStatus::Connecting: return "connecting";
    case clock_service::SyncStatus::WaitingForNtp: return "syncing";
    case clock_service::SyncStatus::Success: return "success";
    case clock_service::SyncStatus::NotConfigured: return "not_configured";
    case clock_service::SyncStatus::WifiStartFailed: return "wifi_error";
    case clock_service::SyncStatus::WifiTimeout: return "wifi_timeout";
    case clock_service::SyncStatus::NtpTimeout: return "ntp_timeout";
    default: return "idle";
  }
}

void sendJson(int code, const char* body) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json; charset=utf-8", body);
}

bool authorized() {
  if (portal_security::authenticated(server.header("X-Portal-Token"), server.client().remoteIP())) return true;
  sendJson(401, "{\"error\":\"session_expired\"}");
  return false;
}

bool requestGate(WebServer& request) {
  if (!request.uri().startsWith("/api/")) {
    if (request.method() != HTTP_POST) return true;
    sendJson(405, "{\"error\":\"request_method\"}");
    return false;
  }
  const String host = request.hostHeader();
  const String origin = request.header("Origin");
  if ((host != "192.168.4.1" && host != "192.168.4.1:80") ||
      (origin.length() != 0 && origin != "http://192.168.4.1" && origin != "http://192.168.4.1:80") ||
      request.header("Sec-Fetch-Site") == "cross-site") {
    sendJson(403, "{\"error\":\"request_origin\"}");
    return false;
  }
  if (request.uri() == "/api/login" && request.method() == HTTP_POST) {
    if (closeRequested || remainingMs() == 0 || power_service::imageSleepPending()) {
      sendJson(409, "{\"error\":\"portal_closing\"}");
      return false;
    }
    if (portal_security::loginAllowed()) return true;
    sendJson(429, "{\"error\":\"login_locked\"}");
    return false;
  }
  if (!authorized()) return false;
  if (closeRequested || remainingMs() == 0) {
    sendJson(409, "{\"error\":\"portal_closing\"}");
    return false;
  }
  if (request.method() == HTTP_POST) {
    if (millis() - writeWindowAt >= 10000) { writeWindowAt = millis(); writeCount = 0; }
    if (writeCount >= 20) { sendJson(429, "{\"error\":\"request_rate\"}"); return false; }
    ++writeCount;
  }
  return true;
}

void login() {
  String password = server.arg("password");
  const bool accepted = portal_security::login(password, server.client().remoteIP());
  secure_memory::clear(password);
  if (!accepted) {
    sendJson(portal_security::loginAllowed() ? 401 : 429,
        portal_security::loginAllowed() ? "{\"error\":\"login_invalid\"}" : "{\"error\":\"login_locked\"}");
    return;
  }
  char json[64];
  std::snprintf(json, sizeof(json), "{\"token\":\"%s\"}", portal_security::token());
  sendJson(200, json);
  secure_memory::wipe(json, sizeof(json));
}

bool calendarSelected() {
  if (display_mode::isCalendar()) return true;
  sendJson(409, "{\"error\":\"wrong_mode\"}");
  return false;
}

void redirectToPortal() {
  // 系统联网探测和陌生网址统一跳到真实 IP，不在探测域名下展开设置页。
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("Location", PORTAL_URL);
  server.send(302, "text/plain; charset=utf-8", "");
}

void sendPage() {
  const String host = server.hostHeader();
  if (host.length() != 0 && host != "192.168.4.1" && host != "192.168.4.1:80") {
    redirectToPortal();
    return;
  }
  portal_assets::send(server, portal_assets::Asset::Page, false);
}

bool readEventsAllowed() {
  return authorized() && calendarSelected();
}

bool writeEventsAllowed() {
  if (!readEventsAllowed()) return false;
  if (closeRequested || remainingMs() == 0) {
    sendJson(409, "{\"error\":\"portal_closing\"}");
    return false;
  }
  if (clock_service::isSyncing() || weather_service::isBusy() ||
      display_service::isBusy() || image_transfer::isReceiving()) {
    sendJson(409, "{\"error\":\"mode_busy\"}");
    return false;
  }
  return true;
}

const char* calendarState(display_service::State state) {
  switch (state) {
    case display_service::State::Queued: return "queued";
    case display_service::State::Drawing: return "drawing";
    case display_service::State::Ready: return "ready";
    case display_service::State::Error: return "error";
    default: return "waiting";
  }
}

const char* calendarError(display_service::Error error) {
  switch (error) {
    case display_service::Error::TaskStartFailed: return "task_failed";
    case display_service::Error::CalendarFailed: return "calendar_failed";
    case display_service::Error::PanelFailed: return "panel_failed";
    case display_service::Error::InvalidFrame: return "invalid_frame";
    default: return "";
  }
}

const char* imageState(const display_service::Status& display) {
  if (display_mode::isCalendar()) return "inactive";
  if (image_transfer::isReceiving()) return "receiving";
  if (display.error == display_service::Error::TaskStartFailed) return "error";
  if (display.imageRequest) {
    if (display.state == display_service::State::Queued) return "queued";
    if (display.state == display_service::State::Drawing) return "drawing";
    if (display.state == display_service::State::Error) return "error";
  }
  if (image_transfer::error() != image_transfer::Error::None) return "upload_error";
  if (display_mode::storageFailed()) return "error";
  if (display.state == display_service::State::Ready && display.image) return "ready";
  if (display_mode::hasRetainedImage()) return "retained";
  return "awaiting_upload";
}

void formatDate(const calendar::Date& date, char* output, size_t size) {
  if (date.year == 0) {
    output[0] = '\0';
    return;
  }
  std::snprintf(output, size, "%04u-%02u-%02u", static_cast<unsigned>(date.year),
                static_cast<unsigned>(date.month), static_cast<unsigned>(date.day));
}

void sendStatus() {
  if (!authorized()) return;
  power_service::requestImageSleep();
  struct tm current = {};
  const bool valid = clock_service::readLocalTime(current);
  char timeText[16] = {};
  char dateText[16] = {};
  if (valid) {
    strftime(timeText, sizeof(timeText), "%H:%M:%S", &current);
    strftime(dateText, sizeof(dateText), "%Y-%m-%d", &current);
  }

  // 状态含当天备忘，仅登录后返回；不回传 Wi-Fi 密码或 API KEY。
  const display_service::Status display = display_service::status();
  char displayedDate[16];
  char requestedDate[16];
  formatDate(display.displayedDate, displayedDate, sizeof(displayedDate));
  formatDate(display.requestedDate, requestedDate, sizeof(requestedDate));
  const power_service::Status power = power_service::status();
  char nextWake[24] = {};
  struct tm wakeTime = {};
  if (power.nextWake != 0 && localtime_r(&power.nextWake, &wakeTime) != nullptr) {
    strftime(nextWake, sizeof(nextWake), "%Y-%m-%d %H:%M:%S", &wakeTime);
  }
  const clock_service::TimeSource source = clock_service::timeSource();
  const char* sourceName = !valid ? "none" :
      (source == clock_service::TimeSource::Network ? "network" : "retained");
  // 使用结构化序列化，保证服务端来源链接中的特殊字符也能安全返回。
  DynamicJsonDocument json(4096);
  json["state"] = stateName();
  json["valid"] = valid;
  json["time"] = timeText;
  json["date"] = dateText;
  json["weekday"] = valid ? current.tm_wday : 0;
  json["lastSync"] = lastSync;
  json["saved"] = hasSavedNetwork;
  json["connected"] = WiFi.status() == WL_CONNECTED;
  json["remaining"] = (remainingMs() + 999) / 1000;
  JsonObject eventsJson = json.createNestedObject("events");
  eventsJson["ready"] = calendar_events::isReady();
  eventsJson["revision"] = calendar_events::revision();
  eventsJson["count"] = calendar_events::count();
  JsonObject calendarJson = json.createNestedObject("calendar");
  calendarJson["state"] = calendarState(display.state);
  calendarJson["error"] = calendarError(display.error);
  calendarJson["date"] = displayedDate;
  calendarJson["target"] = requestedDate;
  calendarJson["preview"] = display.preview;
  calendarJson["rows"] = display.rows;
  calendarJson["eventText"] = display.events.text;
  JsonObject powerJson = json.createNestedObject("power");
  powerJson["timerWake"] = power.timerWake;
  powerJson["nextWake"] = nextWake;
  powerJson["timeSource"] = sourceName;
  powerJson["imageSleepPending"] = power.imageSleepPending;
  powerJson["sleepIn"] = power.sleepInSeconds;
  JsonObject displayJson = json.createNestedObject("display");
  displayJson["mode"] = display_mode::isCalendar() ? "calendar" : "image";
  displayJson["imageState"] = imageState(display);
  displayJson["imageError"] = display_mode::storageFailed() ? "mode_save_failed" :
      (image_transfer::error() != image_transfer::Error::None ? image_transfer::errorName() :
        ((display.imageRequest || display.error == display_service::Error::TaskStartFailed)
          ? calendarError(display.error) : ""));
  displayJson["uploadEnabled"] = display.error != display_service::Error::TaskStartFailed && !power.imageSleepPending;
  const weather_service::Status weather = weather_service::status();
  JsonObject weatherJson = json.createNestedObject("weather");
  weatherJson["preview"] = display.weatherPreview;
  weatherJson["code"] = display.weather.code;
  weatherJson["tempMax"] = display.weather.maxTemperature;
  weatherJson["tempMin"] = display.weather.minTemperature;
  weatherJson["temperaturesValid"] = display.weather.temperaturesValid;
  weatherJson["known"] = weather_icons::isKnown(display.weather.code);
  weatherJson["source"] = weather_display::sourceName(display.weather.source);
  weatherJson["state"] = weather_service::stateName(weather.state);
  weatherJson["error"] = weather_service::errorName(weather.error);
  weatherJson["httpStatus"] = weather.httpStatus;
  weatherJson["attemptDay"] = weather.attemptDay;
  weatherJson["dataDay"] = weather.dataDay;
  weatherJson["locationRefresh"] = weather.locationRefresh;
  weatherJson["citySearchBusy"] = weather_cities::isBusy();
  JsonArray links = weatherJson.createNestedArray("attributions");
  for (size_t i = 0; i < weather_service::attributionCount(); ++i) links.add(weather_service::attribution(i));
  if (json.overflowed()) { sendJson(503, "{\"error\":\"status_memory\"}"); return; }
  String body;
  if (!body.reserve(measureJson(json) + 1)) { sendJson(503, "{\"error\":\"status_memory\"}"); return; }
  serializeJson(json, body);
  sendJson(200, body.c_str());
}

void startRequest(const network_settings::Credentials& credentials, bool save) {
  storageFailed = false;
  successWindow = false;
  savePending = save;
  pendingCredentials = credentials;
  clock_service::beginNetworkTime(credentials.ssid.c_str(),
                                  credentials.password.c_str(), true);
  previousStatus = clock_service::SyncStatus::Idle;
  sendJson(202, "{\"accepted\":true}");
}

void connectNetwork() {
  if (!authorized() || !calendarSelected()) return;
  if (clock_service::isSyncing() || weather_service::isBusy()) {
    sendJson(409, "{\"error\":\"busy\"}");
    return;
  }
  network_settings::Credentials credentials;
  credentials.ssid = server.arg("ssid");
  credentials.password = server.arg("password");
  if (!network_settings::isValid(credentials)) {
    sendJson(400, "{\"error\":\"invalid_credentials\"}");
    return;
  }
  startRequest(credentials, true);
}

void resyncNetwork() {
  if (!authorized() || !calendarSelected()) return;
  if (clock_service::isSyncing() || weather_service::isBusy()) {
    sendJson(409, "{\"error\":\"busy\"}");
    return;
  }
  network_settings::Credentials credentials;
  if (!network_settings::load(credentials)) {
    sendJson(400, "{\"error\":\"no_saved_network\"}");
    return;
  }
  startRequest(credentials, false);
}

void clearNetwork() {
  if (!writeEventsAllowed()) return;
  if (!network_settings::clear()) {
    sendJson(503, "{\"error\":\"network_clear_failed\"}");
    return;
  }
  savePending = false;
  hasSavedNetwork = false;
  storageFailed = false;
  secure_memory::clear(pendingCredentials.password);
  pendingCredentials.ssid = "";
  // 保留热点和已校准的系统时间，只清除 STA 当前连接配置。
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, true);
  wifi_connection::forgetHint();
  clock_service::beginNetworkTime(nullptr, nullptr, true);
  previousStatus = clock_service::syncStatus();
  sendJson(200, "{\"cleared\":true}");
}

void sendWeatherSettings(const weather_settings::Settings& settings, bool saved) {
  // 城市名称通过 JSON 库转义；绝不回传密钥。
  DynamicJsonDocument json(1024);
  json["saved"] = saved;
  json["enabled"] = settings.enabled;
  json["apiHost"] = settings.apiHost;
  json["keySaved"] = settings.apiKey.length() != 0;
  json["latitude100"] = settings.latitude100;
  json["longitude100"] = settings.longitude100;
  json["cityId"] = settings.cityId;
  json["cityName"] = settings.cityName;
  json["cityProvince"] = settings.cityProvince;
  if (json.overflowed()) { sendJson(503, "{\"error\":\"status_memory\"}"); return; }
  String body;
  if (!body.reserve(measureJson(json) + 1)) { sendJson(503, "{\"error\":\"status_memory\"}"); return; }
  serializeJson(json, body);
  sendJson(200, body.c_str());
}

void readWeatherSettings() {
  if (!authorized() || !calendarSelected()) return;
  weather_settings::Settings settings;
  const bool saved = weather_settings::load(settings);
  sendWeatherSettings(settings, saved);
}

void clearWeatherKey() {
  if (!writeEventsAllowed()) return;
  weather_settings::Settings settings;
  if (!weather_settings::load(settings)) {
    sendJson(503, "{\"error\":\"weather_save_failed\"}");
    return;
  }
  secure_memory::clear(settings.apiKey);
  settings.enabled = false;
  if (!weather_settings::save(settings)) {
    sendJson(503, "{\"error\":\"weather_save_failed\"}");
    return;
  }
  weather_service::settingsChanged();
  display_service::requestWeatherRedraw();
  sendWeatherSettings(settings, true);
}

void saveWeatherSettings() {
  if (!authorized() || !calendarSelected()) return;
  if (closeRequested || remainingMs() == 0) {
    sendJson(409, "{\"error\":\"portal_closing\"}");
    return;
  }
  if (clock_service::isSyncing() || weather_service::isBusy() || display_service::isBusy() || image_transfer::isReceiving()) {
    sendJson(409, "{\"error\":\"mode_busy\"}");
    return;
  }
  const String enabled = server.arg("enabled");
  if (enabled != "0" && enabled != "1") {
    sendJson(400, "{\"error\":\"weather_settings_invalid\"}");
    return;
  }
  weather_settings::Settings settings;
  weather_settings::load(settings);
  const int16_t previousLatitude = settings.latitude100;
  const int16_t previousLongitude = settings.longitude100;
  const bool previousEnabled = settings.enabled;
  const String previousCityName = settings.cityName;
  settings.enabled = enabled == "1";
  settings.apiHost = server.arg("apiHost");
  settings.apiHost.trim();
  settings.apiHost.toLowerCase();
  // 留空保留原密钥，不向页面回填密钥，也不在编译时写入源码。
  String key = server.arg("apiKey");
  if (key.length() != 0) settings.apiKey = key;
  secure_memory::clear(key);
  if ((settings.enabled || settings.apiHost.length() != 0) &&
      !weather_settings::isValidHost(settings.apiHost)) {
    sendJson(400, "{\"error\":\"weather_host_invalid\"}");
    return;
  }
  if ((settings.enabled || settings.apiKey.length() != 0) &&
      !weather_settings::isValidKey(settings.apiKey)) {
    sendJson(400, "{\"error\":\"weather_key_invalid\"}");
    return;
  }
  if (!weather_settings::parseCoordinate(server.arg("latitude"), 9000, settings.latitude100) ||
      !weather_settings::parseCoordinate(server.arg("longitude"), 18000, settings.longitude100)) {
    sendJson(400, "{\"error\":\"weather_location_invalid\"}");
    return;
  }
  const String cityId = server.arg("cityId");
  const String cityMode = server.arg("cityMode");
  const String revision = server.arg("cityQueryId");
  // 兼容旧页面未传模式；新页面的搜索选择必须使用服务端候选或已有配置。
  if ((cityMode.length() != 0 && cityMode != "search" && cityMode != "coordinates") ||
      (cityMode == "coordinates" && cityId.length() != 0)) {
    sendJson(400, "{\"error\":\"weather_settings_invalid\"}");
    return;
  }
  if (cityId.length() == 0) {
    if (cityMode == "search") { sendJson(400, "{\"error\":\"weather_city_required\"}"); return; }
    settings.cityId = "";
    settings.cityName = "";
    settings.cityProvince = "";
  } else if (!((revision.length() == 0 || revision == "0") &&
      cityId == settings.cityId && settings.cityName.length() != 0 &&
      settings.latitude100 == previousLatitude && settings.longitude100 == previousLongitude)) {
    char* end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(revision.c_str(), &end, 10);
    bool digits = revision.length() > 0 && revision.length() <= 10;
    for (size_t i = 0; i < revision.length(); ++i) if (revision[i] < '0' || revision[i] > '9') digits = false;
    weather_cities::City city = {};
    if (!digits || errno == ERANGE || *end != '\0' || parsed == 0 ||
        !weather_cities::find(cityId, static_cast<uint32_t>(parsed), settings.latitude100, settings.longitude100, city)) {
      sendJson(409, "{\"error\":\"weather_city_stale\"}");
      return;
    }
    settings.cityId = city.id;
    settings.cityName = city.name;
    settings.cityProvince = city.province;
  }
  if (!weather_settings::save(settings)) {
    Serial.println("[weather] ERROR: settings save failed.");
    sendJson(503, "{\"error\":\"weather_save_failed\"}");
    return;
  }
  Serial.println("[weather] Settings saved; each location change permits one additional request.");
  weather_service::settingsChanged();
  if (settings.latitude100 != previousLatitude || settings.longitude100 != previousLongitude ||
      settings.enabled != previousEnabled || settings.cityName != previousCityName) {
    display_service::requestWeatherRedraw();
  }
  sendWeatherSettings(settings, true);
}

void sendCitySearchStatus() {
  if (!authorized() || !calendarSelected()) return;
  const weather_cities::Result& result = weather_cities::result();
  const weather_cities::State state = weather_cities::state();
  DynamicJsonDocument json(6144);
  json["id"] = weather_cities::revision();
  json["state"] = state == weather_cities::State::Searching ? "searching" :
      (state == weather_cities::State::Ready ? "ready" : (state == weather_cities::State::Error ? "error" : "idle"));
  json["error"] = weather_service::errorName(result.error);
  json["httpStatus"] = result.httpStatus;
  json["apiCode"] = result.apiCode;
  JsonArray cities = json.createNestedArray("cities");
  if (state == weather_cities::State::Ready) {
    for (size_t i = 0; i < result.count; ++i) {
      const weather_cities::City& city = result.cities[i];
      JsonObject item = cities.createNestedObject();
      item["id"] = city.id;
      item["name"] = city.name;
      item["province"] = city.province;
      item["latitude100"] = city.latitude100;
      item["longitude100"] = city.longitude100;
    }
  }
  JsonArray sources = json.createNestedArray("sources");
  for (size_t i = 0; i < result.sourceCount; ++i) sources.add(result.sources[i]);
  if (json.overflowed()) { sendJson(503, "{\"error\":\"status_memory\"}"); return; }
  String body;
  if (!body.reserve(measureJson(json) + 1)) { sendJson(503, "{\"error\":\"status_memory\"}"); return; }
  serializeJson(json, body);
  sendJson(200, body.c_str());
}

void searchWeatherCities() {
  if (!authorized() || !calendarSelected()) return;
  if (closeRequested || remainingMs() == 0) { sendJson(409, "{\"error\":\"portal_closing\"}"); return; }
  if (clock_service::isSyncing() || weather_service::isBusy() || display_service::isBusy() || image_transfer::isReceiving()) {
    sendJson(409, "{\"error\":\"mode_busy\"}"); return;
  }
  String query = server.arg("query"), province = server.arg("province");
  query.trim();
  province.trim();
  if (query.length() == 0 || !weather_settings::isValidName(query) || !weather_settings::isValidName(province)) {
    sendJson(400, "{\"error\":\"weather_city_query\"}"); return;
  }
  weather_settings::Settings credentials;
  weather_settings::load(credentials);
  credentials.apiHost = server.arg("apiHost");
  credentials.apiHost.trim();
  credentials.apiHost.toLowerCase();
  String key = server.arg("apiKey");
  if (key.length() != 0) credentials.apiKey = key;
  secure_memory::clear(key);
  if (!weather_settings::isValidHost(credentials.apiHost)) { sendJson(400, "{\"error\":\"weather_host_invalid\"}"); return; }
  if (!weather_settings::isValidKey(credentials.apiKey)) { sendJson(400, "{\"error\":\"weather_key_invalid\"}"); return; }
  struct tm now = {};
  if (WiFi.status() != WL_CONNECTED || !clock_service::readLocalTime(now)) {
    sendJson(409, "{\"error\":\"weather_city_network\"}"); return;
  }
  if (!weather_cities::start(query, province, credentials)) { sendJson(409, "{\"error\":\"mode_busy\"}"); return; }
  // 手动搜索后保留原有两分钟操作窗口；本地状态轮询不延长热点寿命。
  openedAt = synchronizedAt = millis();
  successWindow = true;
  sendCitySearchStatus();
}

bool parseWeatherNumber(const String& value, long minimum, long maximum, long& result) {
  if (value.length() == 0 || value.length() > 6 ||
      std::strlen(value.c_str()) != value.length()) return false;
  errno = 0;
  char* end = nullptr;
  const long parsed = std::strtol(value.c_str(), &end, 10);
  if (end == value.c_str() || *end != '\0' || errno == ERANGE ||
      parsed < minimum || parsed > maximum) return false;
  result = parsed;
  return true;
}

bool weatherPreviewAllowed() {
  if (!authorized() || !calendarSelected()) return false;
  if (closeRequested || remainingMs() == 0) {
    sendJson(409, "{\"error\":\"portal_closing\"}");
    return false;
  }
  if (clock_service::isSyncing() || weather_service::isBusy() || display_service::isBusy() || image_transfer::isReceiving()) {
    sendJson(409, "{\"error\":\"mode_busy\"}");
    return false;
  }
  return true;
}

void previewWeather() {
  if (!weatherPreviewAllowed()) return;
  long code = 0, maximum = 0, minimum = 0;
  if (!parseWeatherNumber(server.arg("code"), 0, 65535, code) ||
      !parseWeatherNumber(server.arg("tempMax"), -99, 99, maximum) ||
      !parseWeatherNumber(server.arg("tempMin"), -99, 99, minimum) || minimum > maximum) {
    sendJson(400, "{\"error\":\"weather_preview_invalid\"}");
    return;
  }
  const weather_display::Data weather = {static_cast<uint16_t>(code),
      static_cast<int16_t>(maximum), static_cast<int16_t>(minimum), true, weather_display::Source::Preview};
  if (!display_service::requestWeatherPreview(weather)) {
    sendJson(409, "{\"error\":\"weather_preview_unavailable\"}");
    return;
  }
  sendJson(202, "{\"accepted\":true}");
}

void clearWeatherPreview() {
  if (!weatherPreviewAllowed()) return;
  if (!display_service::clearWeatherPreview()) {
    sendJson(409, "{\"error\":\"weather_preview_unavailable\"}");
    return;
  }
  sendJson(202, "{\"accepted\":true}");
}

void previewCalendar() {
  if (!authorized() || !calendarSelected()) return;
  if (weather_service::isBusy()) { sendJson(409, "{\"error\":\"weather_busy\"}"); return; }
  unsigned year = 0, month = 0, day = 0;
  char extra = 0;
  const String value = server.arg("date");
  if (value.length() != 10 ||
      std::sscanf(value.c_str(), "%4u-%2u-%2u%c", &year, &month, &day, &extra) != 3 ||
      year < calendar::MIN_YEAR || year > calendar::MAX_YEAR ||
      month < 1 || month > 12 || day < 1 || day > 31) {
    sendJson(400, "{\"error\":\"invalid_date\"}");
    return;
  }
  const calendar::Date date = {static_cast<uint16_t>(year),
      static_cast<uint8_t>(month), static_cast<uint8_t>(day)};
  if (!calendar::isValidDate(date)) {
    sendJson(400, "{\"error\":\"invalid_date\"}");
    return;
  }
  if (!display_service::requestPreview(date)) {
    sendJson(409, "{\"error\":\"display_unavailable\"}");
    return;
  }
  sendJson(202, "{\"accepted\":true}");
}

void restoreCurrentCalendar() {
  if (!authorized() || !calendarSelected()) return;
  if (weather_service::isBusy()) { sendJson(409, "{\"error\":\"weather_busy\"}"); return; }
  if (!display_service::requestCurrentDate()) {
    sendJson(409, "{\"error\":\"display_unavailable\"}");
    return;
  }
  sendJson(202, "{\"accepted\":true}");
}

void selectDisplayMode() {
  if (!authorized()) return;
  if (power_service::requestImageSleep()) {
    sendJson(409, "{\"error\":\"image_sleep_pending\"}");
    return;
  }
  if (closeRequested) {
    sendJson(409, "{\"error\":\"portal_closing\"}");
    return;
  }
  const String value = server.arg("mode");
  if (value != "calendar" && value != "image") {
    sendJson(400, "{\"error\":\"invalid_mode\"}");
    return;
  }
  if (clock_service::isSyncing() || weather_service::isBusy() || display_service::isBusy() || image_transfer::isReceiving()) {
    sendJson(409, "{\"error\":\"mode_busy\"}");
    return;
  }
  const display_mode::Mode mode = value == "calendar"
      ? display_mode::Mode::Calendar : display_mode::Mode::Image;
  const bool changed = display_mode::selected() != mode;
  if (!display_mode::select(mode)) {
    sendJson(409, display_mode::storageFailed()
        ? "{\"error\":\"mode_save_failed\"}" : "{\"error\":\"display_unavailable\"}");
    return;
  }
  // 主动切换模式开启对应的限时操作窗口，不靠轮询无限续期。
  if (changed) {
    openedAt = millis();
    imageOpenedAt = millis();
    if (display_mode::isCalendar()) imageWorkPending = false;
  }
  // 切回日历时复用已校时连接；只有保留时钟时仍需联网校准并获取天气。
  if (changed && display_mode::isCalendar()) {
    if (clock_service::syncStatus() == clock_service::SyncStatus::Success &&
        clock_service::timeSource() == clock_service::TimeSource::Network &&
        WiFi.status() == WL_CONNECTED) {
      weather_service::onTimeSynchronized();
    } else {
      network_settings::Credentials credentials;
      if (network_settings::load(credentials)) {
        clock_service::beginNetworkTime(credentials.ssid.c_str(),
                                        credentials.password.c_str(), true);
        previousStatus = clock_service::SyncStatus::Idle;
      }
    }
  }
  successWindow = display_mode::isCalendar() &&
      clock_service::syncStatus() == clock_service::SyncStatus::Success && !storageFailed;
  if (changed && successWindow) synchronizedAt = millis();
  sendJson(200, display_mode::isCalendar()
      ? "{\"mode\":\"calendar\"}" : "{\"mode\":\"image\"}");
}

void sendUploadError(int code, const char* error) {
  char json[96];
  std::snprintf(json, sizeof(json), "{\"error\":\"%s\"}", error);
  sendJson(code, json);
}

void receiveImage() {
  // 此路由使用原始二进制流，不把整张图片转换成 String，也不接收 multipart。
  if (server.header("Content-Type") != "application/octet-stream") {
    server.client().stop();
    return;
  }
  HTTPRaw& raw = server.raw();
  if (raw.status == RAW_START) {
    rawAccepted = false;
    rawComplete = false;
    uploadHttpStatus = 400;
    uploadError = "image_size";
    uploadStartedAt = millis();
    // 拒绝超长内容，避免解析器持续接收无界数据。
    if (server.clientContentLength() > static_cast<int>(image_transfer::PACKET_BYTES) ||
        server.clientContentLength() <= 0) {
      server.client().stop();
      return;
    }
    if (!portal_security::authenticated(server.header("X-Portal-Token"), server.client().remoteIP())) {
      uploadHttpStatus = 403;
      uploadError = "session_expired";
    } else if (display_mode::isCalendar()) {
      uploadHttpStatus = 409;
      uploadError = "image_mode_required";
    } else if (power_service::requestImageSleep()) {
      uploadHttpStatus = 409;
      uploadError = "image_sleep_pending";
    } else if (closeRequested || remainingMs() == 0) {
      uploadHttpStatus = 409;
      uploadError = "portal_closing";
    } else if (display_service::isBusy() || clock_service::isSyncing() || weather_service::isBusy()) {
      uploadHttpStatus = 409;
      uploadError = "image_busy";
    } else if (server.clientContentLength() == static_cast<int>(image_transfer::PACKET_BYTES)) {
      rawAccepted = image_transfer::begin();
      if (!rawAccepted) {
        uploadHttpStatus = 409;
        uploadError = "display_unavailable";
      }
    }
  } else if (raw.status == RAW_WRITE) {
    if (millis() - uploadStartedAt >= NetworkTimeConfig::IMAGE_UPLOAD_TIMEOUT_MS) {
      if (rawAccepted) image_transfer::abort();
      rawAccepted = false;
      rawComplete = false;
      server.client().stop();
      return;
    }
    if (rawAccepted) image_transfer::write(raw.buf, raw.currentSize);
  } else if (raw.status == RAW_END && rawAccepted) {
    rawComplete = image_transfer::finish();
    if (!rawComplete) uploadError = image_transfer::errorName();
  } else if (raw.status == RAW_ABORTED && rawAccepted) {
    image_transfer::abort();
    rawAccepted = false;
    rawComplete = false;
  }
}

void displayUploadedImage() {
  if (!authorized()) return;
  if (server.header("Content-Type") != "application/octet-stream") {
    sendUploadError(415, "image_header");
    return;
  }
  if (!rawAccepted || !rawComplete) {
    sendUploadError(uploadHttpStatus, uploadError);
    return;
  }
  rawAccepted = false;
  rawComplete = false;
  if (!image_transfer::submit()) {
    sendUploadError(409, "display_unavailable");
    return;
  }
  imageWorkPending = true;
  imageOpenedAt = millis();
  Serial.println("[image] Received 30000 pixel bytes; CRC32 passed; frame queued.");
  // 接收成功不代表刷屏成功，网页仍需轮询实际显示结果。
  sendJson(202, "{\"accepted\":true}");
}

void registerRoutes() {
  if (routesReady) return;
  const char* headers[] = {"X-Portal-Token", "Content-Type", "Origin", "Sec-Fetch-Site", "Accept-Encoding", "If-None-Match"};
  server.collectHeaders(headers, sizeof(headers) / sizeof(headers[0]));
  server.setRequestGate(requestGate);
  server.on("/", HTTP_GET, sendPage);
  server.on("/", HTTP_HEAD, sendPage);
  portal_assets::registerRoutes(server);
  server.on("/api/status", HTTP_GET, sendStatus);
  server.on("/api/login", HTTP_POST, login);
  server.on("/api/logout", HTTP_POST, []() {
    if (!authorized()) return;
    sendJson(200, "{\"closed\":true}");
    portal_security::endSession();
  });
  server.on("/api/wifi", HTTP_POST, connectNetwork);
  server.on("/api/wifi/clear", HTTP_POST, clearNetwork);
  server.on("/api/resync", HTTP_POST, resyncNetwork);
  server.on("/api/weather/settings", HTTP_GET, readWeatherSettings);
  server.on("/api/weather/settings", HTTP_POST, saveWeatherSettings);
  server.on("/api/weather/key/clear", HTTP_POST, clearWeatherKey);
  server.on("/api/weather/cities/search", HTTP_POST, searchWeatherCities);
  server.on("/api/weather/cities/status", HTTP_GET, sendCitySearchStatus);
  server.on("/api/weather/preview", HTTP_POST, previewWeather);
  server.on("/api/weather/preview/clear", HTTP_POST, clearWeatherPreview);
  server.on("/api/display/mode", HTTP_POST, selectDisplayMode);
  server.on("/api/image", HTTP_POST, displayUploadedImage, receiveImage);
  server.on("/api/calendar/preview", HTTP_POST, previewCalendar);
  server.on("/api/calendar/today", HTTP_POST, restoreCurrentCalendar);
  calendar_events_portal::registerRoutes(server, readEventsAllowed, writeEventsAllowed);
  server.on("/api/close", HTTP_POST, []() {
    if (!authorized()) return;
    if (weather_service::isBusy()) { sendJson(409, "{\"error\":\"weather_busy\"}"); return; }
    if (!display_mode::isCalendar() && display_service::isBusy()) {
      sendUploadError(409, "image_busy");
      return;
    }
    sendJson(200, "{\"closed\":true}");
    closeRequested = true;  // 先发完响应，再在主循环关闭热点。
    closeRequestedAt = millis();
  });
  server.onNotFound([]() {
    if (server.uri().startsWith("/api/")) {
      sendJson(404, "{\"error\":\"not_found\"}");
    } else if (server.method() == HTTP_GET || server.method() == HTTP_HEAD) {
      redirectToPortal();
    } else {
      server.sendHeader("Cache-Control", "no-store");
      server.send(404, "text/plain; charset=utf-8", "Not found");
    }
  });
  routesReady = true;
}

void processResult() {
  const clock_service::SyncStatus current = clock_service::syncStatus();
  if (current == previousStatus) return;
  previousStatus = current;
  if (clock_service::isSyncing()) return;

  if (current == clock_service::SyncStatus::Success) {
    struct tm synchronized = {};
    if (clock_service::readLocalTime(synchronized)) {
      strftime(lastSync, sizeof(lastSync), "%Y-%m-%d %H:%M:%S", &synchronized);
    }
    if (savePending) {
      const bool saved = network_settings::save(pendingCredentials);
      storageFailed = !saved;
      if (saved) hasSavedNetwork = true;
      Serial.println(saved ? "[portal] Network configuration saved."
                           : "[portal] ERROR: configuration save failed.");
    }
    if (!storageFailed) {
      synchronizedAt = millis();
      successWindow = true;
    }
    Serial.printf("[portal] Beijing time: %s\n", lastSync);
  }
  savePending = false;
  secure_memory::clear(pendingCredentials.password);
  pendingCredentials = {};
}

void closePortal() {
  server.stop();
  dns.stop();
  active = false;
  rawAccepted = false;
  rawComplete = false;
  imageWorkPending = false;
  portal_security::endSession();
  secure_memory::clear(pendingCredentials.password);
  pendingCredentials = {};
  clock_service::stopNetwork();
  image_transfer::reset();
  display_mode::cancelPendingImage();
  Serial.println(display_mode::isCalendar()
      ? "[portal] Configuration closed; sleep after rendering. Reset to reopen."
      : "[portal] Image retained; preparing deep sleep. Reset to reopen configuration.");
}

void processImageResult() {
  if (!imageWorkPending || display_service::isBusy()) return;
  imageWorkPending = false;
  if (power_service::requestImageSleep()) {
    Serial.println("[image] Refresh succeeded; image locked for deep sleep.");
    return;
  }
  imageOpenedAt = millis();
  Serial.println("[image] Refresh or mode save incomplete; configuration remains available for retry.");
}

}  // namespace

namespace wifi_portal {

bool begin() {
  if (active) return true;
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  if (!WiFi.mode(display_mode::isCalendar() ? WIFI_AP_STA : WIFI_AP) ||
      !portal_security::begin() ||
      !WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                          IPAddress(255, 255, 255, 0)) ||
      !WiFi.softAP(portal_security::ssid(), portal_security::hotspotPassword(), 1, false, 1)) {
    clock_service::stopNetwork();
    Serial.println("[portal] ERROR: failed to start configuration hotspot.");
    return false;
  }

  writeWindowAt = millis();
  writeCount = 0;
  registerRoutes();
  server.begin();
  dns.start(53, "*", WiFi.softAPIP());
  active = true;
  closeRequested = false;
  savePending = false;
  storageFailed = false;
  successWindow = false;
  openedAt = millis();
  imageOpenedAt = millis();
  Serial.printf("[portal] Hotspot: %s; page: http://192.168.4.1\n",
                portal_security::ssid());

  network_settings::Credentials credentials;
  hasSavedNetwork = network_settings::load(credentials);
  previousStatus = clock_service::SyncStatus::Idle;
  if (hasSavedNetwork && display_mode::isCalendar()) {
    clock_service::beginNetworkTime(credentials.ssid.c_str(),
                                    credentials.password.c_str(), true);
  }
  return true;
}

void handle() {
  if (!active) return;
  processImageResult();
  const bool imageBusy = !display_mode::isCalendar() && display_service::isBusy();
  // 为“关闭配网”的 HTTP 响应留出短暂发送时间，避免先断开无线。
  if (!imageBusy && !weather_service::isBusy() && ((closeRequested && millis() - closeRequestedAt >= 300) ||
      remainingMs() == 0)) {
    closePortal();
    return;
  }
  dns.processNextRequest();
  server.handleClient();
  clock_service::pollNetworkTime();
  processResult();
}

bool isActive() {
  return active;
}

bool close() {
  if (display_service::isBusy() || image_transfer::isReceiving() || weather_service::isBusy()) return false;
  if (active) closePortal();
  return true;
}

void printLocalCredentials(Print& output) {
  if (!active) {
    output.println("[portal] Hotspot inactive. Reset the board, then send P while it is awake.");
    return;
  }
  portal_security::printLocalCredentials(output);
}

}  // namespace wifi_portal
