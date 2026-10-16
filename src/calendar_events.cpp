#include "calendar_events.h"

#include <U8g2_for_Adafruit_GFX.h>
#include <nvs.h>

#include <cstring>
#include <memory>
#include <new>

namespace {

constexpr char STORAGE_NAMESPACE[] = "epd-events";
constexpr char RECORD_KEY[] = "records";
constexpr uint32_t VERSION = 1;

struct Record {
  uint32_t version;
  uint32_t revision;
  uint32_t nextId;
  uint16_t count;
  uint16_t reserved;
  calendar_events::Entry entries[calendar_events::MAX_ENTRIES];
};

Record records = {};
bool ready = false;
using calendar_events::Error;
using calendar_events::Kind;

Error validateText(const char* text, size_t length) {
  if (length == 0 || length >= calendar_events::TEXT_CAPACITY) return Error::InvalidText;
  // 独立字库上下文只查询字形，不访问显示任务共用的字体对象。
  u8g2_font_t font = {};
  u8g2_SetFont(&font, u8g2_font_wqy12_t_gb2312);
  size_t characters = 0;
  bool visible = false;
  for (size_t offset = 0; offset < length;) {
    const uint8_t first = static_cast<uint8_t>(text[offset]);
    uint16_t code;
    size_t bytes;
    if (first < 0x80) { code = first; bytes = 1; }
    else if (first >= 0xC2 && first <= 0xDF) { code = first & 0x1F; bytes = 2; }
    else if (first >= 0xE0 && first <= 0xEF) { code = first & 0x0F; bytes = 3; }
    else if (first >= 0xF0 && first <= 0xF4) { return Error::UnsupportedCharacter; }
    else { return Error::InvalidText; }
    if (bytes > length - offset) return Error::InvalidText;
    for (size_t i = 1; i < bytes; ++i) {
      const uint8_t next = static_cast<uint8_t>(text[offset + i]);
      if ((next & 0xC0) != 0x80) return Error::InvalidText;
      code = static_cast<uint16_t>((code << 6) | (next & 0x3F));
    }
    // 禁止无效 UTF-8、控制字符、换行和代理项，正文始终为单行。
    if ((bytes == 3 && code < 0x800) || (code >= 0xD800 && code <= 0xDFFF) ||
        code < 0x20 || (code >= 0x7F && code <= 0x9F) || code >= 0xFFFE) return Error::InvalidText;
    if (++characters > calendar_events::MAX_TEXT_CHARACTERS) return Error::InvalidText;
    if (!u8g2_IsGlyph(&font, code) || u8g2_GetGlyphWidth(&font, code) <= 0) return Error::UnsupportedCharacter;
    const bool blank = code == 0x20 || code == 0xA0 || code == 0x1680 ||
        (code >= 0x2000 && code <= 0x200A) || code == 0x202F || code == 0x205F || code == 0x3000;
    if (!blank) visible = true;
    offset += bytes;
  }
  return visible ? Error::None : Error::InvalidText;
}

bool conflicts(const calendar_events::Entry& left, const calendar_events::Entry& right) {
  if (left.date.month != right.date.month || left.date.day != right.date.day) return false;
  return left.kind == Kind::Important || right.kind == Kind::Important ||
      left.date.year == right.date.year;
}

bool expiredMemo(const calendar_events::Entry& item, const calendar::Date& today) {
  if (item.kind != Kind::Memo) return false;
  if (item.date.year != today.year) return item.date.year < today.year;
  if (item.date.month != today.month) return item.date.month < today.month;
  return item.date.day < today.day;
}

bool validRecord(const Record& record) {
  if (record.version != VERSION || record.reserved != 0 || record.nextId == 0 ||
      record.count > calendar_events::MAX_ENTRIES) return false;
  for (size_t i = 0; i < record.count; ++i) {
    const calendar_events::Entry& item = record.entries[i];
    if (item.id == 0 || item.id >= record.nextId || !calendar::isValidDate(item.date) ||
        (item.kind != Kind::Important && item.kind != Kind::Memo) ||
        item.text[calendar_events::TEXT_CAPACITY - 1] != '\0' ||
        validateText(item.text, std::strlen(item.text)) != Error::None) return false;
    for (size_t j = 0; j < i; ++j) {
      if (record.entries[j].id == item.id || conflicts(record.entries[j], item)) return false;
    }
  }
  return true;
}

Error commit(const Record& candidate) {
  nvs_handle_t handle;
  if (nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
    ready = false;
    return Error::Storage;
  }
  const bool saved = nvs_set_blob(handle, RECORD_KEY, &candidate, sizeof(candidate)) == ESP_OK &&
      nvs_commit(handle) == ESP_OK;
  nvs_close(handle);
  // 保存状态不确定时停止写入，重新启动读取持久记录后再操作。
  if (!saved) { ready = false; return Error::Storage; }
  records = candidate;
  return Error::None;
}

}  // namespace

