#include "image_transfer.h"

#include <esp_rom_crc.h>
#include <cstring>

#include "display_service.h"
#include "display_mode.h"

namespace {

uint8_t frame[EPD_FRAME_BYTES];
uint8_t header[image_transfer::HEADER_BYTES];
size_t headerReceived = 0;
size_t frameReceived = 0;
uint32_t checksum = 0;
uint32_t expectedChecksum = 0;
bool receiving = false;
bool complete = false;
image_transfer::Error lastError = image_transfer::Error::None;

uint16_t read16(const uint8_t* data) {
  return (static_cast<uint16_t>(data[0]) << 8) | data[1];
}

uint32_t read32(const uint8_t* data) {
  return (static_cast<uint32_t>(data[0]) << 24) |
      (static_cast<uint32_t>(data[1]) << 16) |
      (static_cast<uint32_t>(data[2]) << 8) | data[3];
}

bool validateHeader() {
  // EPD4 + 宽高 + 像素字节数 + CRC32，整数均使用大端序。
  if (std::memcmp(header, "EPD4", 4) != 0 ||
      read16(header + 4) != EPD_WIDTH || read16(header + 6) != EPD_HEIGHT) {
    lastError = image_transfer::Error::InvalidHeader;
    return false;
  }
  if (read32(header + 8) != EPD_FRAME_BYTES) {
    lastError = image_transfer::Error::InvalidSize;
    return false;
  }
  expectedChecksum = read32(header + 12);
  return true;
}

}  // namespace

namespace image_transfer {

bool begin() {
  // 工作任务可能还在读取上传缓冲区，忙时绝不重用该区域。
  if (display_mode::isCalendar() || display_service::isBusy()) return false;
  reset();
  receiving = true;
  return true;
}

void write(const uint8_t* data, size_t length) {
  if (!receiving || lastError != Error::None || length == 0) return;
  if (data == nullptr) {
    lastError = Error::InvalidSize;
    return;
  }
  if (headerReceived < HEADER_BYTES) {
    const size_t remaining = HEADER_BYTES - headerReceived;
    const size_t count = length < remaining ? length : remaining;
    std::memcpy(header + headerReceived, data, count);
    headerReceived += count;
    data += count;
    length -= count;
    if (headerReceived == HEADER_BYTES && !validateHeader()) return;
  }
  if (length > EPD_FRAME_BYTES - frameReceived) {
    lastError = Error::InvalidSize;
    return;
  }
  if (length != 0) {
    std::memcpy(frame + frameReceived, data, length);
    checksum = esp_rom_crc32_le(checksum, data, static_cast<uint32_t>(length));
    frameReceived += length;
  }
}

bool finish() {
  if (!receiving) return false;
  receiving = false;
  if (lastError == Error::None &&
      (headerReceived != HEADER_BYTES || frameReceived != EPD_FRAME_BYTES)) {
    lastError = Error::InvalidSize;
  }
  if (lastError == Error::None && checksum != expectedChecksum) {
    lastError = Error::Checksum;
  }
  complete = lastError == Error::None;
  return complete;
}

bool submit() {
  if (!complete || !display_service::requestImage(frame, sizeof(frame))) return false;
  complete = false;
  return true;
}

void abort() {
  receiving = false;
  complete = false;
  lastError = Error::Aborted;
}

void reset() {
  // 不清写像素缓冲区，reset 可在工作任务读取期间安全调用。
  headerReceived = 0;
  frameReceived = 0;
  checksum = 0;
  expectedChecksum = 0;
  receiving = false;
  complete = false;
  lastError = Error::None;
}

bool isReceiving() { return receiving; }
Error error() { return lastError; }

const char* errorName() {
  switch (lastError) {
    case Error::InvalidHeader: return "image_header";
    case Error::InvalidSize: return "image_size";
    case Error::Checksum: return "image_checksum";
    case Error::Aborted: return "image_aborted";
    default: return "";
  }
}

}  // namespace image_transfer
