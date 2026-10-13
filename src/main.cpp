#include <Arduino.h>

// JD79668 4.2-inch BWRY panel, 400 x 300 pixels, packed 2 bits per pixel.

namespace EpdPin {
constexpr uint8_t BUSY = 13;
constexpr uint8_t CS = 10;
constexpr uint8_t RST = 14;
constexpr uint8_t DC = 9;
constexpr uint8_t SCK = 12;
constexpr uint8_t SDA = 11;  // DIN on the display adapter
}  // namespace EpdPin

constexpr uint16_t EPD_WIDTH = 400;
constexpr uint16_t EPD_HEIGHT = 300;
constexpr uint32_t BUSY_TIMEOUT_MS = 180000;
constexpr uint8_t SPI_HALF_PERIOD_US = 1;

enum class Color4 : uint8_t {
  Black = 0,
  White = 1,
  Yellow = 2,
  Red = 3,
};

// Compact 5 x 7 font used by the price-label artwork. Bit 0 is the top row.
const uint8_t DIGIT_FONT[10][5] PROGMEM = {
    {0x3E, 0x51, 0x49, 0x45, 0x3E},  // 0
    {0x00, 0x42, 0x7F, 0x40, 0x00},  // 1
    {0x42, 0x61, 0x51, 0x49, 0x46},  // 2
    {0x21, 0x41, 0x45, 0x4B, 0x31},  // 3
    {0x18, 0x14, 0x12, 0x7F, 0x10},  // 4
    {0x27, 0x45, 0x45, 0x45, 0x39},  // 5
    {0x3C, 0x4A, 0x49, 0x49, 0x30},  // 6
    {0x01, 0x71, 0x09, 0x05, 0x03},  // 7
    {0x36, 0x49, 0x49, 0x49, 0x36},  // 8
    {0x06, 0x49, 0x49, 0x29, 0x1E},  // 9
};

const uint8_t LETTER_FONT[26][5] PROGMEM = {
    {0x7E, 0x11, 0x11, 0x11, 0x7E},  // A
    {0x7F, 0x49, 0x49, 0x49, 0x36},  // B
    {0x3E, 0x41, 0x41, 0x41, 0x22},  // C
    {0x7F, 0x41, 0x41, 0x22, 0x1C},  // D
    {0x7F, 0x49, 0x49, 0x49, 0x41},  // E
    {0x7F, 0x09, 0x09, 0x09, 0x01},  // F
    {0x3E, 0x41, 0x49, 0x49, 0x7A},  // G
    {0x7F, 0x08, 0x08, 0x08, 0x7F},  // H
    {0x00, 0x41, 0x7F, 0x41, 0x00},  // I
    {0x20, 0x40, 0x41, 0x3F, 0x01},  // J
    {0x7F, 0x08, 0x14, 0x22, 0x41},  // K
    {0x7F, 0x40, 0x40, 0x40, 0x40},  // L
    {0x7F, 0x02, 0x0C, 0x02, 0x7F},  // M
    {0x7F, 0x04, 0x08, 0x10, 0x7F},  // N
    {0x3E, 0x41, 0x41, 0x41, 0x3E},  // O
    {0x7F, 0x09, 0x09, 0x09, 0x06},  // P
    {0x3E, 0x41, 0x51, 0x21, 0x5E},  // Q
    {0x7F, 0x09, 0x19, 0x29, 0x46},  // R
    {0x46, 0x49, 0x49, 0x49, 0x31},  // S
    {0x01, 0x01, 0x7F, 0x01, 0x01},  // T
    {0x3F, 0x40, 0x40, 0x40, 0x3F},  // U
    {0x1F, 0x20, 0x40, 0x20, 0x1F},  // V
    {0x3F, 0x40, 0x38, 0x40, 0x3F},  // W
    {0x63, 0x14, 0x08, 0x14, 0x63},  // X
    {0x07, 0x08, 0x70, 0x08, 0x07},  // Y
    {0x61, 0x51, 0x49, 0x45, 0x43},  // Z
};

