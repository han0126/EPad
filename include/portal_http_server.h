#pragma once

#include <WebServer.h>

// 适配 Arduino-ESP32 2.x 的有界解析层，复用 WebServer 的路由与响应。
// 不调用其无界的表单解析入口，也不修改框架缓存中的源文件。
class PortalHttpServer : public WebServer {
 public:
  explicit PortalHttpServer(IPAddress address) : WebServer(address, 80) {}
  void handleClient() override;
  void close() override;
  void setRequestGate(bool (*gate)(WebServer&)) { gate_ = gate; }

 private:
  enum class Stage : uint8_t { Headers, Form, Raw };
  static constexpr size_t HEADER_LIMIT = 4096;
  static constexpr size_t FORM_LIMIT = 2048;
  char headerBuffer_[HEADER_LIMIT + 1] = {};
  char formBuffer_[FORM_LIMIT + 1] = {};
  size_t received_ = 0;
  size_t bodyReceived_ = 0;
  uint32_t startedAt_ = 0;
  Stage stage_ = Stage::Headers;
  bool (*gate_)(WebServer&) = nullptr;

  bool parseHeaders();
  bool parseForm();
  void finishRequest();
  void resetClient();
  bool reject(int status, const char* error);
};
