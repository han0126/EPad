#include "power_service.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_attr.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <sys/time.h>

#include "board_config.h"
#include "display_service.h"
#include "clock_service.h"
#include "display_mode.h"
#include "image_transfer.h"
#include "network_settings.h"
#include "power_config.h"
#include "wifi_portal.h"
#include "weather_service.h"

namespace {

constexpr uint32_t RETAINED_MAGIC = 0x43414C04;
constexpr uint32_t IMAGE_HOLD_MAGIC = 0x494D4701;
constexpr uint64_t US_PER_SECOND = 1000000ULL;
constexpr uint8_t DISPLAY_OUTPUTS[] = {
    EpdPin::CS, EpdPin::RST, EpdPin::DC, EpdPin::SCK, EpdPin::SDA};

struct RetainedState {
  uint32_t magic;
  bool clockValid;
  bool frameValid;
  calendar::Date displayedDate;
  uint8_t rows;
  weather_display::Data weather;
};

// 时钟与画面状态只用 RTC RAM；天气次数和缓存由独立模块保存到 NVS。
RTC_DATA_ATTR RetainedState retained = {};
// 仅防御意外的深睡唤醒；按复位键或重新接电会清除此标志并开启配置。
RTC_DATA_ATTR uint32_t imageHold = 0;
bool timerWake = false;
uint32_t startedAt = 0;
bool sleepFailed = false;
uint32_t sleepFailedAt = 0;
bool imageSleepArmed = false;
bool recoveringImageHold = false;
uint32_t imageReadyAt = 0;

struct WakePlan {
  uint64_t delayUs;
  time_t target;
  bool clockValid;
};

WakePlan planWake() {
  const uint64_t retryUs =
      static_cast<uint64_t>(PowerConfig::NO_TIME_RETRY_SECONDS) * US_PER_SECOND;
  WakePlan plan = {retryUs, 0, false};
  struct tm local = {};
  if (clock_service::readLocalTime(local)) {
    struct timeval now = {};
    gettimeofday(&now, nullptr);
    // mktime 自动处理月末、年末、闰年；不是从本次开机起等待 24 小时。
    local.tm_mday += 1;
    local.tm_hour = 0;
    local.tm_min = 0;
    local.tm_sec = PowerConfig::MIDNIGHT_OFFSET_SECONDS;
    local.tm_isdst = -1;
    const time_t target = mktime(&local);
    if (target > now.tv_sec) {
      plan = {static_cast<uint64_t>(target - now.tv_sec) * US_PER_SECOND -
                  static_cast<uint64_t>(now.tv_usec), target, true};
      if (display_service::status().state == display_service::State::Error) {
        const uint64_t displayRetryUs =
            static_cast<uint64_t>(PowerConfig::DISPLAY_ERROR_RETRY_SECONDS) *
            US_PER_SECOND;
        if (displayRetryUs < plan.delayUs) {
          plan.delayUs = displayRetryUs;
          plan.target = now.tv_sec + PowerConfig::DISPLAY_ERROR_RETRY_SECONDS;
        }
      }
    }
  }
  if (PowerConfig::WAKE_INTERVAL_OVERRIDE_SECONDS != 0) {
    plan.delayUs = static_cast<uint64_t>(PowerConfig::WAKE_INTERVAL_OVERRIDE_SECONDS) *
        US_PER_SECOND;
    if (plan.clockValid) {
      plan.target = time(nullptr) + PowerConfig::WAKE_INTERVAL_OVERRIDE_SECONDS;
    }
  }
  return plan;
}

void releaseDisplayPins() {
  // main 已先配置好空闲电平，再解锁，避免唤醒瞬间误复位屏幕。
  gpio_deep_sleep_hold_dis();
  for (uint8_t pin : DISPLAY_OUTPUTS) {
    gpio_hold_dis(static_cast<gpio_num_t>(pin));
  }
}

void holdDisplayPins() {
  // 不改变驱动的寄存器、时序和电平，仅保持睡眠前的输出状态。
  for (uint8_t pin : DISPLAY_OUTPUTS) {
    gpio_hold_en(static_cast<gpio_num_t>(pin));
  }
  gpio_deep_sleep_hold_en();
}

bool currentFramePending(const display_service::Status& display) {
  if (display.preview || display.state == display_service::State::Error) return false;
  struct tm local = {};
  if (!clock_service::readLocalTime(local)) return false;
  const calendar::Date today = {static_cast<uint16_t>(local.tm_year + 1900),
      static_cast<uint8_t>(local.tm_mon + 1), static_cast<uint8_t>(local.tm_mday)};
  if (!calendar::isValidDate(today)) return false;
  const bool weatherChanged = !display.weatherPreview &&
      !weather_display::sameData(display.weather, weather_service::dataForDate(today));
  if (display.state == display_service::State::Ready &&
      calendar::sameDate(today, display.displayedDate) && !weatherChanged) return false;
  // 校时可能刚刚完成，不能因日期轮询的一秒节流而提前进入深睡。
  return display_service::requestCurrentDate();
}

void sleepUntilNextWake() {
  if (!clock_service::stopNetwork()) {
    sleepFailed = true;
    sleepFailedAt = millis();
    return;
  }
  const WakePlan plan = planWake();
  const esp_err_t error = esp_sleep_enable_timer_wakeup(plan.delayUs);
  if (error != ESP_OK) {
    Serial.printf("[power] Timer setup failed: %d; retry later.\n", error);
    sleepFailed = true;
    sleepFailedAt = millis();
    return;
  }

  const display_service::Status display = display_service::status();
  retained = {};
  retained.clockValid = plan.clockValid;
  // 预览不作为下次唤醒的正常画面，下次必须恢复实际日期。
  retained.frameValid = display.state == display_service::State::Ready &&
      !display.preview && !display.image && !display.weatherPreview;
  retained.displayedDate = display.displayedDate;
  retained.rows = display.rows;
  retained.weather = display.weather;
  retained.magic = RETAINED_MAGIC;
  imageHold = 0;

  char target[24] = {};
  struct tm local = {};
  if (plan.target != 0 && localtime_r(&plan.target, &local) != nullptr) {
    strftime(target, sizeof(target), "%Y-%m-%d %H:%M:%S", &local);
  }
  Serial.printf("[power] Deep sleep for %llu seconds; next wake: %s\n",
                static_cast<unsigned long long>(plan.delayUs / US_PER_SECOND),
                target[0] ? target : "time unknown, hourly retry");
  Serial.flush();
  holdDisplayPins();
  esp_deep_sleep_start();
}

void sleepWithImage() {
  // 先停止网页和 DNS，再确认 SNTP 与 Wi-Fi 已完全关闭。
  if (!wifi_portal::close()) return;
  if (!clock_service::stopNetwork()) {
    sleepFailed = true;
    sleepFailedAt = millis();
    return;
  }
  const esp_err_t error = esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  // INVALID_STATE 表示没有活动的唤醒源，同样无需再配置定时器。
  if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
    Serial.printf("[power] Cannot disable wake sources: %d; retry later.\n", error);
    sleepFailed = true;
    sleepFailedAt = millis();
    return;
  }
  retained = {};
  imageHold = IMAGE_HOLD_MAGIC;
  Serial.println("[power] Image deep sleep: all automatic wake sources disabled; reset to configure.");
  Serial.flush();
  holdDisplayPins();
  // 图片模式不调用 enable_timer_wakeup，实物评估的短周期参数也不生效。
  esp_deep_sleep_start();
}

}  // namespace

