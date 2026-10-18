#pragma once

#include <Arduino.h>
#include <stddef.h>

namespace secure_memory {

inline void wipe(void* data, size_t length) {
  volatile uint8_t* bytes = static_cast<volatile uint8_t*>(data);
  while (length-- != 0) *bytes++ = 0;
}

// 清理当前有效内容；不承诺擦除 String 曾经分配过的全部堆空间。
inline void clear(String& value) {
  if (value.length() != 0) wipe(&value[0], value.length());
  value = "";
}

class ScopedWipe {
 public:
  ScopedWipe(void* data, size_t length) : data_(data), length_(length) {}
  ~ScopedWipe() { wipe(data_, length_); }
  ScopedWipe(const ScopedWipe&) = delete;
  ScopedWipe& operator=(const ScopedWipe&) = delete;

 private:
  void* data_;
  size_t length_;
};

}  // namespace secure_memory
