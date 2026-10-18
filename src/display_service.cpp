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
#include "weather_settings.h"
#include "weather_service.h"

namespace {

struct Request {
  calendar::Date date;
  bool preview;
  const uint8_t* imageFrame;
  weather_display::Data weather;
  bool weatherPreview;
  calendar_events::DisplayData events;
};

EpdCanvas canvas;
QueueHandle_t requests = nullptr;
portMUX_TYPE statusLock = portMUX_INITIALIZER_UNLOCKED;
display_service::Status displayStatus = {
    display_service::State::WaitingForTime, display_service::Error::None,
    {}, {}, false, 0, false, false, weather_display::unavailable(), false, {}};
bool previewMode = false;
calendar::Date lastAttempt = {};
uint32_t lastClockPoll = 0;
weather_display::Data selectedWeather = weather_display::placeholder();
bool weatherPreviewMode = false;
bool weatherRedrawPending = false;
// 由显示任务在成功完成真实日历刷新后置位，主任务消费一次。
bool cleanupPending = false;
calendar::Date cleanupDate = {};

weather_display::Data weatherFor(const calendar::Date& date) {
  return weatherPreviewMode ? selectedWeather : weather_service::dataForDate(date);
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
      weather_settings::Settings weather;
      weather_settings::load(weather);
      calendar::drawMonth(canvas, month, weather_settings::regionName(weather), request.weather,
                           request.events);
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
        if (!jd79668::sendFrame(canvas.packedData(), EPD_FRAME_BYTES)) {
          error = display_service::Error::PanelFailed;
        } else if (!jd79668::refresh()) {
          error = display_service::Error::PanelFailed;
        }
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
    cleanupPending = error == display_service::Error::None && !image &&
        !request.preview && !request.weatherPreview;
    if (cleanupPending) cleanupDate = request.date;
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

void cleanExpiredMemos() {
  calendar::Date completedDate = {};
  portENTER_CRITICAL(&statusLock);
  const bool pending = cleanupPending;
  if (pending) completedDate = cleanupDate;
  cleanupPending = false;
  portEXIT_CRITICAL(&statusLock);
  if (!pending) return;

  const display_service::Status display = display_service::status();
  if (display.state != display_service::State::Ready ||
      display.error != display_service::Error::None || display.image ||
      display.preview || display.weatherPreview) return;
  calendar::Date today = {};
  // 系统时间必须有效，跨零点或校时后日期不一致时等待新一轮真实刷新。
  if (!readCurrentDate(today) || !calendar::sameDate(today, completedDate) ||
      !calendar::sameDate(today, display.displayedDate)) return;
  size_t removed = 0;
  const calendar_events::Error result = calendar_events::removeExpired(today, removed);
  if (result != calendar_events::Error::None) {
    Serial.printf("[events] Expired memo cleanup incomplete: %s; no repeated writes this refresh.\n",
                  calendar_events::errorName(result));
  } else if (removed != 0) {
    Serial.printf("[events] Removed %u expired memos before %04u-%02u-%02u.\n",
        static_cast<unsigned>(removed), static_cast<unsigned>(today.year),
        static_cast<unsigned>(today.month), static_cast<unsigned>(today.day));
  }
}

bool enqueue(Request request) {
  if (requests == nullptr || display_service::isBusy()) return false;
  // 在主任务中生成快照，显示工作任务不读取可变的记录表。
  request.events = request.imageFrame == nullptr
      ? calendar_events::displayDataForDate(request.date) : calendar_events::DisplayData{};
  // 状态先置为排队，避免工作任务已开始后又被主任务写回排队状态。
  portENTER_CRITICAL(&statusLock);
  const display_service::Status previous = displayStatus;
  displayStatus.state = display_service::State::Queued;
  displayStatus.error = display_service::Error::None;
  displayStatus.requestedDate = request.date;
  displayStatus.preview = request.preview;
  displayStatus.imageRequest = request.imageFrame != nullptr;
  displayStatus.weather = request.weather;
  displayStatus.weatherPreview = request.weatherPreview;
  displayStatus.events = request.events;
  portEXIT_CRITICAL(&statusLock);
  if (xQueueSend(requests, &request, 0) != pdTRUE) {
    portENTER_CRITICAL(&statusLock);
    displayStatus = previous;
    portEXIT_CRITICAL(&statusLock);
    return false;
  }
  previewMode = request.preview;
  if (request.imageFrame == nullptr) {
    lastAttempt = request.date;
    weatherRedrawPending = false;
  }
  return true;
}

bool refreshWeather(const weather_display::Data& weather, bool preview) {
  if (!display_mode::isCalendar() || !display_service::isAvailable() ||
      display_service::isBusy()) return false;
  const display_service::Status display = display_service::status();
  const bool datePreview = display.preview && calendar::isValidDate(display.requestedDate);
  calendar::Date date = {};
  if (datePreview) {
    date = display.requestedDate;
  } else if (!readCurrentDate(date)) {
    return false;
  }
  const weather_display::Data target = preview ? weather : weather_service::dataForDate(date);
  if (!enqueue({date, datePreview, nullptr, target, preview, {}})) return false;
  selectedWeather = weather;
  weatherPreviewMode = preview;
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

bool isAvailable() {
  return requests != nullptr;
}

bool isBusy() {
  const State state = status().state;
  return state == State::Queued || state == State::Drawing;
}

bool restoreRetainedFrame(const calendar::Date& date, uint8_t rows,
                           const weather_display::Data& weather,
                           const calendar_events::DisplayData& events) {
  if (requests == nullptr || isBusy() || !calendar::isValidDate(date) ||
      rows < 4 || rows > 6 || events.text[calendar_events::TEXT_CAPACITY - 1] != '\0' ||
      (events.importantDays & 0x80000000UL) != 0) return false;
  portENTER_CRITICAL(&statusLock);
  displayStatus = {State::Ready, Error::None, date, date, false, rows, false, false,
                   weather, false, events};
  // 恢复记录不等于重新刷屏，不能凭 RTC 中的 Ready 状态触发删除。
  cleanupPending = false;
  portEXIT_CRITICAL(&statusLock);
  previewMode = false;
  lastAttempt = date;
  return true;
}

void handle() {
  if (!display_mode::isCalendar() || requests == nullptr || isBusy() ||
      clock_service::isSyncing() || weather_service::isBusy()) return;
  cleanExpiredMemos();
  if (weatherRedrawPending) {
    const Status snapshot = status();
    const bool datePreview = snapshot.preview && calendar::isValidDate(snapshot.requestedDate);
    calendar::Date date = {};
    if (datePreview) date = snapshot.requestedDate;
    else if (!readCurrentDate(date)) return;
    enqueue({date, datePreview, nullptr, weatherFor(date), weatherPreviewMode, {}});
    return;
  }
  if (millis() - lastClockPoll < 1000) return;
  lastClockPoll = millis();
  const Status snapshot = status();
  if (previewMode) {
    // 日期预览也响应记录编辑，但不会改变系统时间或清理备忘。
    const calendar::Date date = snapshot.requestedDate;
    if (calendar::isValidDate(date) &&
        !calendar_events::sameDisplayData(snapshot.events, calendar_events::displayDataForDate(date))) {
      enqueue({date, true, nullptr, weatherFor(date), weatherPreviewMode, {}});
    }
    return;
  }
  calendar::Date today = {};
  if (!readCurrentDate(today)) return;
  const weather_display::Data target = weatherFor(today);
  if (calendar::sameDate(today, lastAttempt) &&
      weather_display::sameData(target, snapshot.weather) &&
      calendar_events::sameDisplayData(snapshot.events, calendar_events::displayDataForDate(today))) return;
  // 同一天重复 NTP 校时不刷屏；失败也不在循环内持续刷新屏幕。
  enqueue({today, false, nullptr, target, weatherPreviewMode, {}});
}

bool requestPreview(const calendar::Date& date) {
  if (!display_mode::isCalendar() || !calendar::isValidDate(date) || weather_service::isBusy()) return false;
  return enqueue({date, true, nullptr, weatherFor(date), weatherPreviewMode, {}});
}

bool requestCurrentDate(bool forceRefresh) {
  if (!display_mode::isCalendar() || isBusy() || requests == nullptr || weather_service::isBusy()) return false;
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
  const weather_display::Data target = weatherFor(today);
  if (!forceRefresh && snapshot.state == State::Ready && !snapshot.image &&
      calendar::sameDate(today, snapshot.displayedDate) && weather_display::sameData(target, snapshot.weather) &&
      calendar_events::sameDisplayData(snapshot.events, calendar_events::displayDataForDate(today))) {
    portENTER_CRITICAL(&statusLock);
    displayStatus.preview = false;
    portEXIT_CRITICAL(&statusLock);
    lastAttempt = today;
    return true;
  }
  return enqueue({today, false, nullptr, target, weatherPreviewMode, {}});
}

bool requestWeatherPreview(const weather_display::Data& weather) {
  if (!weather_display::isValidPreview(weather) || weather_service::isBusy()) return false;
  weather_display::Data preview = weather;
  preview.source = weather_display::Source::Preview;
  return refreshWeather(preview, true);
}

bool clearWeatherPreview() {
  if (weather_service::isBusy()) return false;
  return refreshWeather(weather_display::unavailable(), false);
}

void requestWeatherRedraw() {
  weatherRedrawPending = true;
}

bool requestImage(const uint8_t* frame, size_t length) {
  if (display_mode::isCalendar() || frame == nullptr || length != EPD_FRAME_BYTES) {
    return false;
  }
  return enqueue({{}, false, frame, weather_display::placeholder(), false, {}});
}

}  // namespace display_service
