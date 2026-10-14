#include <Arduino.h>

#include "calendar_data.h"
#include "calendar_view.h"
#include "epd_canvas.h"
#include "jd79668_driver.h"

namespace {

// 画布占 30 KB，使用静态存储，避免挤占 ESP32 任务栈。
EpdCanvas displayCanvas;

Color4 readDisplayPixel(uint16_t x, uint16_t y) {
  return displayCanvas.pixel(x, y);
}

bool showCalendarDemo() {
  if (!jd79668::initialize()) return false;

  // 绘图、发送和物理刷新分开；读取像素时不再触发绘图。
  calendar::drawMonth(displayCanvas, calendar::demoMonth());
  Serial.println("Sending 400x300 four-color static calendar...");
  jd79668::sendFrame(readDisplayPixel);
  if (!jd79668::refresh()) return false;

  jd79668::sleep();
  return true;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("JD79668 4.2-inch 400x300 four-color static calendar demo");

  jd79668::beginPins();

  if (showCalendarDemo()) {
    Serial.println("Done. The panel is now sleeping.");
  } else {
    Serial.println("Demo stopped because the panel did not become ready.");
  }
}

void loop() {
  // 静态画面只刷新一次；屏幕休眠后仍会保留内容。
}
