#pragma once

#include <stddef.h>
#include <stdint.h>

class WebServer;

namespace portal_assets {

enum class Asset : uint8_t {
  Page, ImageTools, ImageAlgorithms, ImageCropper, CropperScript, CropperStyle,
  CropperLicense, WeatherIcons, WeatherPreview, WeatherCities, CalendarEvents, Count
};

struct Resource {
  const uint8_t* bytes;
  size_t length;
  const char* path;
  const char* contentType;
  const char* etag;
  bool gzip;
};

// 由编译前脚本生成；所有数据直接驻留 Flash，不在设备上压缩。
extern const Resource RESOURCES[];
void send(WebServer& server, Asset asset, bool cache = true);
void registerRoutes(WebServer& server);

}  // namespace portal_assets
