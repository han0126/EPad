#include <Arduino.h>

#include "calendar_events.h"
#include "display_service.h"
#include "display_mode.h"
#include "clock_service.h"
#include "jd79668_driver.h"
#include "power_service.h"
#include "wifi_portal.h"
#include "weather_service.h"

namespace {

void handleConsole(Stream& port) {
  while (port.available()) {
    const char command = static_cast<char>(port.read());
    if (command == 'p' || command == 'P') {
      // 在接收命令的同一串口答复，兼容原生 USB 和 USB 转 UART 接口。
      port.println("[console] P received.");
      wifi_portal::printLocalCredentials(port);
    } else if (command == 'w' || command == 'W') {
      const bool opened = power_service::openConfiguration();
      port.println(opened ? "[console] Configuration hotspot open. Send P for credentials."
                          : "[console] Cannot open hotspot now; reset the board and retry.");
    }
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  // 当前板型的 Serial 是 USB CDC；UART0 使用 TX=43、RX=44。
  Serial0.begin(115200);
#endif
  delay(300);
  Serial.println();
  Serial.println("JD79668 400x300: daily calendar / image deep sleep");
#if ARDUINO_USB_CDC_ON_BOOT
  Serial0.println("[console] UART0 ready at 115200. Send P for credentials, W to open configuration.");
#endif

  jd79668::beginPins();

  // 没有有效时间时保留屏幕原画面，不把旧演示日期当作当前日期。
  display_mode::begin();
  calendar_events::begin();
  weather_service::begin();
  display_service::begin();
  power_service::begin();
  Serial.println("[power] Calendar wakes daily; images sleep until reset. W works before image completion.");
#if ARDUINO_USB_CDC_ON_BOOT
  Serial0.println(wifi_portal::isActive() ? "[console] Hotspot ready. Send P now."
                                        : "[console] Hotspot inactive. Send W to retry configuration.");
#endif
}

void loop() {
  // 深睡时串口不能唤醒；按复位键配网，清醒期间仍支持串口 W。
  handleConsole(Serial);
#if ARDUINO_USB_CDC_ON_BOOT
  handleConsole(Serial0);
#endif
  wifi_portal::handle();
  if (!wifi_portal::isActive()) clock_service::pollNetworkTime();
  weather_service::handle();
  display_service::handle();
  power_service::handle();
  delay(wifi_portal::isActive() ? 5 : 20);
}
