"""Generate deterministic compressed portal resources during PlatformIO builds."""

import gzip
import hashlib
import io
import json
from pathlib import Path

Import("env")

ASSETS = (
    ("/", "web/wifi_setup.html", "text/html; charset=utf-8"),
    ("/image-tools.js", "web/image_tools.js", "application/javascript; charset=utf-8"),
    ("/image-algorithms.js", "web/image_algorithms.js", "application/javascript; charset=utf-8"),
    ("/image-cropper.js", "web/image-cropper.js", "application/javascript; charset=utf-8"),
    ("/cropper.min.js", "web/vendor/cropper.min.js", "application/javascript; charset=utf-8"),
    ("/cropper.min.css", "web/vendor/cropper.min.css", "text/css; charset=utf-8"),
    ("/cropper-license.txt", "web/vendor/cropper.LICENSE.txt", "text/plain; charset=utf-8"),
    ("/weather-icons.json", "web/weather-icons.json", "application/json; charset=utf-8"),
    ("/weather-preview.js", "web/weather-preview.js", "application/javascript; charset=utf-8"),
    ("/weather-cities.js", "web/weather-cities.js", "application/javascript; charset=utf-8"),
    ("/calendar-events.js", "web/calendar-events.js", "application/javascript; charset=utf-8"),
)

project = Path(env.subst("$PROJECT_DIR"))
generated = Path(env.subst("$BUILD_DIR")) / "generated-portal"
generated.mkdir(parents=True, exist_ok=True)
lines = ['#include <Arduino.h>', '#include "portal_assets.h"', '',
         'namespace portal_assets {', 'namespace {']
resources = []
for index, (route, relative, content_type) in enumerate(ASSETS):
    original = (project / relative).read_bytes()
    output = io.BytesIO()
    # 固定时间戳且不记录原文件名，避免无内容变化也重编资源。
    with gzip.GzipFile(fileobj=output, mode="wb", filename="", mtime=0, compresslevel=9) as stream:
        stream.write(original)
    compressed = output.getvalue()
    use_gzip = len(compressed) < len(original)
    data = compressed if use_gzip else original
    name = "ASSET_%d" % index
    lines.append("const uint8_t %s[] PROGMEM = {" % name)
    for offset in range(0, len(data), 16):
        lines.append("  " + ", ".join("0x%02x" % byte for byte in data[offset:offset + 16]) + ",")
    lines.append("};")
    etag = '"' + hashlib.sha256(data).hexdigest()[:32] + '"'
    resources.append("  {%s, sizeof(%s), %s, %s, %s, %s}," % (
        name, name, json.dumps(route), json.dumps(content_type), json.dumps(etag),
        "true" if use_gzip else "false"))
lines.extend(['}  // namespace', 'const Resource RESOURCES[] = {', *resources, '};',
              'static_assert(sizeof(RESOURCES) / sizeof(RESOURCES[0]) == static_cast<size_t>(Asset::Count),',
              '              "Portal resource count mismatch");', '}  // namespace portal_assets', ''])
source = "\n".join(lines)
target = generated / "portal_resources.cpp"
if not target.exists() or target.read_text(encoding="utf-8") != source:
    with target.open("w", encoding="utf-8", newline="\n") as stream:
        stream.write(source)

# 预构建脚本早于项目源码配置，额外资源对象必须显式加入项目头文件目录。
env.AppendUnique(CPPPATH=[env.subst("$PROJECT_INCLUDE_DIR")])
env.BuildSources(str(generated.parent / "portal-assets-objects"), str(generated), src_filter="+<portal_resources.cpp>")
