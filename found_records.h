#pragma once
#include "checkpoint_store.h"
#include <array>
#include <vector>
namespace btc {
struct FoundRecord {
  uint16_t puzzle = 0;
  std::array<uint8_t, 32> key{};
  bool compressed = true;
  bool pending = true;
  uint64_t attempts = 0;
  uint64_t elapsedMs = 0;
};
class FoundRecords {
public:
  explicit FoundRecords(Journal &journal) : journal_(journal) {}
  bool load();
  bool put(const FoundRecord &record);
  bool flush();
  bool clear(uint16_t puzzle); // zero means all records
  bool markDelivered(uint16_t puzzle);
  const FoundRecord *get(uint16_t puzzle) const;
  const std::vector<FoundRecord> &all() const { return records_; }
  bool dirty() const { return dirty_; }

private:
  Journal &journal_;
  std::vector<FoundRecord> records_;
  bool dirty_ = false;
};
} // namespace btc
