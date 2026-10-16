#include "display_mode.h"

#include <Arduino.h>
#include <Preferences.h>

#include "display_service.h"
#include "image_transfer.h"

namespace {

constexpr char STORAGE_NAMESPACE[] = "epd-display";
constexpr char MODE_KEY[] = "mode";
constexpr uint32_t SAVE_RETRY_MS = 5000;

display_mode::Mode current = display_mode::Mode::Calendar;
display_mode::Mode remembered = display_mode::Mode::Calendar;
bool saveFailed = false;
uint32_t lastSaveAttempt = 0;

bool saveMode(display_mode::Mode mode) {
  // 模式未改变不写 Flash，日常唤醒只读取。
  if (mode == remembered) {
    saveFailed = false;
    return true;
  }
  if (saveFailed && millis() - lastSaveAttempt < SAVE_RETRY_MS) return false;
  lastSaveAttempt = millis();
  Preferences preferences;
  bool saved = false;
  if (preferences.begin(STORAGE_NAMESPACE, false)) {
    saved = preferences.putUChar(MODE_KEY, static_cast<uint8_t>(mode)) == 1;
    preferences.end();
  }
  saveFailed = !saved;
  if (saved) {
    remembered = mode;
    Serial.println(mode == display_mode::Mode::Calendar
        ? "[mode] Calendar mode saved." : "[mode] Image mode saved.");
  } else {
    Serial.println("[mode] ERROR: mode save failed; retry later.");
  }
  return saved;
}

}  // namespace

namespace display_mode {

void begin() {
  current = remembered = Mode::Calendar;
  saveFailed = false;
  lastSaveAttempt = 0;
  Preferences preferences;
  if (preferences.begin(STORAGE_NAMESPACE, true)) {
    const uint8_t value = preferences.getUChar(MODE_KEY, static_cast<uint8_t>(Mode::Calendar));
    preferences.end();
    if (value == static_cast<uint8_t>(Mode::Image)) current = remembered = Mode::Image;
  }
  Serial.println(isCalendar() ? "[mode] Restored calendar mode."
                             : "[mode] Restored image mode; keeping panel contents.");
}

Mode selected() {
  return current;
}

bool isCalendar() {
  return current == Mode::Calendar;
}

bool hasRetainedImage() {
  return current == Mode::Image && remembered == Mode::Image;
}

bool storageFailed() {
  return saveFailed;
}

bool select(Mode mode) {
  if (mode != Mode::Calendar && mode != Mode::Image) return false;
  if (mode == current) return true;
  if (display_service::isBusy() || image_transfer::isReceiving()) return false;
  if (mode == Mode::Calendar) {
    if (!display_service::isAvailable() || !saveMode(mode)) return false;
  }
  current = mode;
  if (isCalendar() && !display_service::requestCurrentDate()) {
    // 模式已保存；日期轮询会继续安排刷新，不回退到图片模式。
    Serial.println("[mode] Calendar selected; rendering will retry in the main loop.");
  }
  image_transfer::reset();
  Serial.println(isCalendar() ? "[mode] Calendar selected."
                             : "[mode] Image selected; waiting for upload.");
  return true;
}

bool saveDisplayedImage() {
  const display_service::Status display = display_service::status();
  if (isCalendar() || display.state != display_service::State::Ready ||
      display.error != display_service::Error::None || !display.image) return false;
  return saveMode(Mode::Image);
}

void cancelPendingImage() {
  if (isCalendar()) return;
  // 断电恢复的照片没有内存帧，不能据此判定选图失败并切回日历。
  if (hasRetainedImage()) return;
  const display_service::Status display = display_service::status();
  // 已成功显示图片时保留画面，由电源服务安排无自动唤醒的深睡。
  if (display_service::isBusy() ||
      (display.state == display_service::State::Ready && display.image)) return;
  current = Mode::Calendar;
  image_transfer::reset();
  // 不清屏、不修改时间；未上传图片就退出时，恢复原日历调度。
  display_service::requestCurrentDate();
  Serial.println("[mode] Pending image selection cancelled; calendar resumed.");
}

}  // namespace display_mode
