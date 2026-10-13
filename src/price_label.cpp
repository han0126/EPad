#include "price_label.h"

#include "epd_driver.h"
#include "label_font.h"

namespace {

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

}  // namespace

Color4 priceLabelPixel(uint16_t x, uint16_t y) {
  constexpr uint16_t border = 5;

  if (x < border || x >= EPD_WIDTH - border || y < border ||
      y >= EPD_HEIGHT - border) {
    return Color4::Black;
  }

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

  if (textPixel(PRICE_TEXT, sizeof(PRICE_TEXT) - 1, 72, 151, 10, x, y)) {
    return Color4::Red;
  }
  if (textPixel(CURRENCY_TEXT, sizeof(CURRENCY_TEXT) - 1, 12, 176, 3, x, y) ||
      textPixel(REGULAR_TEXT, sizeof(REGULAR_TEXT) - 1, 12, 230, 2, x, y) ||
      textPixel(UNIT_TEXT, sizeof(UNIT_TEXT) - 1, 300, 230, 2, x, y)) {
    return Color4::Black;
  }
  if (inRectangle(x, y, 10, 237, 160, 3)) return Color4::Red;

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

void sendPriceLabelFrame() {
  epd::sendFrame(priceLabelPixel);
}
