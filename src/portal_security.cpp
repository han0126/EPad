#include "portal_security.h"

#include <Preferences.h>
#include <WiFi.h>
#include <esp_system.h>
#include <cstdio>
#include <cstring>

#include "secure_memory.h"

namespace {

constexpr char ALPHABET[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
static_assert(sizeof(ALPHABET) == 33, "Random secrets require a 32-character alphabet");
constexpr uint32_t LOCKOUT_MS = 60000;
struct Record {
  uint32_t version;
  char hotspot[17];
  char admin[21];
};
Record credentials = {};
char hotspotName[32] = {};
char session[33] = {};
IPAddress sessionClient;
bool ready = false;
uint8_t failures = 0;
uint32_t lockedAt = 0;

template <size_t N> void generate(char (&output)[N]) {
  uint8_t random[N - 1];
  esp_fill_random(random, sizeof(random));
  for (size_t i = 0; i < sizeof(random); ++i) output[i] = ALPHABET[random[i] & 31];
  output[N - 1] = '\0';
  secure_memory::wipe(random, sizeof(random));
}

bool validSecret(const char* value, size_t length) {
  if (value[length] != '\0') return false;
  for (size_t i = 0; i < length; ++i) {
    if (value[i] == '\0' || std::strchr(ALPHABET, value[i]) == nullptr) return false;
  }
  return true;
}

}  // namespace

namespace portal_security {

bool begin() {
  endSession();
  if (ready) return true;
  Preferences preferences;
  if (!preferences.begin("epd-portal", false)) return false;
  const bool exists = preferences.isKey("credentials");
  bool valid = false;
  if (exists) {
    valid = preferences.getBytesLength("credentials") == sizeof(credentials) &&
        preferences.getBytes("credentials", &credentials, sizeof(credentials)) == sizeof(credentials) &&
        credentials.version == 1 && validSecret(credentials.hotspot, 16) && validSecret(credentials.admin, 20);
  } else {
    credentials.version = 1;
    generate(credentials.hotspot);
    generate(credentials.admin);
    valid = preferences.putBytes("credentials", &credentials, sizeof(credentials)) == sizeof(credentials);
  }
  preferences.end();
  if (!valid) {
    secure_memory::wipe(&credentials, sizeof(credentials));
    return false;
  }
  uint8_t mac[6];
  WiFi.softAPmacAddress(mac);
  std::snprintf(hotspotName, sizeof(hotspotName), "EPD-Calendar-%02X%02X%02X",
      static_cast<unsigned>(mac[3]), static_cast<unsigned>(mac[4]), static_cast<unsigned>(mac[5]));
  ready = true;
  if (!exists) {
    printLocalCredentials(Serial);
#if ARDUINO_USB_CDC_ON_BOOT
    printLocalCredentials(Serial0);
#endif
  }
  return true;
}

const char* ssid() { return hotspotName; }
const char* hotspotPassword() { return credentials.hotspot; }

void printLocalCredentials(Print& output) {
  if (!ready) {
    output.println("[portal] Credentials unavailable; check hotspot initialization.");
    return;
  }
  output.println("[portal] Local provisioning credentials (keep private):");
  output.printf("SSID: %s\nHotspot password: %s\nAdmin code: %s\n",
      hotspotName, credentials.hotspot, credentials.admin);
}

bool loginAllowed() {
  if (failures < 5) return ready;
  if (millis() - lockedAt < LOCKOUT_MS) return false;
  failures = 0;
  return ready;
}

bool login(const String& password, IPAddress client) {
  if (!loginAllowed()) return false;
  if (password.length() != 20 || !password.equalsConstantTime(credentials.admin)) {
    if (++failures >= 5) lockedAt = millis();
    return false;
  }
  uint8_t bytes[16];
  esp_fill_random(bytes, sizeof(bytes));
  constexpr char DIGITS[] = "0123456789abcdef";
  for (size_t i = 0; i < sizeof(bytes); ++i) {
    session[i * 2] = DIGITS[bytes[i] >> 4];
    session[i * 2 + 1] = DIGITS[bytes[i] & 15];
  }
  session[32] = '\0';
  secure_memory::wipe(bytes, sizeof(bytes));
  sessionClient = client;
  failures = 0;
  return true;
}

bool authenticated(const String& value, IPAddress client) {
  return ready && session[0] != '\0' && client == sessionClient &&
      value.length() == 32 && value.equalsConstantTime(session);
}

const char* token() { return session; }
void endSession() {
  secure_memory::wipe(session, sizeof(session));
  sessionClient = IPAddress();
}

}  // namespace portal_security
