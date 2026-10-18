#include "portal_http_server.h"

#include <cstring>
#include <new>

#include "image_transfer.h"
#include "network_time_config.h"
#include "secure_memory.h"

namespace {

bool decimalLength(const char* text, size_t& result) {
  result = 0;
  if (*text == '\0') return false;
  for (; *text; ++text) {
    if (*text < '0' || *text > '9' || result > (image_transfer::PACKET_BYTES - (*text - '0')) / 10) return false;
    result = result * 10 + (*text - '0');
  }
  return true;
}

bool headerNameValid(const char* value) {
  for (; *value; ++value) {
    const char c = *value;
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-')) return false;
  }
  return true;
}

}  // namespace

bool PortalHttpServer::reject(int status, const char* error) {
  sendHeader("Cache-Control", "no-store");
  sendHeader("X-Content-Type-Options", "nosniff");
  String body = "{\"error\":\"";
  body += error;
  body += "\"}";
  send(status, "application/json; charset=utf-8", body);
  return false;
}

void PortalHttpServer::resetClient() {
  if (_currentArgs != nullptr) {
    for (int i = 0; i < _currentArgCount; ++i) secure_memory::clear(_currentArgs[i].value);
    delete[] _currentArgs;
    _currentArgs = nullptr;
  }
  _currentArgCount = 0;
  for (int i = 0; i < _headerKeysCount; ++i) secure_memory::clear(_currentHeaders[i].value);
  _currentClient.stop();
  _currentClient = WiFiClient();
  _currentStatus = HC_NONE;
  _currentUpload.reset();
  _currentRaw.reset();
  _currentHandler = nullptr;
  _currentUri = "";
  _hostHeader = "";
  _responseHeaders = "";
  secure_memory::wipe(headerBuffer_, sizeof(headerBuffer_));
  secure_memory::wipe(formBuffer_, sizeof(formBuffer_));
  received_ = bodyReceived_ = 0;
  stage_ = Stage::Headers;
}

void PortalHttpServer::close() {
  resetClient();
  WebServer::close();
}

