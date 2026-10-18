#include "jd79668_driver.h"

#include <Arduino.h>

#include "board_config.h"
#include "epd_spi.h"

namespace {

bool writeCommand(uint8_t command) {
  digitalWrite(EpdPin::CS, LOW);
  digitalWrite(EpdPin::DC, LOW);
  const bool sent = epd_spi::writeByte(command);
  digitalWrite(EpdPin::CS, HIGH);
  digitalWrite(EpdPin::DC, HIGH);
  return sent;
}

bool writeData(uint8_t data) {
  digitalWrite(EpdPin::CS, LOW);
  digitalWrite(EpdPin::DC, HIGH);
  const bool sent = epd_spi::writeByte(data);
  digitalWrite(EpdPin::CS, HIGH);
  digitalWrite(EpdPin::SDA, LOW);
  return sent;
}

bool writePixelRow(const uint8_t* data, size_t length) {
#if EPD_USE_HARDWARE_SPI && EPD_SPI_BURST_PIXELS
  digitalWrite(EpdPin::CS, LOW);
  digitalWrite(EpdPin::DC, HIGH);
  // 一行内保持片选有效；底层将 100 字节拆为 64 + 36 字节同步发送。
  const bool sent = epd_spi::writeBytes(data, length);
  // 失败也立即释放片选，由原有错误处理结束本次刷新并休眠屏幕。
  digitalWrite(EpdPin::CS, HIGH);
  digitalWrite(EpdPin::SDA, LOW);
  return sent;
#else
  // 关闭块传输或使用软件 SPI 时，完整保留原有逐字节片选时序。
  for (size_t i = 0; i < length; ++i) {
    if (!writeData(data[i])) return false;
  }
  return true;
#endif
}

bool writeCommandData(uint8_t command, const uint8_t* data, size_t length) {
  if (!writeCommand(command)) return false;
  for (size_t i = 0; i < length; ++i) {
    if (!writeData(data[i])) return false;
  }
  return true;
}

bool waitUntilReady(const char* operation) {
  const uint32_t startedAt = millis();
  // 此屏 BUSY 为低表示忙，等待时让出 CPU。
  while (digitalRead(EpdPin::BUSY) == LOW) {
    if (millis() - startedAt >= BUSY_TIMEOUT_MS) {
      Serial.printf("ERROR: BUSY timeout during %s. Check BUSY, power and GND.\n",
                    operation);
      return false;
    }
    delay(10);
  }
  return true;
}

bool resetPanel() {
  digitalWrite(EpdPin::RST, HIGH);
  delay(20);
  digitalWrite(EpdPin::RST, LOW);
  delay(40);
  digitalWrite(EpdPin::RST, HIGH);
  delay(50);
  return waitUntilReady("hardware reset");
}

bool setFullWindow() {
  const uint8_t fullWindow[] = {
      0x00, 0x00, 0x01, 0x8F, 0x00, 0x00, 0x01, 0x2B, 0x01};
  return writeCommandData(0x83, fullWindow, sizeof(fullWindow));
}

bool initPanel() {
  // 以下寄存器序列为当前屏幕已使用的配置，整理代码时不改动。
  if (!writeCommand(0x4D) || !writeData(0x78)) return false;

  const uint8_t panelSetting[] = {0x0F, 0x29};
  if (!writeCommandData(0x00, panelSetting, sizeof(panelSetting))) return false;

  const uint8_t boosterSoftStart[] = {
      0x0D, 0x12, 0x24, 0x25, 0x12, 0x29, 0x10};
  if (!writeCommandData(0x06, boosterSoftStart, sizeof(boosterSoftStart))) return false;

  if (!writeCommand(0x30) || !writeData(0x08)) return false;
  if (!writeCommand(0x50) || !writeData(0x37)) return false;

  const uint8_t resolution[] = {0x01, 0x90, 0x01, 0x2C};
  if (!writeCommandData(0x61, resolution, sizeof(resolution))) return false;

  if (!writeCommand(0xAE) || !writeData(0xCF)) return false;
  if (!writeCommand(0xB0) || !writeData(0x13)) return false;
  if (!writeCommand(0xBD) || !writeData(0x07)) return false;
  if (!writeCommand(0xBE) || !writeData(0xFE)) return false;
  if (!writeCommand(0xE9) || !writeData(0x01)) return false;

  if (!writeCommand(0x04)) return false;  // 开启屏幕电源。
  if (!waitUntilReady("power on")) return false;
  return setFullWindow();
}

}  // namespace

