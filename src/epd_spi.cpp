#include "epd_spi.h"

#include <Arduino.h>

#include "board_config.h"

#if EPD_USE_HARDWARE_SPI
#include <driver/spi_master.h>
#include <esp_err.h>
#include <soc/soc_caps.h>
#endif

namespace {

void restoreIdlePins() {
  digitalWrite(EpdPin::SCK, HIGH);
  digitalWrite(EpdPin::SDA, LOW);
  pinMode(EpdPin::SCK, OUTPUT);
  pinMode(EpdPin::SDA, OUTPUT);
}

#if EPD_USE_HARDWARE_SPI
constexpr spi_host_device_t HOST = SPI2_HOST;
constexpr size_t MAX_TRANSFER_BYTES = 64;
static_assert(MAX_TRANSFER_BYTES <= SOC_SPI_MAXIMUM_BUFFER_SIZE,
              "Non-DMA SPI transfers must fit the hardware buffer");
spi_device_handle_t device = nullptr;
bool busOwned = false;

void reportError(const char* operation, esp_err_t error) {
  Serial.printf("[epd-spi] %s failed: %s.\n", operation, esp_err_to_name(error));
#if ARDUINO_USB_CDC_ON_BOOT
  Serial0.printf("[epd-spi] %s failed: %s.\n", operation, esp_err_to_name(error));
#endif
}
#endif

}  // namespace

namespace epd_spi {

bool begin() {
#if EPD_USE_HARDWARE_SPI
  // 不接管已被其他设备占用的 SPI 总线；初始化失败不会切换到半途回退。
  if (device != nullptr || busOwned) {
    if (!end()) return false;
  }
  spi_bus_config_t bus = {};
  bus.mosi_io_num = EpdPin::SDA;
  bus.miso_io_num = -1;  // GPIO13 是 BUSY，不能让 SPI 库自动作为 MISO 使用。
  bus.sclk_io_num = EpdPin::SCK;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.data4_io_num = -1;
  bus.data5_io_num = -1;
  bus.data6_io_num = -1;
  bus.data7_io_num = -1;
  bus.max_transfer_sz = MAX_TRANSFER_BYTES;
  esp_err_t error = spi_bus_initialize(HOST, &bus, SPI_DMA_DISABLED);
  if (error != ESP_OK) {
    reportError("Bus initialization", error);
    return false;
  }
  busOwned = true;
  spi_device_interface_config_t config = {};
  // 原驱动空闲 SCK 高，下降沿后设置数据、上升沿采样，对应 MODE3。
  config.mode = 3;
  config.clock_speed_hz = EPD_HARDWARE_SPI_HZ;
  config.spics_io_num = -1;
  config.queue_size = 1;
  config.flags = SPI_DEVICE_HALFDUPLEX;
  error = spi_bus_add_device(HOST, &config, &device);
  if (error != ESP_OK) {
    reportError("Device initialization", error);
    end();
    return false;
  }
  // 首笔空传输在 CS 高时完成，避免首次配置空闲时钟产生有效采样沿。
  digitalWrite(EpdPin::CS, HIGH);
  if (!writeByte(0x00)) {
    end();
    return false;
  }
  Serial.printf("[epd-spi] Hardware SPI2: %lu Hz, MODE3, MSB first; MISO disabled.\n",
      static_cast<unsigned long>(EPD_HARDWARE_SPI_HZ));
#if ARDUINO_USB_CDC_ON_BOOT
  Serial0.printf("[epd-spi] Hardware SPI2: %lu Hz, MODE3, MSB first; MISO disabled.\n",
      static_cast<unsigned long>(EPD_HARDWARE_SPI_HZ));
#endif
#else
  restoreIdlePins();
  Serial.println("[epd-spi] Software SPI: original pin sequence and delays.");
#if ARDUINO_USB_CDC_ON_BOOT
  Serial0.println("[epd-spi] Software SPI: original pin sequence and delays.");
#endif
#endif
  return true;
}

bool writeByte(uint8_t value) {
#if EPD_USE_HARDWARE_SPI
  if (device == nullptr) return false;
  spi_transaction_t transaction = {};
  transaction.flags = SPI_TRANS_USE_TXDATA;
  transaction.length = 8;
  transaction.tx_data[0] = value;
  // 同步完成后才允许上层拉高 CS；不启用 DMA，也不借用上传缓冲区。
  const esp_err_t error = spi_device_polling_transmit(device, &transaction);
  if (error != ESP_OK) reportError("Byte transfer", error);
  return error == ESP_OK;
#else
  for (uint8_t bit = 0; bit < 8; ++bit) {
    digitalWrite(EpdPin::SCK, LOW);
    digitalWrite(EpdPin::SDA, (value & 0x80) ? HIGH : LOW);
    delayMicroseconds(SPI_HALF_PERIOD_US);
    digitalWrite(EpdPin::SCK, HIGH);
    delayMicroseconds(SPI_HALF_PERIOD_US);
    value <<= 1;
  }
  return true;
#endif
}

bool writeBytes(const uint8_t* data, size_t length) {
  if (data == nullptr || length == 0) return false;
#if EPD_USE_HARDWARE_SPI
  if (device == nullptr) return false;
  while (length != 0) {
    const size_t chunk = length < MAX_TRANSFER_BYTES ? length : MAX_TRANSFER_BYTES;
    spi_transaction_t transaction = {};
    transaction.length = chunk * 8;
    transaction.tx_buffer = data;
    // 不使用 DMA；每块同步完成后才移动指针，不复制或异步持有画布。
    const esp_err_t error = spi_device_polling_transmit(device, &transaction);
    if (error != ESP_OK) {
      reportError("Block transfer", error);
      return false;
    }
    data += chunk;
    length -= chunk;
  }
#else
  for (size_t i = 0; i < length; ++i) {
    if (!writeByte(data[i])) return false;
  }
#endif
  return true;
}

bool end() {
  digitalWrite(EpdPin::CS, HIGH);
#if EPD_USE_HARDWARE_SPI
  if (device != nullptr) {
    const esp_err_t error = spi_bus_remove_device(device);
    if (error != ESP_OK) { reportError("Device release", error); return false; }
    device = nullptr;
  }
  if (busOwned) {
    const esp_err_t error = spi_bus_free(HOST);
    if (error != ESP_OK) { reportError("Bus release", error); return false; }
    busOwned = false;
  }
#endif
  restoreIdlePins();
  return true;
}

}  // namespace epd_spi
