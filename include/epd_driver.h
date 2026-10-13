#pragma once

#include "epd_config.h"

namespace epd {

// Configure the GPIO pins used by the manually clocked SPI bus.
void beginPins();

// Reset and initialize the JD79668 controller.
bool initialize();

// Generate and transmit one complete packed 2-bit frame.
void sendFrame(PixelProvider pixelProvider);

// Start a display update and wait until the panel is ready.
bool refresh();

// Power down the panel after an update.
void sleep();

}  // namespace epd
