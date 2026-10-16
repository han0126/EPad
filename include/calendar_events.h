#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

#include "calendar_data.h"

namespace calendar_events {

constexpr size_t MAX_ENTRIES = 64;
constexpr size_t MAX_TEXT_CHARACTERS = 15;
constexpr size_t TEXT_CAPACITY = MAX_TEXT_CHARACTERS * 3 + 1;

enum class Kind : uint8_t { Important, Memo };
enum class Error : uint8_t {
  None, Storage, InvalidDate, InvalidKind, InvalidText, UnsupportedCharacter,
  Conflict, Full, NotFound, Stale
};

struct Entry {
  uint32_t id;
  calendar::Date date;  // 重要日只匹配月日；备忘匹配完整年月日。
  Kind kind;
  char text[TEXT_CAPACITY];
};

// 显示队列只复制当前月份的标记和目标日期正文，不携带完整记录表。
struct DisplayData {
  uint32_t importantDays;  // 第 0 位对应 1 日，第 30 位对应 31 日。
  char text[TEXT_CAPACITY];
};

// 此模块只由主任务访问，成功提交 NVS 后才更新内存记录。
void begin();
bool isReady();
uint32_t revision();
size_t count();
const Entry* entry(size_t index);
DisplayData displayDataForDate(const calendar::Date& date);
bool sameDisplayData(const DisplayData& left, const DisplayData& right);
Error upsert(uint32_t id, uint32_t expectedRevision, Kind kind,
             const calendar::Date& date, const String& text, uint32_t& savedId);
Error remove(uint32_t id, uint32_t expectedRevision);
// 只在实际日期的日历成功刷新后调用，批量删除早于今天的备忘。
Error removeExpired(const calendar::Date& today, size_t& removedCount);
const char* errorName(Error error);

}  // namespace calendar_events
