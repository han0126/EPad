#pragma once

#include "display_config.h"

namespace image_transfer {

constexpr size_t HEADER_BYTES = 16;
constexpr size_t PACKET_BYTES = HEADER_BYTES + EPD_FRAME_BYTES;

enum class Error : uint8_t { None, InvalidHeader, InvalidSize, Checksum, Aborted };

// 上传暂存区与屏幕画布分离，接收失败不触碰当前显示画面。
bool begin();
void write(const uint8_t* data, size_t length);
bool finish();
bool submit();
void abort();
void reset();
bool isReceiving();
Error error();
const char* errorName();

}  // namespace image_transfer
