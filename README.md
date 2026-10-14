# JD79668 四色墨水屏日历

ESP32-S3 驱动 4.2 英寸 400 x 300 四色墨水屏的静态日历示例。
使用 U8g2 文泉驿中文字体，屏幕底层保持原有 JD79668 驱动流程。

## 文件分工

业务源码位于 `src/`，对应接口位于 `include/`。

| 文件 | 职责 |
| --- | --- |
| `src/main.cpp` | 启动、绘图、发送、刷新和休眠 |
| `include/board_config.h` | GPIO 接线、BUSY 超时和软件 SPI 延时 |
| `include/display_config.h` | 屏幕尺寸、四色编码和取色回调类型 |
| `jd79668_driver.h/.cpp` | 控制器通信及屏幕刷新 |
| `epd_canvas.h/.cpp` | 两位像素缓冲和 Adafruit GFX 适配 |
| `text_renderer.h/.cpp` | UTF-8 字体绘制、字宽计算和居中 |
| `calendar_data.h/.cpp` | 月份数据结构和静态演示数据 |
| `calendar_view.h/.cpp` | 标题、星期栏、网格和日期排版 |

修改接线看 `board_config.h`，调整字体看 `text_renderer.cpp`，
修改演示数据看 `calendar_data.cpp`，调整布局看 `calendar_view.cpp`。

## 绘制流程

```text
月份数据 -> 日历排版 -> U8g2 / GFX -> 四色画布
         -> JD79668 发送像素 -> 全屏刷新 -> 屏幕休眠
```

程序只刷新一次，屏幕休眠后仍保留画面。农历标签目前为字体示例，
尚未接入公历转农历计算。

详细接口和中文调用示例见 [绘图说明](docs/display-guide.md)。
`reference/` 为参考工程，整理时不修改，也不参与本项目源码编译。
