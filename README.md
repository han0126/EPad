# JD79668 四色墨水屏日历

ESP32-S3 + Arduino + PlatformIO，驱动 400 × 300 黑、白、黄、红四色屏。

## 功能

- 日历：1901—2099 年公历、农历、节气和常见节日，自动适配月份行数；每日零点后唤醒、联网校时、刷新并深睡。
- 天气：和风 API KEY 认证，支持预设地区、国内城市在线搜索和自定义坐标。每日自动尝试一次；手动保存不同坐标时额外请求一次，同坐标重复保存不重复请求。
- 重要日与备忘：网页管理，每日最多一条、正文最多 15 个字符。重要日按月日每年重复并永久保存；过期备忘在实际日历成功刷新后删除。
- 图片：网页上传、旋转和手动裁切，提供原有方案、四色照片、四色平滑、感知扩散、亮度自适应、蓝噪声、结构自适应和感知迭代，共 8 种算法。
- 新增算法支持灰度保护、计算色板，感知迭代支持 1—12 轮；均在浏览器端处理，设备只接收 30,000 字节四色像素和校验信息。
- 日历／图片模式记入 NVS，断电后保留。图片成功显示后约 3 秒关闭无线、取消自动唤醒并深睡；复位或重新上电可再次配置。

## 使用

1. 上电或复位，连接热点 `EPD-Calendar`，密码 `calendar1234`。
2. 浏览器打开 `http://192.168.4.1/`，选择日历或图片模式。
3. 日历模式配置 2.4 GHz Wi-Fi；天气填写专属 API Host 和 KEY，城市搜索需具备 GeoAPI 调用权限。
4. 图片支持 PNG、JPEG、WebP，最大 10 MiB、1600 万像素，任一边不超过 8192 像素；预览完成后上传，刷屏时勿断电。

图片处理及上传不需要外网；校时、天气和在线城市搜索需要外网。定时唤醒不开热点；日历配网初始窗口为 5 分钟，校时成功后通常保留 2 分钟，图片配置窗口为 10 分钟。

## 文件

- `src/`、`include/`：屏幕驱动、日历、联网、天气、事件记录和电源管理。
- `web/`：随固件嵌入的离线网页；`image_tools.js` 负责图片处理与上传，`image_algorithms.js` 提供五种新增算法。
- `lib/sxtwl/`：农历依赖，不可删除。
- `assets/qweather/`：图标许可；`web/vendor/`：离线裁切组件及许可，均需保留。
- `tools/image_algorithm_lab.html`：可直接在电脑浏览器打开的独立算法对比页。
- `tools/generate_weather_icons.ps1`、`tools/weather_conditions.json`：天气图标再生成工具及输入数据。

板型与依赖见 `platformio.ini`，接线见 `include/board_config.h`。正常使用时 `include/power_config.h` 的 `WAKE_INTERVAL_OVERRIDE_SECONDS` 应为 `0`。

## 编译烧录

```sh
pio run
pio run -t upload
pio device monitor -b 115200
```

依赖许可保留在 `lib/sxtwl/LICENSE`、`assets/qweather/NOTICE.txt`、`web/vendor/cropper.LICENSE.txt`。勿提交真实密码或密钥；可选本地配置 `include/wifi_credentials.h` 已忽略。更新后需自行编译和实物确认。