bool PortalHttpServer::parseHeaders() {
  _currentVersion = 1;
  _contentLength = CONTENT_LENGTH_NOT_SET;
  _chunked = false;
  _clientContentLength = 0;
  char* end = std::strstr(headerBuffer_, "\r\n");
  if (end == nullptr) return reject(400, "request_invalid");
  *end = '\0';
  char* path = std::strchr(headerBuffer_, ' ');
  if (path == nullptr) return reject(400, "request_invalid");
  *path++ = '\0';
  char* version = std::strchr(path, ' ');
  if (version == nullptr) return reject(400, "request_invalid");
  *version++ = '\0';
  if (std::strcmp(version, "HTTP/1.1") != 0 && std::strcmp(version, "HTTP/1.0") != 0) return reject(400, "request_invalid");
  if (std::strcmp(headerBuffer_, "GET") == 0) _currentMethod = HTTP_GET;
  else if (std::strcmp(headerBuffer_, "HEAD") == 0) _currentMethod = HTTP_HEAD;
  else if (std::strcmp(headerBuffer_, "POST") == 0) _currentMethod = HTTP_POST;
  else return reject(405, "request_method");
  if (*path != '/' || std::strlen(path) > 256 || std::strchr(path, '#') != nullptr) return reject(400, "request_invalid");
  // 配置参数仅接收 POST 正文，禁止把密码或 Key 放在 URL 中。
  if (std::strchr(path, '?') != nullptr) {
    if (std::strncmp(path, "/api/", 5) == 0) return reject(400, "request_invalid");
    *std::strchr(path, '?') = '\0';
  }
  _currentUri = path;
  bool lengthSeen = false;
  uint8_t count = 0;
  char* line = end + 2;
  while (*line) {
    end = std::strstr(line, "\r\n");
    if (end == nullptr) return reject(400, "request_invalid");
    if (end == line) break;
    if (++count > 32 || end - line > 1024) return reject(431, "request_headers");
    *end = '\0';
    char* value = std::strchr(line, ':');
    if (value == nullptr) return reject(400, "request_invalid");
    *value++ = '\0';
    if (*line == '\0' || !headerNameValid(line)) return reject(400, "request_invalid");
    while (*value == ' ' || *value == '\t') ++value;
    char* tail = value + std::strlen(value);
    while (tail != value && (tail[-1] == ' ' || tail[-1] == '\t')) *--tail = '\0';
    for (const char* p = value; *p; ++p) {
      if (*p == '\r' || *p == '\n') return reject(400, "request_invalid");
    }
    const String name(line);
    if (name.equalsIgnoreCase("Transfer-Encoding") || name.equalsIgnoreCase("Content-Encoding")) return reject(415, "request_encoding");
    if (name.equalsIgnoreCase("Expect")) return reject(417, "request_invalid");
    if (name.equalsIgnoreCase("Content-Length")) {
      size_t length;
      if (lengthSeen) return reject(400, "request_invalid");
      if (!decimalLength(value, length)) return reject(413, "request_too_large");
      lengthSeen = true;
      _clientContentLength = static_cast<int>(length);
    } else {
      if (name.equalsIgnoreCase("Host")) {
        if (_hostHeader.length() != 0 || *value == '\0') return reject(400, "request_invalid");
        _hostHeader = value;
      }
      for (int i = 0; i < _headerKeysCount; ++i) {
        if (_currentHeaders[i].key.equalsIgnoreCase(name) && _currentHeaders[i].value.length() != 0) return reject(400, "request_invalid");
      }
      _collectHeader(line, value);
    }
    line = end + 2;
  }
  const bool image = _currentMethod == HTTP_POST && _currentUri == "/api/image";
  if (_currentMethod != HTTP_POST && _clientContentLength != 0) return reject(400, "request_invalid");
  if (!image && _clientContentLength > static_cast<int>(FORM_LIMIT)) return reject(413, "request_too_large");
  if (_currentMethod == HTTP_POST) {
    if (!lengthSeen) return reject(411, "request_length");
    const String type = header("Content-Type");
    if (image) {
      if (_clientContentLength != static_cast<int>(image_transfer::PACKET_BYTES)) return reject(400, "image_size");
      if (type != "application/octet-stream") return reject(415, "image_header");
    } else if (type != "application/x-www-form-urlencoded" && type != "application/x-www-form-urlencoded;charset=UTF-8" &&
               type != "application/x-www-form-urlencoded; charset=UTF-8") return reject(415, "request_encoding");
  }
  sendHeader("X-Frame-Options", "DENY");
  sendHeader("X-Content-Type-Options", "nosniff");
  sendHeader("Referrer-Policy", "no-referrer");
  if (gate_ != nullptr && !gate_(*this)) return false;
  for (_currentHandler = _firstHandler; _currentHandler; _currentHandler = _currentHandler->next()) {
    if (_currentHandler->canHandle(_currentMethod, _currentUri)) break;
  }
  if (_currentMethod == HTTP_POST && _currentHandler == nullptr) return reject(404, "not_found");
  if (image) {
    if (!_currentHandler->canRaw(_currentUri)) return reject(400, "image_header");
    _currentRaw.reset(new (std::nothrow) HTTPRaw{});
    if (!_currentRaw) return reject(503, "status_memory");
    _currentRaw->status = RAW_START;
    _currentHandler->raw(*this, _currentUri, *_currentRaw);
    stage_ = Stage::Raw;
    return _currentClient.connected();
  }
  stage_ = Stage::Form;
  return true;
}

