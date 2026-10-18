#pragma once

#include <stddef.h>
#include <stdint.h>

// 只由显示任务访问；CS/DC 仍由 JD79668 驱动按原顺序控制。
namespace epd_spi {

bool begin();
bool writeByte(uint8_t value);
// 同步发送非空数据，内部拆为不超过 64 字节的块；调用方保持 CS/DC 不变。
bool writeBytes(const uint8_t* data, size_t length);
// 释放硬件外设并恢复 SCK 高、DIN 低，供原有深睡 GPIO 保持逻辑使用。
bool end();

}  // namespace epd_spi
