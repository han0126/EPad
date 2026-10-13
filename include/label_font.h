#pragma once

#include "epd_config.h"

// Render a single-byte ASCII label using the built-in 5x7 font.
// The function intentionally retains the original ASCII-only behavior.
bool textPixel(const char* text, TextLength length, uint16_t originX,
               uint16_t originY, uint8_t scale, uint16_t x, uint16_t y);
