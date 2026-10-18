#pragma once

#include "display_config.h"

namespace jd79668 {

// 初始化屏幕 GPIO 的空闲电平，硬件 SPI 在实际刷新时才启用。
void beginPins();

// 初始化 SPI、复位并配置控制器；SPI 失败或 BUSY 超时返回 false。
bool initialize();

// 校验完整的两位像素帧后直接发送；缓冲区在返回前不得修改。
// 参数无效或 SPI 传输失败返回 false，屏幕 BUSY 仍由 refresh() 等待。
bool sendFrame(const uint8_t* frame, size_t length);

// 触发全屏刷新并等待完成。
bool refresh();

// 关闭屏幕电源、发送深睡命令并释放 SPI；任何一步失败返回 false。
bool sleep();

}  // namespace jd79668
