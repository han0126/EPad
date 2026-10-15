# JD79668 四色墨水屏

基于 ESP32-S3、Arduino 和 PlatformIO，驱动 400 × 300 黑、白、红、黄四色墨水屏。
支持自动日历和网页传图，两个模式互斥。

## 使用方式

1. 上电或按复位键，连接热点 `EPD-Calendar`，默认密码 `calendar1234`。
2. 用浏览器打开 `http://192.168.4.1`，选择显示模式。
3. 日历模式：配置 2.4 GHz Wi-Fi，联网校准北京时间。凭据校时成功后保存到 NVS。
4. 图片模式：选择 PNG、JPEG 或 WebP 图片，预览四色效果后上传；支持旋转、完整显示、铺满裁切和色彩抖动。原图最大 10 MB、4000 万像素，图片传输不依赖外网。

日历每天北京时间约 00:00:02 唤醒，尝试一次校时，日期变化后刷新并深睡。
实际更新还需联网和刷屏时间；联网失败时沿用有效保留时钟，长期断网会累积误差。
支持 1901—2099 年公历、农历、节气及常见节日，自动适配月份行数。

图片在浏览器转换为 400 × 300 两位像素数据，设备校验尺寸、长度和 CRC32 后刷屏。
成功后约三秒自动关闭无线、取消全部自动唤醒并深睡，不能继续在网页换图。
重新配置请复位或断电重接，再连接热点并刷新网页。不要在刷屏过程中断电。
上传、校验或刷屏失败时不进入永久深睡，热点窗口内仍可重试。

串口波特率为 `115200`。串口 `W` 仅在清醒且图片尚未成功锁定时开启配置，不能唤醒深睡设备。

## 代码结构

公共头文件位于 `include/`，同名实现位于 `src/`。

| 文件或模块 | 作用 |
| --- | --- |
| `main.cpp` | 启动和主循环调度 |
| `display_service` | 日历、图片共用的异步显示任务，串行访问屏幕 |
| `display_mode` | 显示模式选择及未完成图片操作的退出处理 |
| `calendar_data`、`lunar_calendar` | 日历数据、农历换算、节气和节日标签 |
| `calendar_view`、`text_renderer` | 日历布局和 UTF-8 字体绘制 |
| `epd_canvas`、`jd79668_driver` | 两位像素画布和屏幕硬件驱动 |
| `clock_service`、`wifi_connection` | 非阻塞校时、RTC 快连缓存及扫描回退 |
| `network_settings`、`wifi_portal` | NVS 凭据、热点、网页及 HTTP 接口 |
| `image_transfer` | 图片流接收、校验和显示提交 |
| `power_service` | 每日定时唤醒和图片常驻深睡 |
| `web/wifi_setup.html`、`web/image_tools.js` | 离线网页、图片转换和上传，随固件嵌入 |
| `lib/sxtwl` | 农历与节气计算核心 |

## 常用配置

- `include/board_config.h`：接线及驱动时序；默认 BUSY=13、CS=10、RST=14、DC=9、SCK=12、SDA=11。
- `include/display_config.h`：尺寸、帧大小及颜色编码；黑=0、白=1、黄=2、红=3。
- `src/calendar_view.cpp`：日期、农历文字、黄色标签背景及当日边框的位置。
- `include/network_time_config.h`：时区、NTP 服务器、热点名称、密码和超时。
- `include/power_config.h`：图片深睡提示时间、错误重试间隔及唤醒参数。`WAKE_INTERVAL_OVERRIDE_SECONDS` 正常使用须为 `0`；非零仅用于日历实物评估，不影响图片永久深睡。

优先通过网页配网。可选的本地配置格式见 `include/wifi_credentials.example.h`，实际 `wifi_credentials.h` 已被 Git 忽略，不要提交真实密码。

## 编译烧录

在项目根目录使用 PlatformIO；板型和依赖见 `platformio.ini`：

```sh
pio run
pio run -t upload
pio device monitor -b 115200
```

中文使用 U8g2 文泉驿 GB2312 字库，绘图依赖 Adafruit GFX。
农历核心使用 `sxtwl`，保留其 BSD-3-Clause 许可证，详见 `lib/sxtwl/LICENSE`。
`reference/` 中的 jcalendar 仅用于方案参考，不参与固件编译。

修改后需编译并上机确认；实际深睡电流受开发板稳压器、指示灯和屏幕适配板影响。
