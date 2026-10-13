#pragma once

#include "epd_config.h"

// Return the color of one pixel in the retail-label artwork.
Color4 priceLabelPixel(uint16_t x, uint16_t y);

// The callback passed to the display driver for a complete frame.
void sendPriceLabelFrame();
