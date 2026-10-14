# 四色墨水屏绘图说明

## 显示流程

`main.cpp` 创建静态画布，调用 `calendar::drawMonth()` 绘制日历，再通过
`jd79668::sendFrame()` 发送像素，最后刷新并休眠。

`jd79668_driver.cpp` 保留原有初始化寄存器、软件 SPI 电平顺序、延时、
BUSY 等待、刷新和休眠命令。驱动只读取像素，不负责文字和日历排版。

`EpdCanvas` 为 Adafruit GFX 提供 400 x 300 的两位像素缓冲，共占 30,000
字节 RAM。画布应使用全局或静态存储，避免在函数栈上创建。

绘图颜色使用 `Color4` 的整数值，不是 RGB565：黑=0、白=1、黄=2、红=3。
画布支持 GFX 旋转，但驱动始终按屏幕物理坐标读取。

## 中文字体

文字渲染使用与参考工程相同的 U8g2 字库，不需要 GxEPD2。

| text::Font | U8g2 字体 | 用途 |
| --- | --- | --- |
| Chinese16 | u8g2_font_wqy16_t_gb2312 | 主中文文字 |
| Chinese12 | u8g2_font_wqy12_t_gb2312 | 农历等小字 |
| Date17 | u8g2_font_fub17_tn | 日期数字 |
| Title25 | u8g2_font_fub25_tn | 标题数字 |

源码保存为 UTF-8，字符串直接传入绘图函数，无需转换成 GB2312。
字库名中的 `gb2312` 表示收录范围，不表示输入字符串的编码。
中英文混排使用中文字体；数字专用字体不用于汉字。

```cpp
#include "epd_canvas.h"
#include "text_renderer.h"

static EpdCanvas exampleCanvas;

void drawExample() {
  exampleCanvas.clear();
  text::drawText(exampleCanvas, "农历闰六月廿三", 12, 12,
                 Color4::Black, text::Font::Chinese16);
  text::drawText(exampleCanvas, "春节 端午 中秋 冬至", 12, 34,
                 Color4::Red, text::Font::Chinese12);
}
```

`drawText()` 的纵坐标表示文字顶部，模块内部转换为 U8g2 所需的基线。
`drawCenteredText()` 根据实际字宽居中。文字透明绘制，替换文字前应先
清除或重画背景。字符必须包含在所选字库中。

上例仅绘制画布；要上屏，可参照 `main.cpp` 的取色回调及发送流程。

## 日历数据

`calendar_data.cpp` 保存固定的 2025 年 6 月演示数据。`MonthData` 包含年、月、
天数、月初星期、标红日期和每天的农历标签。周日为 0，周六为 6；
标红日期为 0 时不标红。农历标签可为空，数量不足时只绘制已有标签。

当前农历标签是初一至三十的字体示例，不是公历日期对应的真实农历。
动态日期、农历换算、节气和节日计算需要独立的数据逻辑，绘图层不处理这些计算。

## 依赖

`platformio.ini` 固定 Adafruit GFX 与 U8g2 依赖版本，下次正常构建时由
PlatformIO 解析。依照本次整理要求，未运行编译、测试或检查命令。
