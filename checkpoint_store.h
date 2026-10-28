#pragma once
#include <cstdint>
#include <string>
namespace btc {
class FileStore {
public:
  virtual ~FileStore() = default;
  virtual bool read(const std::string &path, std::string &data) = 0;
  // Return true only after all bytes have been written and flushed.
  virtual bool write(const std::string &path, const std::string &data) = 0;
};
// The previous valid slot is never removed or overwritten during a checkpoint.
class Journal {
public:
  Journal(FileStore &files, const char *a, const char *b) : files_(files), a_(a), b_(b) {}
  bool load(std::string &payload);
  bool save(const std::string &payload);
  bool hasFiles() const { return hasFiles_; }

private:
  FileStore &files_;
  std::string a_, b_;
  uint64_t sequence_ = 0;
  bool initialized_ = false;
  bool hasFiles_ = false;
};
} // namespace btc
