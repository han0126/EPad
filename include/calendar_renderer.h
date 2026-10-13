#pragma once

#include "epd_config.h"

// Render the fixed calendar artwork used by the static display demo.
Color4 calendarPixel(uint16_t x, uint16_t y);

// Generate and transmit one complete calendar frame through the existing EPD driver.
void sendCalendarFrame();
