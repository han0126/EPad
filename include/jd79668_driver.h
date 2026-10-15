#pragma once

#include "display_config.h"

namespace jd79668 {

// 初始化软件 SPI 使用的 GPIO。
void beginPins();

// 硬件复位并初始化控制器；BUSY 超时返回 false。
bool initialize();

// 按物理坐标读取画布，每四个像素打包成一个字节发送。
void sendFrame(PixelProvider pixelProvider);

// 触发全屏刷新并等待完成。
bool refresh();

// 刷新后关闭电源并发送深度休眠命令；BUSY 超时返回 false。
bool sleep();

}  // namespace jd79668
