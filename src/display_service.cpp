#include "display_service.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "calendar_view.h"
#include "clock_service.h"
#include "display_mode.h"
#include "epd_canvas.h"
#include "jd79668_driver.h"

namespace {

struct Request {
  calendar::Date date;
  bool preview;
  const uint8_t* imageFrame;
};

EpdCanvas canvas;
QueueHandle_t requests = nullptr;
portMUX_TYPE statusLock = portMUX_INITIALIZER_UNLOCKED;
display_service::Status displayStatus = {
    display_service::State::WaitingForTime, display_service::Error::None,
    {}, {}, false, 0, false, false};
bool previewMode = false;
calendar::Date lastAttempt = {};
uint32_t lastClockPoll = 0;

Color4 readPixel(uint16_t x, uint16_t y) {
  return canvas.pixel(x, y);
}

void yieldCalendar() {
  vTaskDelay(1);
}

void displayWorker(void*) {
  Request request;
  for (;;) {
    if (xQueueReceive(requests, &request, portMAX_DELAY) != pdTRUE) continue;
    portENTER_CRITICAL(&statusLock);
    displayStatus.state = display_service::State::Drawing;
    portEXIT_CRITICAL(&statusLock);

    calendar::MonthData month = {};
    display_service::Error error = display_service::Error::None;
    uint8_t rows = 0;
    const bool image = request.imageFrame != nullptr;
    if (image) {
      if (!canvas.loadPackedFrame(request.imageFrame, EPD_FRAME_BYTES)) {
        error = display_service::Error::InvalidFrame;
      }
      Serial.println("[image] Rendering validated four-color frame.");
    } else if (!calendar::buildMonth(request.date, month, yieldCalendar)) {
      error = display_service::Error::CalendarFailed;
    } else {
      rows = (month.firstWeekday + month.dayCount + 6) / 7;
      calendar::drawMonth(canvas, month);
      Serial.printf("[calendar] Rendering %04u-%02u-%02u, rows=%u, lunar=%s %s%s\n",
                    static_cast<unsigned>(request.date.year),
                    static_cast<unsigned>(request.date.month),
                    static_cast<unsigned>(request.date.day), static_cast<unsigned>(rows),
                    month.lunarYear, month.lunarDate, request.preview ? " [preview]" : "");
    }
    if (error == display_service::Error::None) {
      if (!jd79668::initialize()) {
        error = display_service::Error::PanelFailed;
      } else {
        jd79668::sendFrame(readPixel);
        if (!jd79668::refresh()) error = display_service::Error::PanelFailed;
        if (!jd79668::sleep()) error = display_service::Error::PanelFailed;
      }
    }

    portENTER_CRITICAL(&statusLock);
    displayStatus.error = error;
    displayStatus.state = error == display_service::Error::None
        ? display_service::State::Ready : display_service::State::Error;
    if (error == display_service::Error::None) {
      displayStatus.displayedDate = request.date;
      displayStatus.rows = rows;
      displayStatus.image = image;
    }
    portEXIT_CRITICAL(&statusLock);
    if (image) {
      Serial.println(error == display_service::Error::None
                         ? "[image] Frame ready; panel sleeping."
                         : "[image] Render failed; retry from configuration.");
    } else {
      Serial.println(error == display_service::Error::None
                         ? "[calendar] Frame ready; panel sleeping."
                         : "[calendar] Render failed; hourly retry or retry from configuration.");
    }
  }
}

bool readCurrentDate(calendar::Date& date) {
  struct tm now = {};
  if (!clock_service::readLocalTime(now)) return false;
  date = {static_cast<uint16_t>(now.tm_year + 1900),
          static_cast<uint8_t>(now.tm_mon + 1), static_cast<uint8_t>(now.tm_mday)};
  return calendar::isValidDate(date);
}

bool enqueue(const Request& request) {
  if (requests == nullptr || display_service::isBusy()) return false;
  // 状态先置为排队，避免工作任务已开始后又被主任务写回排队状态。
  portENTER_CRITICAL(&statusLock);
  const display_service::Status previous = displayStatus;
  displayStatus.state = display_service::State::Queued;
  displayStatus.error = display_service::Error::None;
  displayStatus.requestedDate = request.date;
  displayStatus.preview = request.preview;
  displayStatus.imageRequest = request.imageFrame != nullptr;
  portEXIT_CRITICAL(&statusLock);
  if (xQueueSend(requests, &request, 0) != pdTRUE) {
    portENTER_CRITICAL(&statusLock);
    displayStatus = previous;
    portEXIT_CRITICAL(&statusLock);
    return false;
  }
  previewMode = request.preview;
  if (request.imageFrame == nullptr) lastAttempt = request.date;
  return true;
}

}  // namespace

