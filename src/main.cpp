#include <Arduino.h>

#include "epd_driver.h"
#include "calendar_renderer.h"

namespace {

bool showCalendarDemo() {
  if (!epd::initialize()) return false;

  Serial.println("Sending 400x300 four-color static calendar...");
  sendCalendarFrame();
  if (!epd::refresh()) return false;

  epd::sleep();
  return true;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("JD79668 4.2-inch 400x300 four-color static calendar demo");

  epd::beginPins();

  if (showCalendarDemo()) {
    Serial.println("Done. The panel is now sleeping.");
  } else {
    Serial.println("Demo stopped because the panel did not become ready.");
  }
}

void loop() {
  // Intentionally empty: repeatedly refreshing an e-paper panel is unnecessary.
}
