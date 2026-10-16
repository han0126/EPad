#include <Arduino.h>

#include "display_service.h"
#include "display_mode.h"
#include "clock_service.h"
#include "jd79668_driver.h"
#include "power_service.h"
#include "wifi_portal.h"
#include "weather_service.h"

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("JD79668 400x300: daily calendar / image deep sleep");

  jd79668::beginPins();

  // 没有有效时间时保留屏幕原画面，不把旧演示日期当作当前日期。
  display_mode::begin();
  weather_service::begin();
  display_service::begin();
  power_service::begin();
  Serial.println("[power] Calendar wakes daily; images sleep until reset. W works before image completion.");
}

void loop() {
  // 深睡时串口不能唤醒；按复位键配网，清醒期间仍支持串口 W。
  while (Serial.available()) {
    const char command = static_cast<char>(Serial.read());
    if (command == 'w' || command == 'W') power_service::openConfiguration();
  }
  wifi_portal::handle();
  if (!wifi_portal::isActive()) clock_service::pollNetworkTime();
  weather_service::handle();
  display_service::handle();
  power_service::handle();
  delay(wifi_portal::isActive() ? 5 : 20);
}
