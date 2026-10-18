#pragma once

#include <Arduino.h>
#include <IPAddress.h>

namespace portal_security {

// 在无线启动后生成设备独立凭据；存储损坏时拒绝退回公共密码。
bool begin();
const char* ssid();
const char* hotspotPassword();
// 只允许首次生成或本地串口 P 命令输出，不通过网页返回。
void printLocalCredentials(Print& output);
bool loginAllowed();
bool login(const String& password, IPAddress client);
bool authenticated(const String& token, IPAddress client);
const char* token();
void endSession();

}  // namespace portal_security