namespace jd79668 {

void beginPins() {
  pinMode(EpdPin::BUSY, INPUT_PULLUP);
  pinMode(EpdPin::CS, OUTPUT);
  pinMode(EpdPin::RST, OUTPUT);
  pinMode(EpdPin::DC, OUTPUT);
  pinMode(EpdPin::SCK, OUTPUT);
  pinMode(EpdPin::SDA, OUTPUT);

  digitalWrite(EpdPin::CS, HIGH);
  digitalWrite(EpdPin::RST, HIGH);
  digitalWrite(EpdPin::DC, HIGH);
  digitalWrite(EpdPin::SCK, HIGH);
  digitalWrite(EpdPin::SDA, LOW);
}

bool initialize() {
  if (!epd_spi::begin()) return false;
  if (resetPanel() && initPanel()) return true;
  epd_spi::end();
  return false;
}

bool sendFrame(const uint8_t* frame, size_t length) {
  // 先校验再发送命令，拒绝不完整数据，避免越界或部分覆盖屏幕 RAM。
  if (frame == nullptr || length != EPD_FRAME_BYTES) return false;
  constexpr size_t ROW_BYTES = EPD_WIDTH / 4;
#if EPD_USE_HARDWARE_SPI && EPD_SPI_BURST_PIXELS
  constexpr const char* TRANSFER_MODE = "hardware row-burst";
#elif EPD_USE_HARDWARE_SPI
  constexpr const char* TRANSFER_MODE = "hardware byte";
#else
  constexpr const char* TRANSFER_MODE = "software byte";
#endif

  const uint32_t startedAt = millis();
  if (!writeCommand(0x10)) return false;

  // 只优化像素数据，寄存器和控制命令继续使用原有写入方式。
  for (uint16_t y = 0; y < EPD_HEIGHT; ++y) {
    const size_t offset = static_cast<size_t>(y) * ROW_BYTES;
    if (!writePixelRow(frame + offset, ROW_BYTES)) return false;

    if ((y & 0x3F) == 0) {
      Serial.printf("  row %u / %u\n", y, EPD_HEIGHT);
    }
    yield();
  }
  const unsigned long elapsed = static_cast<unsigned long>(millis() - startedAt);
  Serial.printf("[epd-spi] Frame transfer: %lu ms, %u bytes, %s; excludes refresh wait.\n",
      elapsed, static_cast<unsigned>(length), TRANSFER_MODE);
#if ARDUINO_USB_CDC_ON_BOOT
  Serial0.printf("[epd-spi] Frame transfer: %lu ms, %u bytes, %s; excludes refresh wait.\n",
      elapsed, static_cast<unsigned>(length), TRANSFER_MODE);
#endif
  return true;
}

bool refresh() {
  if (!setFullWindow() || !writeCommand(0x12) || !writeData(0x00)) return false;
  delay(100);
  return waitUntilReady("display refresh");
}

bool sleep() {
  bool slept = waitUntilReady("before sleep");
  if (slept) slept = writeCommand(0x02) && writeData(0x00) && waitUntilReady("power off");
  if (slept) {
    delay(100);
    slept = writeCommand(0x07) && writeData(0xA5);
  }
  // 无论等待是否成功，都尝试释放硬件 SPI 并恢复睡眠前的 GPIO 电平。
  const bool released = epd_spi::end();
  return slept && released;
}

}  // namespace jd79668
