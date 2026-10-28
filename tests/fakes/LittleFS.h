#pragma once
#include <map>
#include <memory>
#include <string>
#include <cstring>
class File {
  std::shared_ptr<std::string> data;

public:
  File() = default;
  File(std::shared_ptr<std::string> s) : data(s) {}
  explicit operator bool() const { return bool(data); }
  size_t size() const { return data ? data->size() : 0; }
  size_t readBytes(char *p, size_t n) {
    if (!data)
      return 0;
    n = std::min(n, data->size());
    memcpy(p, data->data(), n);
    return n;
  }
  size_t write(const uint8_t *p, size_t n) {
    if (!data)
      return 0;
    data->append(reinterpret_cast<const char *>(p), n);
    return n;
  }
  void flush() {}
  void close() { data.reset(); }
};
struct FakeLittleFS {
  std::map<std::string, std::shared_ptr<std::string>> files;
  bool mountOk = true;
  bool formatted = false;
  bool begin(bool format) {
    if (mountOk)
      return true;
    if (format) {
      files.clear();
      formatted = true;
      mountOk = true;
      return true;
    }
    return false;
  }
  File open(const char *p, const char *mode) {
    if (*mode == 'w')
      files[p] = std::make_shared<std::string>();
    return files.count(p) ? File(files[p]) : File();
  }
};
extern FakeLittleFS LittleFS;
