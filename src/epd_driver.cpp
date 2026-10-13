#include "epd_driver.h"

#include <Arduino.h>

namespace {

void spiWriteByte(uint8_t value) {
  for (uint8_t bit = 0; bit < 8; ++bit) {
    digitalWrite(EpdPin::SCK, LOW);
    digitalWrite(EpdPin::SDA, (value & 0x80) ? HIGH : LOW);
    delayMicroseconds(SPI_HALF_PERIOD_US);
    digitalWrite(EpdPin::SCK, HIGH);
    delayMicroseconds(SPI_HALF_PERIOD_US);
    value <<= 1;
  }
}

void writeCommand(uint8_t command) {
  digitalWrite(EpdPin::CS, LOW);
  digitalWrite(EpdPin::DC, LOW);
  spiWriteByte(command);
  digitalWrite(EpdPin::CS, HIGH);
  digitalWrite(EpdPin::DC, HIGH);
}

void writeData(uint8_t data) {
  digitalWrite(EpdPin::CS, LOW);
  digitalWrite(EpdPin::DC, HIGH);
  spiWriteByte(data);
  digitalWrite(EpdPin::CS, HIGH);
  digitalWrite(EpdPin::SDA, LOW);
}

void writeCommandData(uint8_t command, const uint8_t* data, size_t length) {
  writeCommand(command);
  for (size_t i = 0; i < length; ++i) {
    writeData(data[i]);
  }
}

bool waitUntilReady(const char* operation) {
  const uint32_t startedAt = millis();
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

void setFullWindow() {
  const uint8_t fullWindow[] = {
      0x00, 0x00, 0x01, 0x8F, 0x00, 0x00, 0x01, 0x2B, 0x01};
  writeCommandData(0x83, fullWindow, sizeof(fullWindow));
}

bool initPanel() {
  writeCommand(0x4D);
  writeData(0x78);

  const uint8_t panelSetting[] = {0x0F, 0x29};
  writeCommandData(0x00, panelSetting, sizeof(panelSetting));

  const uint8_t boosterSoftStart[] = {
      0x0D, 0x12, 0x24, 0x25, 0x12, 0x29, 0x10};
  writeCommandData(0x06, boosterSoftStart, sizeof(boosterSoftStart));

  writeCommand(0x30);
  writeData(0x08);
  writeCommand(0x50);
  writeData(0x37);

  const uint8_t resolution[] = {0x01, 0x90, 0x01, 0x2C};
  writeCommandData(0x61, resolution, sizeof(resolution));

  writeCommand(0xAE);
  writeData(0xCF);
  writeCommand(0xB0);
  writeData(0x13);
  writeCommand(0xBD);
  writeData(0x07);
  writeCommand(0xBE);
  writeData(0xFE);
  writeCommand(0xE9);
  writeData(0x01);

  writeCommand(0x04);  // Power on.
  if (!waitUntilReady("power on")) return false;
  setFullWindow();
  return true;
}

}  // namespace

namespace epd {

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
  return resetPanel() && initPanel();
}

void sendFrame(PixelProvider pixelProvider) {
  if (pixelProvider == nullptr) return;

  writeCommand(0x10);

  // Four 2-bit pixels are packed into each byte:
  // black=00, white=01, yellow=10, red=11.
  for (uint16_t y = 0; y < EPD_HEIGHT; ++y) {
    for (uint16_t x = 0; x < EPD_WIDTH; x += 4) {
      const uint8_t p1 = static_cast<uint8_t>(pixelProvider(x, y));
      const uint8_t p2 = static_cast<uint8_t>(pixelProvider(x + 1, y));
      const uint8_t p3 = static_cast<uint8_t>(pixelProvider(x + 2, y));
      const uint8_t p4 = static_cast<uint8_t>(pixelProvider(x + 3, y));
      writeData((p1 << 6) | (p2 << 4) | (p3 << 2) | p4);
    }

    if ((y & 0x3F) == 0) {
      Serial.printf("  row %u / %u\n", y, EPD_HEIGHT);
    }
    yield();
  }
}

bool refresh() {
  setFullWindow();
  writeCommand(0x12);
  writeData(0x00);
  delay(100);
  return waitUntilReady("display refresh");
}

void sleep() {
  if (!waitUntilReady("before sleep")) return;
  writeCommand(0x02);
  writeData(0x00);
  if (!waitUntilReady("power off")) return;
  delay(100);
  writeCommand(0x07);
  writeData(0xA5);
}

}  // namespace epd