namespace display_service {

bool begin() {
  if (requests != nullptr) return true;
  requests = xQueueCreate(1, sizeof(Request));
  // 日历和图片共用同一任务，画布、字体及屏幕驱动不会并发访问。
  if (requests == nullptr ||
      xTaskCreatePinnedToCore(displayWorker, "epd-display", 16384, nullptr,
                              1, nullptr, 1) != pdPASS) {
    if (requests != nullptr) vQueueDelete(requests);
    requests = nullptr;
    portENTER_CRITICAL(&statusLock);
    displayStatus.state = State::Error;
    displayStatus.error = Error::TaskStartFailed;
    portEXIT_CRITICAL(&statusLock);
    Serial.println("[display] ERROR: failed to start display task.");
    return false;
  }
  return true;
}

Status status() {
  portENTER_CRITICAL(&statusLock);
  const Status snapshot = displayStatus;
  portEXIT_CRITICAL(&statusLock);
  return snapshot;
}

bool isBusy() {
  const State state = status().state;
  return state == State::Queued || state == State::Drawing;
}

bool restoreRetainedFrame(const calendar::Date& date, uint8_t rows) {
  if (requests == nullptr || isBusy() || !calendar::isValidDate(date) ||
      rows < 4 || rows > 6) return false;
  portENTER_CRITICAL(&statusLock);
  displayStatus = {State::Ready, Error::None, date, date, false, rows, false, false};
  portEXIT_CRITICAL(&statusLock);
  previewMode = false;
  lastAttempt = date;
  return true;
}

void handle() {
  if (!display_mode::isCalendar() || previewMode || requests == nullptr || isBusy()) return;
  if (millis() - lastClockPoll < 1000) return;
  lastClockPoll = millis();
  if (clock_service::isSyncing()) return;
  calendar::Date today = {};
  if (!readCurrentDate(today) || calendar::sameDate(today, lastAttempt)) return;
  // 同一天重复 NTP 校时不刷屏；失败也不在循环内持续刷新屏幕。
  enqueue({today, false, nullptr});
}

bool requestPreview(const calendar::Date& date) {
  if (!display_mode::isCalendar() || !calendar::isValidDate(date)) return false;
  return enqueue({date, true, nullptr});
}

bool requestCurrentDate() {
  if (!display_mode::isCalendar() || isBusy() || requests == nullptr) return false;
  previewMode = false;
  calendar::Date today = {};
  if (!readCurrentDate(today)) {
    lastAttempt = {};
    portENTER_CRITICAL(&statusLock);
    displayStatus.state = State::WaitingForTime;
    displayStatus.preview = false;
    portEXIT_CRITICAL(&statusLock);
    return true;
  }
  const Status snapshot = status();
  if (snapshot.state == State::Ready && !snapshot.image &&
      calendar::sameDate(today, snapshot.displayedDate)) {
    portENTER_CRITICAL(&statusLock);
    displayStatus.preview = false;
    portEXIT_CRITICAL(&statusLock);
    lastAttempt = today;
    return true;
  }
  return enqueue({today, false, nullptr});
}

bool requestImage(const uint8_t* frame, size_t length) {
  if (display_mode::isCalendar() || frame == nullptr || length != EPD_FRAME_BYTES) {
    return false;
  }
  return enqueue({{}, false, frame});
}

}  // namespace display_service
