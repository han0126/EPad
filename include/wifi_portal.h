#pragma once

class Print;

namespace wifi_portal {

// 开启限时设备热点和 HTML 配网页面，已保存的网络自动尝试校时。
bool begin();
// 在 loop() 中处理网页请求、校时状态和自动关闭倒计时。
void handle();
bool isActive();
// 仅主循环调用；屏幕或上传忙时拒绝关闭，网络关闭结果由电源服务复核。
bool close();
// 本地串口查看设备独立热点密码和管理码，不提供 HTTP 接口。
void printLocalCredentials(Print& output);

}  // namespace wifi_portal
