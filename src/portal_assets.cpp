#include "portal_assets.h"

#include <WebServer.h>

namespace {

bool acceptsGzip(String header) {
  // 没有该请求头表示未限制内容编码；显式 gzip;q=0 优先于通配符。
  if (header.length() == 0) return true;
  header.toLowerCase();
  bool wildcard = false, specified = false, accepted = false;
  int start = 0;
  while (start < static_cast<int>(header.length())) {
    int end = header.indexOf(',', start);
    if (end < 0) end = header.length();
    String entry = header.substring(start, end);
    const int separator = entry.indexOf(';');
    String name = separator < 0 ? entry : entry.substring(0, separator);
    name.trim();
    bool enabled = true;
    if (separator >= 0) {
      String parameter = entry.substring(separator + 1);
      parameter.trim();
      if (parameter.startsWith("q=")) {
        const float quality = parameter.substring(2).toFloat();
        enabled = quality > 0 && quality <= 1;
      }
    }
    if (name == "gzip") { specified = true; accepted = enabled; }
    if (name == "*") wildcard = enabled;
    start = end + 1;
  }
  return specified ? accepted : wildcard;
}

bool matchesEtag(const String& header, const char* etag) {
  int start = 0;
  while (start < static_cast<int>(header.length())) {
    int end = header.indexOf(',', start);
    if (end < 0) end = header.length();
    String value = header.substring(start, end);
    value.trim();
    if (value.startsWith("W/")) value = value.substring(2);
    if (value == "*" || value == etag) return true;
    start = end + 1;
  }
  return false;
}

}  // namespace

namespace portal_assets {

void send(WebServer& server, Asset asset, bool cache) {
  const Resource& resource = RESOURCES[static_cast<size_t>(asset)];
  server.sendHeader("Vary", "Accept-Encoding");
  if (resource.gzip && !acceptsGzip(server.header("Accept-Encoding"))) {
    server.sendHeader("Cache-Control", "no-store");
    server.send(406, "text/plain; charset=utf-8", "This page requires gzip support.");
    return;
  }
  if (resource.gzip) server.sendHeader("Content-Encoding", "gzip");
  // 每次重用先校验内容指纹，升级固件后不会沿用旧脚本；API 另行禁止缓存。
  server.sendHeader("Cache-Control", cache ? "public, max-age=0, must-revalidate" : "no-store");
  if (cache) {
    server.sendHeader("ETag", resource.etag);
    if (matchesEtag(server.header("If-None-Match"), resource.etag)) {
      server.setContentLength(resource.length);
      server.send(304, resource.contentType, "");
      return;
    }
  }
  if (server.method() == HTTP_HEAD) {
    server.setContentLength(resource.length);
    server.send(200, resource.contentType, "");
    return;
  }
  server.send_P(200, resource.contentType, reinterpret_cast<const char*>(resource.bytes), resource.length);
}

void registerRoutes(WebServer& server) {
  // 根页面保留配网模块的 IP 跳转逻辑，只统一注册其余静态资源。
  for (size_t i = 1; i < static_cast<size_t>(Asset::Count); ++i) {
    const Asset asset = static_cast<Asset>(i);
    const auto handler = [&server, asset]() { send(server, asset); };
    server.on(RESOURCES[i].path, HTTP_GET, handler);
    server.on(RESOURCES[i].path, HTTP_HEAD, handler);
  }
}

}  // namespace portal_assets