namespace power_service {

void begin() {
  releaseDisplayPins();
  startedAt = millis();
  if (esp_reset_reason() == ESP_RST_DEEPSLEEP && imageHold == IMAGE_HOLD_MAGIC) {
    recoveringImageHold = true;
    imageSleepArmed = true;
    clock_service::begin(false);
    Serial.println("[power] Unexpected wake while holding image; sleeping again without radio or redraw.");
    sleepWithImage();
    return;
  }
  imageHold = 0;
  timerWake = display_mode::isCalendar() && esp_reset_reason() == ESP_RST_DEEPSLEEP &&
      esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;
  const bool retainedValid = timerWake && retained.magic == RETAINED_MAGIC;
  clock_service::begin(retainedValid && retained.clockValid);
  if (retainedValid && retained.frameValid) {
    display_service::restoreRetainedFrame(retained.displayedDate, retained.rows, retained.weather);
  }

  if (!timerWake) {
    retained = {};
    Serial.println("[power] Power-on/reset: opening configuration hotspot.");
    openConfiguration();
    return;
  }

  Serial.println("[power] Timer wake: one NTP attempt, no configuration hotspot.");
  network_settings::Credentials credentials;
  if (network_settings::load(credentials)) {
    clock_service::beginNetworkTime(credentials.ssid.c_str(),
                                    credentials.password.c_str(), false);
  } else {
    Serial.println("[power] No saved network. Reset the board to configure Wi-Fi.");
  }
}

bool openConfiguration() {
  if (weather_service::isBusy()) {
    Serial.println("[power] Weather request in progress; configure after it completes.");
    return false;
  }
  if (requestImageSleep()) {
    Serial.println("[power] Image sleep pending; reset the board to configure again.");
    return false;
  }
  if (wifi_portal::isActive()) return true;
  // 主动配网优先，先取消普通唤醒周期中尚未结束的校时。
  clock_service::stopNetwork();
  return wifi_portal::begin();
}

bool requestImageSleep() {
  if (imageSleepArmed) return true;
  if (display_mode::isCalendar() || display_service::isBusy() ||
      image_transfer::isReceiving() || clock_service::isSyncing() || weather_service::isBusy()) return false;
  const display_service::Status display = display_service::status();
  if (display.state != display_service::State::Ready ||
      display.error != display_service::Error::None || !display.image) return false;
  // 成功保存后才锁定深睡，避免网页提示成功但断电又回到日历。
  if (!display_mode::saveDisplayedImage()) return false;
  imageSleepArmed = true;
  imageReadyAt = millis();
  sleepFailed = false;
  Serial.printf("[power] Image ready; permanent sleep in %lu ms.\n",
                static_cast<unsigned long>(PowerConfig::IMAGE_SLEEP_NOTICE_MS));
  return true;
}

bool imageSleepPending() {
  return imageSleepArmed;
}

void handle() {
  requestImageSleep();
  if (imageSleepArmed) {
    if (display_service::isBusy() || image_transfer::isReceiving()) return;
    if (!recoveringImageHold && wifi_portal::isActive() &&
        millis() - imageReadyAt < PowerConfig::IMAGE_SLEEP_NOTICE_MS) return;
    if (sleepFailed && millis() - sleepFailedAt < PowerConfig::SLEEP_ERROR_RETRY_MS) return;
    sleepWithImage();
    return;
  }
  if (!display_mode::isCalendar()) {
    // 重启后只开放限时配置窗口，不重新刷图，也不安排每日唤醒。
    if (!display_mode::hasRetainedImage() || wifi_portal::isActive() ||
        display_service::isBusy() || image_transfer::isReceiving()) return;
    if (millis() - startedAt < PowerConfig::MIN_AWAKE_MS) return;
    if (sleepFailed && millis() - sleepFailedAt < PowerConfig::SLEEP_ERROR_RETRY_MS) return;
    sleepWithImage();
    return;
  }
  if (wifi_portal::isActive() || clock_service::isSyncing() ||
      weather_service::isBusy() || display_service::isBusy()) return;
  if (millis() - startedAt < PowerConfig::MIN_AWAKE_MS) return;
  if (sleepFailed && millis() - sleepFailedAt < PowerConfig::SLEEP_ERROR_RETRY_MS) return;
  if (currentFramePending(display_service::status())) return;
  sleepUntilNextWake();
}

Status status() {
  const uint32_t elapsed = millis() - imageReadyAt;
  const uint32_t remaining = imageSleepArmed && !recoveringImageHold &&
      elapsed < PowerConfig::IMAGE_SLEEP_NOTICE_MS
      ? PowerConfig::IMAGE_SLEEP_NOTICE_MS - elapsed : 0;
  return {timerWake,
      display_mode::isCalendar() && !imageSleepArmed ? planWake().target : time_t{0},
      imageSleepArmed, (remaining + 999) / 1000};
}

}  // namespace power_service
