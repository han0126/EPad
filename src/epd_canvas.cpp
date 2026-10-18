#include "epd_canvas.h"

#include <cstring>

EpdCanvas::EpdCanvas() : Adafruit_GFX(EPD_WIDTH, EPD_HEIGHT) {
  clear();
}

void EpdCanvas::clear(Color4 color) {
  const uint8_t value = static_cast<uint8_t>(color) & 0x03;
  // 0x55 将同一个两位颜色复制到一个字节的四个像素中。
  std::memset(frame_, value * 0x55, sizeof(frame_));
}

bool EpdCanvas::loadPackedFrame(const uint8_t* data, size_t length) {
  if (data == nullptr || length != sizeof(frame_)) return false;
  setRotation(0);
  std::memcpy(frame_, data, sizeof(frame_));
  return true;
}

void EpdCanvas::drawPixel(int16_t x, int16_t y, uint16_t color) {
  if (x < 0 || y < 0 || x >= width() || y >= height() || color > 3) {
    return;
  }

  // 将 GFX 旋转后的坐标还原成屏幕物理坐标。
  int16_t physicalX = x;
  int16_t physicalY = y;
  switch (getRotation()) {
    case 1:
      physicalX = EPD_WIDTH - 1 - y;
      physicalY = x;
      break;
    case 2:
      physicalX = EPD_WIDTH - 1 - x;
      physicalY = EPD_HEIGHT - 1 - y;
      break;
    case 3:
      physicalX = y;
      physicalY = EPD_HEIGHT - 1 - x;
      break;
    default:
      break;
  }

  const size_t offset = static_cast<size_t>(physicalY) * (EPD_WIDTH / 4) +
                        static_cast<uint16_t>(physicalX) / 4;
  const uint8_t shift = (3 - (physicalX & 3)) * 2;
  // 只改当前像素对应的两位，保留同一字节中的其他像素。
  const uint8_t mask = static_cast<uint8_t>(0x03U << shift);
  frame_[offset] = static_cast<uint8_t>(
      (frame_[offset] & static_cast<uint8_t>(~mask)) | (color << shift));
}

const uint8_t* EpdCanvas::packedData() const {
  return frame_;
}