namespace calendar_events {

void begin() {
  records = {};
  records.version = VERSION;
  records.nextId = 1;
  ready = false;
  nvs_handle_t handle;
  const esp_err_t opened = nvs_open(STORAGE_NAMESPACE, NVS_READONLY, &handle);
  if (opened == ESP_ERR_NVS_NOT_FOUND) { ready = true; return; }
  if (opened != ESP_OK) return;
  size_t length = 0;
  const esp_err_t found = nvs_get_blob(handle, RECORD_KEY, nullptr, &length);
  if (found == ESP_ERR_NVS_NOT_FOUND) { nvs_close(handle); ready = true; return; }
  if (found != ESP_OK || length != sizeof(Record)) { nvs_close(handle); return; }
  // 完整记录放在堆上，避免大数组占用 Arduino 主任务栈。
  std::unique_ptr<Record> loaded(new (std::nothrow) Record{});
  if (!loaded) { nvs_close(handle); return; }
  const esp_err_t result = nvs_get_blob(handle, RECORD_KEY, loaded.get(), &length);
  nvs_close(handle);
  if (result != ESP_OK || length != sizeof(Record) || !validRecord(*loaded)) return;
  records = *loaded;
  ready = true;
}

bool isReady() { return ready; }
uint32_t revision() { return records.revision; }
size_t count() { return records.count; }
const Entry* entry(size_t index) { return index < records.count ? &records.entries[index] : nullptr; }

DisplayData displayDataForDate(const calendar::Date& date) {
  DisplayData data = {};
  if (!calendar::isValidDate(date)) return data;
  for (size_t i = 0; i < records.count; ++i) {
    const Entry& item = records.entries[i];
    if (item.date.month != date.month) continue;
    if (item.kind == Kind::Important) {
      const calendar::Date occurrence = {date.year, item.date.month, item.date.day};
      // 2 月 29 日只在闰年标记，不挪到其他日期。
      if (!calendar::isValidDate(occurrence)) continue;
      data.importantDays |= uint32_t{1} << (item.date.day - 1);
    }
    if (item.date.day == date.day && (item.kind == Kind::Important || item.date.year == date.year)) {
      std::memcpy(data.text, item.text, sizeof(data.text));
    }
  }
  return data;
}

bool sameDisplayData(const DisplayData& left, const DisplayData& right) {
  return left.importantDays == right.importantDays &&
      std::strncmp(left.text, right.text, TEXT_CAPACITY) == 0;
}

Error upsert(uint32_t id, uint32_t expectedRevision, Kind kind,
             const calendar::Date& date, const String& text, uint32_t& savedId) {
  savedId = 0;
  if (!ready) return Error::Storage;
  if (expectedRevision != records.revision) return Error::Stale;
  if (kind != Kind::Important && kind != Kind::Memo) return Error::InvalidKind;
  if (!calendar::isValidDate(date)) return Error::InvalidDate;
  const Error textError = validateText(text.c_str(), text.length());
  if (textError != Error::None) return textError;
  size_t index = records.count;
  if (id != 0) {
    for (size_t i = 0; i < records.count; ++i) {
      if (records.entries[i].id == id) { index = i; break; }
    }
    if (index == records.count) return Error::NotFound;
  } else if (records.count == MAX_ENTRIES || records.nextId == UINT32_MAX) {
    return Error::Full;
  }
  Entry value = {};
  value.id = id != 0 ? id : records.nextId;
  value.date = date;
  value.kind = kind;
  std::memcpy(value.text, text.c_str(), text.length());
  for (size_t i = 0; i < records.count; ++i) {
    if (i != index && conflicts(records.entries[i], value)) return Error::Conflict;
  }
  if (id != 0 && calendar::sameDate(records.entries[index].date, date) &&
      records.entries[index].kind == kind && std::strcmp(records.entries[index].text, value.text) == 0) {
    savedId = id;
    return Error::None;
  }
  if (records.revision == UINT32_MAX) return Error::Storage;
  std::unique_ptr<Record> candidate(new (std::nothrow) Record(records));
  if (!candidate) return Error::Storage;
  candidate->entries[index] = value;
  if (id == 0) { ++candidate->count; ++candidate->nextId; }
  ++candidate->revision;
  const Error result = commit(*candidate);
  if (result == Error::None) savedId = value.id;
  return result;
}

Error remove(uint32_t id, uint32_t expectedRevision) {
  if (!ready) return Error::Storage;
  if (expectedRevision != records.revision) return Error::Stale;
  size_t index = records.count;
  for (size_t i = 0; i < records.count; ++i) {
    if (records.entries[i].id == id) { index = i; break; }
  }
  if (index == records.count) return Error::NotFound;
  if (records.revision == UINT32_MAX) return Error::Storage;
  std::unique_ptr<Record> candidate(new (std::nothrow) Record(records));
  if (!candidate) return Error::Storage;
  for (size_t i = index + 1; i < candidate->count; ++i) candidate->entries[i - 1] = candidate->entries[i];
  candidate->entries[--candidate->count] = {};
  ++candidate->revision;
  return commit(*candidate);
}

Error removeExpired(const calendar::Date& today, size_t& removedCount) {
  removedCount = 0;
  if (!ready) return Error::Storage;
  if (!calendar::isValidDate(today)) return Error::InvalidDate;
  size_t expiredCount = 0;
  for (size_t i = 0; i < records.count; ++i) {
    if (expiredMemo(records.entries[i], today)) ++expiredCount;
  }
  // 没有过期记录就不写 Flash，重要日不会进入删除分支。
  if (expiredCount == 0) return Error::None;
  if (records.revision == UINT32_MAX) return Error::Storage;
  std::unique_ptr<Record> candidate(new (std::nothrow) Record(records));
  if (!candidate) return Error::Storage;
  size_t kept = 0;
  for (size_t i = 0; i < records.count; ++i) {
    if (!expiredMemo(records.entries[i], today)) candidate->entries[kept++] = records.entries[i];
  }
  for (size_t i = kept; i < records.count; ++i) candidate->entries[i] = {};
  candidate->count = static_cast<uint16_t>(kept);
  ++candidate->revision;
  // 一次提交完成整批清理；失败时不发布新的内存记录和删除数量。
  const Error result = commit(*candidate);
  if (result == Error::None) removedCount = expiredCount;
  return result;
}

const char* errorName(Error error) {
  switch (error) {
    case Error::None: return "none";
    case Error::InvalidDate: return "events_date";
    case Error::InvalidKind: return "events_kind";
    case Error::InvalidText: return "events_text";
    case Error::UnsupportedCharacter: return "events_character";
    case Error::Conflict: return "events_conflict";
    case Error::Full: return "events_full";
    case Error::NotFound: return "events_not_found";
    case Error::Stale: return "events_stale";
    default: return "events_storage";
  }
}

}  // namespace calendar_events
