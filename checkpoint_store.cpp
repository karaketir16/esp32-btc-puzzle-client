#include "checkpoint_store.h"
#include "search_core.h"
#include <limits>
namespace btc {
namespace {
bool decode(const std::string &text, uint64_t &sequence, std::string &payload) {
  if (text.compare(0, 6, "BTCJ1\n") != 0)
    return false;
  size_t end = text.find('\n', 6);
  if (end == std::string::npos || end == 6)
    return false;
  uint64_t n = 0;
  for (size_t i = 6; i < end; ++i) {
    char c = text[i];
    if (c < '0' || c > '9' || n > (UINT64_MAX - unsigned(c - '0')) / 10)
      return false;
    n = n * 10 + unsigned(c - '0');
  }
  if (!n || text.size() < end + 66 || text[end + 65] != '\n')
    return false;
  std::string body = text.substr(end + 66);
  if (sha256Hex(text.substr(6, end - 6) + "\n" + body) != text.substr(end + 1, 64))
    return false;
  sequence = n;
  payload = std::move(body);
  return true;
}
} // namespace
bool Journal::load(std::string &payload) {
  std::string a, b, pa, pb;
  const bool existsA = files_.read(a_, a), existsB = files_.read(b_, b);
  hasFiles_ = existsA || existsB;
  uint64_t sa = 0, sb = 0;
  const bool validA = existsA && decode(a, sa, pa), validB = existsB && decode(b, sb, pb);
  initialized_ = true;
  sequence_ = 0;
  if (!validA && !validB)
    return false;
  if (validA && (!validB || sa >= sb)) {
    sequence_ = sa;
    payload = std::move(pa);
  } else {
    sequence_ = sb;
    payload = std::move(pb);
  }
  return true;
}
bool Journal::save(const std::string &payload) {
  if (!initialized_) {
    std::string old;
    load(old);
  }
  if (sequence_ == UINT64_MAX)
    return false;
  const uint64_t next = sequence_ + 1;
  const std::string seq = std::to_string(next);
  const std::string digest = sha256Hex(seq + "\n" + payload);
  if (digest.size() != 64)
    return false;
  const std::string data = "BTCJ1\n" + seq + "\n" + digest + "\n" + payload;
  const std::string &path = (next & 1) ? b_ : a_;
  if (!files_.write(path, data))
    return false;
  std::string verify, actual;
  uint64_t written = 0;
  if (!files_.read(path, verify) || !decode(verify, written, actual) || written != next ||
      actual != payload)
    return false;
  sequence_ = next;
  hasFiles_ = true;
  return true;
}
} // namespace btc
