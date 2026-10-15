#include "display_mode.h"

#include <Arduino.h>

#include "display_service.h"
#include "image_transfer.h"

namespace {

// 模式不写入 Flash；复位默认日历，图片深睡标志由电源服务管理。
display_mode::Mode current = display_mode::Mode::Calendar;

}  // namespace

namespace display_mode {

Mode selected() {
  return current;
}

bool isCalendar() {
  return current == Mode::Calendar;
}

bool select(Mode mode) {
  if (mode != Mode::Calendar && mode != Mode::Image) return false;
  if (mode == current) return true;
  if (display_service::isBusy() || image_transfer::isReceiving()) return false;
  const Mode previous = current;
  current = mode;
  if (isCalendar() && !display_service::requestCurrentDate()) {
    current = previous;
    return false;
  }
  image_transfer::reset();
  Serial.println(isCalendar() ? "[mode] Calendar selected."
                             : "[mode] Image selected; waiting for upload.");
  return true;
}

void cancelPendingImage() {
  if (isCalendar()) return;
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