bool PortalHttpServer::parseForm() {
  // 限制字段数量并拒绝重复字段，避免歧义和堆分配放大。
  if (bodyReceived_ == 0) return true;
  size_t count = 1;
  for (size_t i = 0; i < bodyReceived_; ++i) if (formBuffer_[i] == '&') ++count;
  if (count > 16) return reject(400, "request_invalid");
  _currentArgs = new (std::nothrow) RequestArgument[count];
  if (_currentArgs == nullptr) return reject(503, "status_memory");
  char* field = formBuffer_;
  for (size_t i = 0; i < count; ++i) {
    char* next = std::strchr(field, '&');
    if (next) *next++ = '\0';
    char* equal = std::strchr(field, '=');
    if (equal == nullptr || equal == field) return reject(400, "request_invalid");
    *equal++ = '\0';
    // 在调用框架 URL 解码器前校验百分号编码，拒绝畸形序列。
    const char* parts[] = {field, equal};
    for (const char* part : parts) {
      for (const char* p = part; *p; ++p) if (*p == '%') {
        const auto hex = [](char c) { return (c >= '0' && c <= '9') ||
            (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); };
        if (p[1] == '\0' || p[2] == '\0' || !hex(p[1]) || !hex(p[2])) return reject(400, "request_invalid");
        p += 2;
      }
    }
    RequestArgument& argument = _currentArgs[_currentArgCount++];
    argument.key = urlDecode(field);
    argument.value = urlDecode(equal);
    if (argument.key.length() > 32 || std::strlen(argument.key.c_str()) != argument.key.length() ||
        std::strlen(argument.value.c_str()) != argument.value.length()) return reject(400, "request_invalid");
    for (int j = 0; j < _currentArgCount - 1; ++j) if (_currentArgs[j].key == argument.key) return reject(400, "request_invalid");
    field = next;
  }
  return true;
}

void PortalHttpServer::finishRequest() {
  _currentClient.setTimeout(HTTP_MAX_SEND_WAIT / 1000);
  _handleRequest();
  resetClient();
}

void PortalHttpServer::handleClient() {
  if (_currentStatus == HC_NONE) {
    _currentClient = _server.available();
    if (!_currentClient) return;
    _currentStatus = HC_WAIT_READ;
    startedAt_ = millis();
    _currentVersion = 1;
    _contentLength = CONTENT_LENGTH_NOT_SET;
    _chunked = false;
  }
  const uint32_t timeout = stage_ == Stage::Raw ? NetworkTimeConfig::IMAGE_UPLOAD_TIMEOUT_MS : 3000;
  if (!_currentClient.connected() || millis() - startedAt_ >= timeout) {
    if (stage_ == Stage::Raw && _currentRaw && _currentHandler) {
      _currentRaw->status = RAW_ABORTED;
      _currentHandler->raw(*this, _currentUri, *_currentRaw);
    }
    resetClient();
    return;
  }
  // 每轮有限量接收，校时、天气和热点关闭逻辑仍能在主循环运行。
  size_t budget = 2048;
  while (_currentClient.available() && budget-- != 0) {
    if (stage_ == Stage::Headers) {
      const int value = _currentClient.read();
      if (value < 0) break;
      if (value == 0 || (value < 32 && value != '\r' && value != '\n' && value != '\t') || value == 127) {
        reject(400, "request_invalid"); resetClient(); return;
      }
      if (received_ == HEADER_LIMIT) { reject(431, "request_headers"); resetClient(); return; }
      headerBuffer_[received_++] = static_cast<char>(value);
      if (received_ >= 4 && std::memcmp(headerBuffer_ + received_ - 4, "\r\n\r\n", 4) == 0) {
        if (!parseHeaders()) { resetClient(); return; }
        if (stage_ == Stage::Form && _clientContentLength == 0) { finishRequest(); return; }
      }
    } else if (stage_ == Stage::Form) {
      const int value = _currentClient.read();
      if (value < 0) break;
      if (value == 0) { reject(400, "request_invalid"); resetClient(); return; }
      formBuffer_[bodyReceived_++] = static_cast<char>(value);
      if (bodyReceived_ == static_cast<size_t>(_clientContentLength)) {
        if (parseForm()) finishRequest(); else resetClient();
        return;
      }
    } else {
      HTTPRaw& raw = *_currentRaw;
      size_t size = static_cast<size_t>(_currentClient.available());
      const size_t remaining = static_cast<size_t>(_clientContentLength) - raw.totalSize;
      if (size > sizeof(raw.buf)) size = sizeof(raw.buf);
      if (size > remaining) size = remaining;
      const int read = _currentClient.read(raw.buf, size);
      if (read <= 0) break;
      raw.currentSize = static_cast<size_t>(read);
      raw.totalSize += raw.currentSize;
      raw.status = RAW_WRITE;
      _currentHandler->raw(*this, _currentUri, raw);
      if (raw.totalSize == static_cast<size_t>(_clientContentLength)) {
        raw.status = RAW_END;
        _currentHandler->raw(*this, _currentUri, raw);
        finishRequest();
      }
      return;
    }
  }
}
