#pragma once

#include <stddef.h>

#include "display_config.h"

class EpdCanvas;

namespace weather_icons {

constexpr int16_t SIZE = 30;

struct Condition {
  uint16_t code;
  const char* name;
  const char* shortText;
  Color4 color;
};

size_t count();
bool isKnown(uint16_t code);
// 未收录的代码统一返回 999，不能作为数组下标直接使用。
const Condition& condition(uint16_t code);
void draw(EpdCanvas& canvas, uint16_t code, int16_t left, int16_t top);

}  // namespace weather_icons
