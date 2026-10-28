#include "found_records.h"
#include "search_core.h"
#include <algorithm>

namespace btc {
namespace {
bool number(const std::string &s, uint64_t &n) {
  if (s.empty())
    return false;
  n = 0;
  for (char c : s) {
    if (c < '0' || c > '9' || n > (UINT64_MAX - unsigned(c - '0')) / 10)
      return false;
    n = n * 10 + unsigned(c - '0');
  }
  return true;
}
std::string encodeRecords(const std::vector<FoundRecord> &records) {
  std::string out;
  for (const auto &r : records) {
    out += std::to_string(r.puzzle) + " " + (r.compressed ? "1 " : "0 ") +
           (r.pending ? "1 " : "0 ") + std::to_string(r.attempts) + " " +
           std::to_string(r.elapsedMs) + " " + hex(r.key.data(), r.key.size()) + "\n";
  }
  return out;
}
} // namespace
bool FoundRecords::load() {
  std::string payload;
  if (!journal_.load(payload))
    return !journal_.hasFiles();
  std::vector<FoundRecord> loaded;
  size_t start = 0;
  while (start < payload.size()) {
    const size_t end = payload.find('\n', start);
    if (end == std::string::npos)
      return false;
    const std::string line = payload.substr(start, end - start);
    start = end + 1;
    std::string fields[6];
    size_t pos = 0;
    for (unsigned i = 0; i < 5; ++i) {
      const size_t split = line.find(' ', pos);
      if (split == std::string::npos)
        return false;
      fields[i] = line.substr(pos, split - pos);
      pos = split + 1;
    }
    fields[5] = line.substr(pos);
    uint64_t id = 0, compressed = 0, pending = 0;
    FoundRecord r;
    if (!number(fields[0], id) || !number(fields[1], compressed) || !number(fields[2], pending) ||
        !number(fields[3], r.attempts) || !number(fields[4], r.elapsedMs) || id < 1 || id > 160 ||
        compressed > 1 || pending > 1 || !unhex(fields[5], r.key.data(), 32))
      return false;
    if (std::any_of(loaded.begin(), loaded.end(),
                    [id](const FoundRecord &x) { return x.puzzle == id; }))
      return false;
    r.puzzle = id;
    r.compressed = compressed;
    r.pending = pending;
    loaded.push_back(r);
  }
  records_ = std::move(loaded);
  dirty_ = false;
  return true;
}
const FoundRecord *FoundRecords::get(uint16_t id) const {
  for (const auto &r : records_)
    if (r.puzzle == id)
      return &r;
  return nullptr;
}
bool FoundRecords::flush() {
  if (!dirty_)
    return true;
  if (!journal_.save(encodeRecords(records_)))
    return false;
  dirty_ = false;
  return true;
}
bool FoundRecords::put(const FoundRecord &record) {
  if (record.puzzle < 1 || record.puzzle > 160)
    return false;
  auto found = std::find_if(records_.begin(), records_.end(),
                            [&](const FoundRecord &r) { return r.puzzle == record.puzzle; });
  if (found == records_.end())
    records_.push_back(record);
  else
    *found = record;
  dirty_ = true;
  return flush();
}
bool FoundRecords::markDelivered(uint16_t id) {
  for (auto &r : records_)
    if (r.puzzle == id) {
      r.pending = false;
      dirty_ = true;
      return flush();
    }
  return false;
}
bool FoundRecords::clear(uint16_t id) {
  auto remaining = records_;
  remaining.erase(std::remove_if(remaining.begin(), remaining.end(),
                                 [id](const FoundRecord &r) { return id == 0 || r.puzzle == id; }),
                  remaining.end());
  if (!journal_.save(encodeRecords(remaining)))
    return false;
  records_ = std::move(remaining);
  dirty_ = false;
  return true;
}
} // namespace btc