constexpr char HEADER_TEXT[] = "FRESH MARKET";
constexpr char SALE_TEXT[] = "SALE";
constexpr char PRODUCT_TEXT[] = "PREMIUM COFFEE";
constexpr char DETAIL_TEXT[] = "ROASTED BEANS 500G";
constexpr char SKU_TEXT[] = "SKU 202609";
constexpr char SKU_NUMBER[] = "202609";
constexpr char CURRENCY_TEXT[] = "CNY";
constexpr char PRICE_TEXT[] = "29.90";
constexpr char REGULAR_TEXT[] = "REGULAR 39.90";
constexpr char UNIT_TEXT[] = "PER BAG";
constexpr char MEMBER_TEXT[] = "MEMBER PRICE";
constexpr char SAVE_TEXT[] = "SAVE 10 CNY";
constexpr char BARCODE_PATTERN[] =
    "10110010111001011010001110100101100111010010110100111010110001011001101";

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

void writeCommandData(uint8_t command, const uint8_t *data, size_t length) {
  writeCommand(command);
  for (size_t i = 0; i < length; ++i) {
    writeData(data[i]);
  }
}

bool waitUntilReady(const char *operation) {
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
  // X: 0..399 (0x018F), Y: 0..299 (0x012B), scan enabled.
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

uint8_t fontColumn(char character, uint8_t column) {
  if (character >= '0' && character <= '9') {
    return pgm_read_byte(&DIGIT_FONT[character - '0'][column]);
  }
  if (character >= 'A' && character <= 'Z') {
    return pgm_read_byte(&LETTER_FONT[character - 'A'][column]);
  }
  if (character == '.') {
    constexpr uint8_t dot[5] = {0x00, 0x60, 0x60, 0x00, 0x00};
    return dot[column];
  }
  if (character == '-') return 0x08;
  if (character == '/') {
    constexpr uint8_t slash[5] = {0x20, 0x10, 0x08, 0x04, 0x02};
    return slash[column];
  }
  return 0;
}

bool textPixel(const char *text, uint8_t length, uint16_t originX,
               uint16_t originY, uint8_t scale, uint16_t x, uint16_t y) {
  const uint16_t width = static_cast<uint16_t>(length) * 6 * scale - scale;
  const uint16_t height = 7 * scale;
  if (x < originX || x >= originX + width || y < originY ||
      y >= originY + height) {
    return false;
  }

  const uint16_t relativeX = x - originX;
  const uint8_t characterIndex = relativeX / (6 * scale);
  const uint8_t glyphX = (relativeX % (6 * scale)) / scale;
  if (glyphX >= 5) return false;

  const uint8_t glyphY = (y - originY) / scale;
  return (fontColumn(text[characterIndex], glyphX) & (1U << glyphY)) != 0;
}

bool inRectangle(uint16_t x, uint16_t y, uint16_t left, uint16_t top,
                 uint16_t width, uint16_t height) {
  return x >= left && x < left + width && y >= top && y < top + height;
}

bool barcodePixel(uint16_t x, uint16_t y) {
  constexpr uint16_t barcodeX = 291;
  constexpr uint16_t barcodeY = 79;
  constexpr uint8_t moduleWidth = 1;
  constexpr uint16_t normalBottom = 113;
  constexpr uint16_t guardBottom = 121;

  if (x < barcodeX || y < barcodeY) return false;
  const uint16_t module = (x - barcodeX) / moduleWidth;
  if (module >= sizeof(BARCODE_PATTERN) - 1 ||
      BARCODE_PATTERN[module] != '1') {
    return false;
  }

  const bool guardBar = module < 3 ||
                        (module >= 25 && module <= 27) ||
                        module >= sizeof(BARCODE_PATTERN) - 4;
  return y < (guardBar ? guardBottom : normalBottom);
}

Color4 priceLabelPixel(uint16_t x, uint16_t y) {
  constexpr uint16_t border = 5;

  if (x < border || x >= EPD_WIDTH - border || y < border ||
      y >= EPD_HEIGHT - border) {
    return Color4::Black;
  }

  // Yellow brand band with a red sale badge.
  if (y < 60) {
    if (y >= 54) return Color4::Red;
    if (inRectangle(x, y, 310, 12, 78, 40)) {
      if (textPixel(SALE_TEXT, sizeof(SALE_TEXT) - 1, 326, 25, 2, x, y)) {
        return Color4::White;
      }
      return Color4::Red;
    }
    if (textPixel(HEADER_TEXT, sizeof(HEADER_TEXT) - 1, 12, 16, 4, x, y)) {
      return Color4::Black;
    }
    return Color4::Yellow;
  }

  // Barcode box and its human-readable number.
  if (inRectangle(x, y, 278, 68, 110, 66)) {
    if (x < 280 || x >= 386 || y < 70 || y >= 132) {
      return Color4::Black;
    }
    if (barcodePixel(x, y) ||
        textPixel(SKU_NUMBER, sizeof(SKU_NUMBER) - 1, 312, 122, 1, x, y)) {
      return Color4::Black;
    }
    return Color4::White;
  }

  if (textPixel(PRODUCT_TEXT, sizeof(PRODUCT_TEXT) - 1, 12, 72, 3, x, y) ||
      textPixel(DETAIL_TEXT, sizeof(DETAIL_TEXT) - 1, 12, 103, 2, x, y) ||
      textPixel(SKU_TEXT, sizeof(SKU_TEXT) - 1, 12, 123, 1, x, y)) {
    return Color4::Black;
  }

  if (inRectangle(x, y, 12, 140, 376, 5)) return Color4::Red;

  // Large promotional price: red attracts attention, black carries metadata.
  if (textPixel(PRICE_TEXT, sizeof(PRICE_TEXT) - 1, 72, 151, 10, x, y)) {
    return Color4::Red;
  }
  if (textPixel(CURRENCY_TEXT, sizeof(CURRENCY_TEXT) - 1, 12, 176, 3, x, y) ||
      textPixel(REGULAR_TEXT, sizeof(REGULAR_TEXT) - 1, 12, 230, 2, x, y) ||
      textPixel(UNIT_TEXT, sizeof(UNIT_TEXT) - 1, 300, 230, 2, x, y)) {
    return Color4::Black;
  }
  if (inRectangle(x, y, 10, 237, 160, 3)) return Color4::Red;

  // Yellow member-offer footer with black and red copy.
  if (y >= 252) {
    if (y < 257) return Color4::Black;
    if (inRectangle(x, y, 178, 261, 5, 30)) return Color4::Red;
    if (textPixel(MEMBER_TEXT, sizeof(MEMBER_TEXT) - 1, 12, 268, 2, x, y)) {
      return Color4::Black;
    }
    if (textPixel(SAVE_TEXT, sizeof(SAVE_TEXT) - 1, 198, 268, 2, x, y)) {
      return Color4::Red;
    }
    return Color4::Yellow;
  }

  return Color4::White;
}

void sendDemoFrame() {
  writeCommand(0x10);

  // The JD79668 accepts four packed 2-bit pixels per byte:
  // black=00, white=01, yellow=10, red=11. The image is generated on demand,
  // so this sketch does not need a 30,000-byte frame buffer.
  for (uint16_t y = 0; y < EPD_HEIGHT; ++y) {
    for (uint16_t x = 0; x < EPD_WIDTH; x += 4) {
      const uint8_t p1 = static_cast<uint8_t>(priceLabelPixel(x, y));
      const uint8_t p2 = static_cast<uint8_t>(priceLabelPixel(x + 1, y));
      const uint8_t p3 = static_cast<uint8_t>(priceLabelPixel(x + 2, y));
      const uint8_t p4 = static_cast<uint8_t>(priceLabelPixel(x + 3, y));
      const uint8_t packed =
          (p1 << 6) | (p2 << 4) | (p3 << 2) | p4;
      writeData(packed);
    }

    if ((y & 0x3F) == 0) {
      Serial.printf("  row %u / %u\n", y, EPD_HEIGHT);
    }
    yield();
  }
}

bool refreshPanel() {
  setFullWindow();
  writeCommand(0x12);
  writeData(0x00);
  delay(100);
  return waitUntilReady("display refresh");
}

void sleepPanel() {
  if (!waitUntilReady("before sleep")) return;
  writeCommand(0x02);
  writeData(0x00);
  if (!waitUntilReady("power off")) return;
  delay(100);
  writeCommand(0x07);
  writeData(0xA5);
}

bool showPriceLabelDemo() {
  if (!resetPanel() || !initPanel()) return false;

  Serial.println("Sending 400x300 four-color retail price label...");
  sendDemoFrame();
  if (!refreshPanel()) return false;

  sleepPanel();
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("JD79668 4.2-inch 400x300 four-color price-label demo");

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

  if (showPriceLabelDemo()) {
    Serial.println("Done. The panel is now sleeping.");
  } else {
    Serial.println("Demo stopped because the panel did not become ready.");
  }
}

void loop() {
  // Intentionally empty: repeatedly refreshing an e-paper panel is unnecessary.
}
 