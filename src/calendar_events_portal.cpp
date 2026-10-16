#include "calendar_events_portal.h"

#include <ArduinoJson.h>
#include <WebServer.h>

#include <cstdio>

#include "calendar_events.h"

namespace {

void sendError(WebServer& server, int status, const char* code) {
  char body[96];
  std::snprintf(body, sizeof(body), "{\"error\":\"%s\"}", code);
  server.sendHeader("Cache-Control", "no-store");
  server.send(status, "application/json; charset=utf-8", body);
}

void sendState(WebServer& server, uint32_t savedId = 0) {
  DynamicJsonDocument json(12288);
  json["ready"] = calendar_events::isReady();
  json["revision"] = calendar_events::revision();
  json["capacity"] = calendar_events::MAX_ENTRIES;
  json["savedId"] = savedId;
  JsonArray entries = json.createNestedArray("entries");
  for (size_t i = 0; i < calendar_events::count(); ++i) {
    const calendar_events::Entry& item = *calendar_events::entry(i);
    char date[16];
    std::snprintf(date, sizeof(date), "%04u-%02u-%02u", static_cast<unsigned>(item.date.year),
        static_cast<unsigned>(item.date.month), static_cast<unsigned>(item.date.day));
    JsonObject value = entries.createNestedObject();
    value["id"] = item.id;
    value["kind"] = item.kind == calendar_events::Kind::Important ? "important" : "memo";
    value["date"] = date;
    value["text"] = item.text;
  }
  if (json.overflowed()) { sendError(server, 503, "status_memory"); return; }
  String body;
  if (!body.reserve(measureJson(json) + 1)) { sendError(server, 503, "status_memory"); return; }
  serializeJson(json, body);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json; charset=utf-8", body);
}

bool parseUnsigned(const String& input, uint32_t& value) {
  if (input.length() == 0 || input.length() > 10) return false;
  value = 0;
  for (size_t i = 0; i < input.length(); ++i) {
    const char digit = input[i];
    if (digit < '0' || digit > '9' || value > (UINT32_MAX - (digit - '0')) / 10) return false;
    value = value * 10 + digit - '0';
  }
  return true;
}

bool parseDate(const String& input, calendar::Date& date) {
  if (input.length() != 10 || input[4] != '-' || input[7] != '-') return false;
  unsigned values[3] = {};
  size_t field = 0;
  for (size_t i = 0; i < input.length(); ++i) {
    if (i == 4 || i == 7) { ++field; continue; }
    if (input[i] < '0' || input[i] > '9') return false;
    values[field] = values[field] * 10 + input[i] - '0';
  }
  date = {static_cast<uint16_t>(values[0]), static_cast<uint8_t>(values[1]),
      static_cast<uint8_t>(values[2])};
  return calendar::isValidDate(date);
}

void finish(WebServer& server, calendar_events::Error result, uint32_t savedId = 0) {
  if (result == calendar_events::Error::None) { sendState(server, savedId); return; }
  const int status = result == calendar_events::Error::Storage ? 503 :
      (result == calendar_events::Error::Conflict || result == calendar_events::Error::Stale ||
       result == calendar_events::Error::Full || result == calendar_events::Error::NotFound ? 409 : 400);
  sendError(server, status, calendar_events::errorName(result));
}

}  // namespace

namespace calendar_events_portal {

void registerRoutes(WebServer& server, bool (*readAllowed)(), bool (*writeAllowed)()) {
  server.on("/api/calendar/events", HTTP_GET, [&server, readAllowed]() {
    if (readAllowed()) sendState(server);
  });
  server.on("/api/calendar/events", HTTP_POST, [&server, writeAllowed]() {
    if (!writeAllowed()) return;
    uint32_t id = 0, revision = 0;
    if (!parseUnsigned(server.arg("id"), id) || !parseUnsigned(server.arg("revision"), revision)) {
      sendError(server, 400, "events_request");
      return;
    }
    const String kind = server.arg("kind");
    if (kind != "important" && kind != "memo") { sendError(server, 400, "events_kind"); return; }
    calendar::Date date = {};
    if (!parseDate(server.arg("date"), date)) { sendError(server, 400, "events_date"); return; }
    uint32_t savedId = 0;
    const calendar_events::Error result = calendar_events::upsert(id, revision,
        kind == "important" ? calendar_events::Kind::Important : calendar_events::Kind::Memo,
        date, server.arg("text"), savedId);
    finish(server, result, savedId);
  });
  server.on("/api/calendar/events/delete", HTTP_POST, [&server, writeAllowed]() {
    if (!writeAllowed()) return;
    uint32_t id = 0, revision = 0;
    if (!parseUnsigned(server.arg("id"), id) || id == 0 ||
        !parseUnsigned(server.arg("revision"), revision)) {
      sendError(server, 400, "events_request");
      return;
    }
    finish(server, calendar_events::remove(id, revision));
  });
}

}  // namespace calendar_events_portal
