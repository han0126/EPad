#include "wifi_portal.h"

#include <Arduino.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_system.h>

#include <cstdio>
#include <cstring>

#include "display_service.h"
#include "clock_service.h"
#include "display_mode.h"
#include "image_transfer.h"
#include "network_settings.h"
#include "network_time_config.h"
#include "power_service.h"

// PlatformIO 将独立 HTML 文件直接嵌入固件，无需上传文件系统。
extern const uint8_t WIFI_SETUP_HTML[] asm("_binary_web_wifi_setup_html_start");
extern const uint8_t IMAGE_TOOLS_JS[] asm("_binary_web_image_tools_js_start");

namespace {

WebServer server(IPAddress(192, 168, 4, 1), 80);
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
char sessionToken[17] = {};
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
  if (server.header("X-Portal-Token") == sessionToken) return true;
  sendJson(403, "{\"error\":\"session_expired\"}");
  return false;
}

bool calendarSelected() {
  if (display_mode::isCalendar()) return true;
  sendJson(409, "{\"error\":\"wrong_mode\"}");
  return false;
}

void sendPage() {
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html; charset=utf-8",
                reinterpret_cast<const char*>(WIFI_SETUP_HTML));
}

void sendImageTools() {
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "application/javascript; charset=utf-8",
                reinterpret_cast<const char*>(IMAGE_TOOLS_JS));
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
  if (display.state == display_service::State::Ready && display.image) return "ready";
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
  power_service::requestImageSleep();
  struct tm current = {};
  const bool valid = clock_service::readLocalTime(current);
  char timeText[16] = {};
  char dateText[16] = {};
  if (valid) {
    strftime(timeText, sizeof(timeText), "%H:%M:%S", &current);
    strftime(dateText, sizeof(dateText), "%Y-%m-%d", &current);
  }

  // JSON 仅包含固定状态和设备生成的时间，不回传 Wi-Fi 名称或密码。
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
  const bool calendarMode = display_mode::isCalendar();
  char json[1024];
  std::snprintf(json, sizeof(json),
      "{\"state\":\"%s\",\"valid\":%s,\"time\":\"%s\",\"date\":\"%s\","
      "\"weekday\":%d,\"lastSync\":\"%s\",\"saved\":%s,\"connected\":%s,"
      "\"remaining\":%lu,\"token\":\"%s\","
      "\"calendar\":{\"state\":\"%s\",\"error\":\"%s\",\"date\":\"%s\","
      "\"target\":\"%s\",\"preview\":%s,\"rows\":%u},"
      "\"power\":{\"timerWake\":%s,\"nextWake\":\"%s\",\"timeSource\":\"%s\","
      "\"imageSleepPending\":%s,\"sleepIn\":%lu},"
      "\"display\":{\"mode\":\"%s\",\"imageState\":\"%s\",\"imageError\":\"%s\","
      "\"uploadEnabled\":%s}}",
      stateName(), valid ? "true" : "false", timeText, dateText,
      valid ? current.tm_wday : 0, lastSync,
      hasSavedNetwork ? "true" : "false",
      WiFi.status() == WL_CONNECTED ? "true" : "false",
      static_cast<unsigned long>((remainingMs() + 999) / 1000), sessionToken,
      calendarState(display.state), calendarError(display.error), displayedDate,
      requestedDate, display.preview ? "true" : "false", static_cast<unsigned>(display.rows),
      power.timerWake ? "true" : "false", nextWake, sourceName,
      power.imageSleepPending ? "true" : "false",
      static_cast<unsigned long>(power.sleepInSeconds),
      calendarMode ? "calendar" : "image", imageState(display),
      image_transfer::error() != image_transfer::Error::None
          ? image_transfer::errorName() :
          ((display.imageRequest || display.error == display_service::Error::TaskStartFailed)
              ? calendarError(display.error) : ""),
      display.error == display_service::Error::TaskStartFailed || power.imageSleepPending
          ? "false" : "true");
  sendJson(200, json);
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
  if (clock_service::isSyncing()) {
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
  if (clock_service::isSyncing()) {
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

void previewCalendar() {
  if (!authorized() || !calendarSelected()) return;
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
  if (clock_service::isSyncing() || display_service::isBusy() || image_transfer::isReceiving()) {
    sendJson(409, "{\"error\":\"mode_busy\"}");
    return;
  }
  const display_mode::Mode mode = value == "calendar"
      ? display_mode::Mode::Calendar : display_mode::Mode::Image;
  const bool changed = display_mode::selected() != mode;
  if (!display_mode::select(mode)) {
    sendJson(409, "{\"error\":\"display_unavailable\"}");
    return;
  }
  // 主动切换模式开启对应的限时操作窗口，不靠轮询无限续期。
  if (changed) {
    openedAt = millis();
    imageOpenedAt = millis();
    if (display_mode::isCalendar()) imageWorkPending = false;
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
    if (server.header("X-Portal-Token") != sessionToken) {
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
    } else if (display_service::isBusy() || clock_service::isSyncing()) {
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
  const char* headers[] = {"X-Portal-Token", "Content-Type"};
  server.collectHeaders(headers, 2);
  server.on("/", HTTP_GET, sendPage);
  server.on("/image-tools.js", HTTP_GET, sendImageTools);
  server.on("/api/status", HTTP_GET, sendStatus);
  server.on("/api/wifi", HTTP_POST, connectNetwork);
  server.on("/api/resync", HTTP_POST, resyncNetwork);
  server.on("/api/display/mode", HTTP_POST, selectDisplayMode);
  server.on("/api/image", HTTP_POST, displayUploadedImage, receiveImage);
  server.on("/api/calendar/preview", HTTP_POST, previewCalendar);
  server.on("/api/calendar/today", HTTP_POST, restoreCurrentCalendar);
  server.on("/api/close", HTTP_POST, []() {
    if (!authorized()) return;
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
    } else {
      sendPage();
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
  pendingCredentials = {};
}

void closePortal() {
  server.stop();
  dns.stop();
  active = false;
  rawAccepted = false;
  rawComplete = false;
  imageWorkPending = false;
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
  Serial.println("[image] Refresh failed; configuration remains available for retry.");
}

}  // namespace

namespace wifi_portal {

bool begin() {
  if (active) return true;
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  if (!WiFi.mode(WIFI_AP_STA) ||
      !WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                          IPAddress(255, 255, 255, 0)) ||
      !WiFi.softAP(NetworkTimeConfig::PORTAL_SSID,
                    NetworkTimeConfig::PORTAL_PASSWORD)) {
    clock_service::stopNetwork();
    Serial.println("[portal] ERROR: failed to start configuration hotspot.");
    return false;
  }

  std::snprintf(sessionToken, sizeof(sessionToken), "%08lx%08lx",
                static_cast<unsigned long>(esp_random()),
                static_cast<unsigned long>(esp_random()));
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
                NetworkTimeConfig::PORTAL_SSID);

  network_settings::Credentials credentials;
  hasSavedNetwork = network_settings::load(credentials);
  previousStatus = clock_service::SyncStatus::Idle;
  if (hasSavedNetwork) {
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
  if (!imageBusy && ((closeRequested && millis() - closeRequestedAt >= 300) ||
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
  if (display_service::isBusy() || image_transfer::isReceiving()) return false;
  if (active) closePortal();
  return true;
}

}  // namespace wifi_portal
