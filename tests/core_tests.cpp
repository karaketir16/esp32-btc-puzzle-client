#include "search_core.h"
#include "checkpoint_store.h"
#include "found_records.h"
#include "run_stats.h"
#include "selftest_vectors.h"
#include "puzzle_metadata.h"
#include "public_solved_vectors.h"
#include <openssl/ec.h>
#include <openssl/obj_mac.h>
#include <cassert>
#include <cstring>
#include <iostream>
#include <map>
#include <random>
using namespace btc;
static std::mt19937_64 randomSource(0x425443);
static int rng(void *, unsigned char *p, size_t n) {
  while (n--)
    *p++ = randomSource();
  return 0;
}
struct MemoryFiles : FileStore {
  std::map<std::string, std::string> files;
  size_t cut = SIZE_MAX;
  bool fail = false;
  bool read(const std::string &p, std::string &s) override {
    if (!files.count(p))
      return false;
    s = files[p];
    return true;
  }
  bool write(const std::string &p, const std::string &s) override {
    if (fail)
      return false;
    files[p] = s.substr(0, cut);
    return cut >= s.size();
  }
};
static void testClock() {
  RunStats s;
  s.start(0);
  s.attempts = 100;
  s.stop(1000);
  s.start(2000);
  s.attempts += 100;
  s.stop(3000);
  assert(s.elapsedMs == 2000 && s.rate() == 100.0);
  s.start(0xfffffff0U);
  s.tick(0x10);
  s.stop(0x10);
  assert(s.elapsedMs == 2032);
  s.tick(6000);
  assert(s.elapsedMs == 2032);
  std::cout << "PASS statistics: stop/start, start at zero, rollover, paused time\n";
}
static void testJournal() {
  MemoryFiles fs;
  Journal j(fs, "/a", "/b");
  std::string out;
  assert(!j.load(out));
  assert(j.save("old"));
  auto baseline = fs.files;
  assert(j.save("new payload"));
  size_t n = fs.files["/a"].size();
  for (size_t cut = 0; cut < n; ++cut) {
    fs.files = baseline;
    fs.cut = cut;
    Journal w(fs, "/a", "/b");
    assert(w.load(out) && out == "old");
    assert(!w.save("new payload"));
    Journal reboot(fs, "/a", "/b");
    assert(reboot.load(out) && out == "old");
  }
  fs.files = baseline;
  fs.cut = SIZE_MAX;
  Journal w(fs, "/a", "/b");
  assert(w.load(out) && w.save("new"));
  Journal reboot(fs, "/a", "/b");
  assert(reboot.load(out) && out == "new");
  for (size_t i = 0; i < fs.files["/a"].size(); ++i) {
    auto saved = fs.files["/a"];
    fs.files["/a"][i] ^= 1;
    Journal damaged(fs, "/a", "/b");
    assert(damaged.load(out) && out == "old");
    fs.files["/a"] = saved;
  }
  std::cout << "PASS journal: every truncated-write byte and every corrupted-envelope byte recover "
               "prior checkpoint\n";
}
static void testFirstCheckpoint() {
  MemoryFiles fs;
  fs.cut = 10;
  Journal j(fs, "/a", "/b");
  assert(!j.save("first"));
  fs.cut = SIZE_MAX;
  Journal reboot(fs, "/a", "/b");
  std::string out;
  assert(!reboot.load(out));
  assert(reboot.save("recovered"));
  Journal again(fs, "/a", "/b");
  assert(again.load(out) && out == "recovered");
}
static void testFound() {
  MemoryFiles fs;
  Journal j(fs, "/fa", "/fb");
  FoundRecords f(j);
  assert(f.load());
  FoundRecord a;
  a.puzzle = 10;
  a.key[30] = 2;
  a.key[31] = 2;
  a.compressed = true;
  a.pending = true;
  a.attempts = 123;
  a.elapsedMs = 456;
  assert(f.put(a));
  FoundRecord b = a;
  b.puzzle = 4;
  b.key.fill(0);
  b.key[31] = 8;
  assert(f.put(b));
  Journal r(fs, "/fa", "/fb");
  FoundRecords restored(r);
  assert(restored.load());
  assert(restored.get(10) && restored.get(10)->key == a.key && restored.get(10)->pending);
  assert(restored.get(4));
  assert(restored.markDelivered(10));
  assert(restored.clear(4));
  Journal r2(fs, "/fa", "/fb");
  FoundRecords after(r2);
  assert(after.load() && after.get(10) && !after.get(10)->pending && !after.get(4));
  fs.fail = true;
  assert(!after.put(b) && after.dirty() && after.get(4));
  Journal failedReload(fs, "/fa", "/fb");
  FoundRecords beforeRetry(failedReload);
  assert(beforeRetry.load() && !beforeRetry.get(4) && beforeRetry.get(10));
  fs.fail = false;
  assert(after.flush() && !after.dirty());
  Journal retryReload(fs, "/fa", "/fb");
  FoundRecords afterRetry(retryReload);
  assert(afterRetry.load() && afterRetry.get(4) && afterRetry.get(10));
  fs.fail = true;
  assert(!after.clear(10));
  assert(after.get(10));
  fs.fail = false;
  assert(after.clear(0));
  Journal r3(fs, "/fa", "/fb");
  FoundRecords empty(r3);
  assert(empty.load() && empty.all().empty());
  std::cout << "PASS found records: multiple puzzles, reboot, pending delivery, clear one/all, "
               "failed clear\n";
}
static void testCrypto() {
  Crypto c;
  assert(c.begin(rng));
  EC_GROUP *g = EC_GROUP_new_by_curve_name(NID_secp256k1);
  BN_CTX *ctx = BN_CTX_new();
  EC_POINT *p = EC_POINT_new(g);
  size_t comparisons = 0;
  for (unsigned bits = 1; bits <= 160; ++bits) {
    for (int i = 0; i < 8; ++i) {
      uint8_t key[32];
      assert(randomKey(bits, key, rng));
      unsigned leading = 256 - bits;
      for (unsigned b = 0; b < leading; ++b)
        assert(!(key[b / 8] & (0x80 >> (b % 8))));
      assert(key[leading / 8] & (0x80 >> (leading % 8)));
      assert(c.derive(key));
      for (int step = 0; step < 8; ++step) {
        assert(c.increment());
        for (int k = 31; k >= 0; --k)
          if (++key[k])
            break;
        BIGNUM *n = BN_bin2bn(key, 32, nullptr);
        assert(EC_POINT_mul(g, p, n, nullptr, nullptr, ctx));
        uint8_t expected[65], actual[65];
        size_t len = 0;
        assert(c.serialize(false, actual, len));
        assert(EC_POINT_point2oct(g, p, POINT_CONVERSION_UNCOMPRESSED, expected, 65, ctx) == len);
        assert(!memcmp(expected, actual, len));
        BN_free(n);
        ++comparisons;
      }
    }
  }
  for (unsigned id = 1; id <= 160; ++id) {
    uint8_t hash[20];
    assert(decodeAddress(PUZZLE_ADDRESSES[id - 1], hash));
  }
  for (unsigned id = 1; id <= 10; ++id) {
    uint8_t hash[20], key[32] = {};
    assert(decodeAddress(PUZZLE_ADDRESSES[id - 1], hash));
    uint64_t n = SELFTEST_KEYS[id - 1];
    for (int i = 31; n; --i, n >>= 8)
      key[i] = n;
    bool compressed = false;
    assert(c.check(key, hash, compressed) == Check::Match && compressed);
  }
  for (const auto &v : publishedVectors) {
    uint8_t key[32], target[20];
    assert(unhex(v.key, key, 32));
    assert(decodeAddress(PUZZLE_ADDRESSES[v.id - 1], target));
    bool compressed = false;
    assert(c.check(key, target, compressed) == Check::Match);
  }
  uint8_t key[32] = {};
  key[31] = 1;
  assert(wif(key, true) == "KwDiBf89QgGbjEhKnhXJuH7LrciVrZi3qYjgd9M7rFU73sVHnoWn");
  assert(wif(key, false) == "5HpHagT65TZzG1PH3CSu63k8DbpvD8s5ip4nEB3kEsreAnchuDf");
  uint8_t hash[20];
  assert(decodeAddress("1EHNa6Q4Jz2uvNExL497mE43ikXhwF6kZm", hash));
  bool compressed = true;
  assert(c.check(key, hash, compressed) == Check::Match && !compressed);
  key[31] = 0;
  assert(c.check(key, hash, compressed) == Check::Error);
  EC_POINT_free(p);
  BN_CTX_free(ctx);
  EC_GROUP_free(g);
  std::cout << "PASS crypto: " << comparisons
            << " incremental points matched OpenSSL; ranges 1-160, known keys, "
               "compressed/uncompressed, WIF, error reporting\n";
}
static void testRandomSearch() {
  Crypto c;
  assert(c.begin(rng));
  uint64_t total = 0;
  for (unsigned id = 1; id <= 10; ++id) {
    uint8_t hash[20];
    assert(decodeAddress(PUZZLE_ADDRESSES[id - 1], hash));
    bool found = false;
    for (unsigned attempt = 0; attempt < 20000; ++attempt) {
      uint8_t key[32];
      assert(randomKey(id, key, rng));
      bool compressed = false;
      auto r = c.check(key, hash, compressed);
      assert(r != Check::Error);
      ++total;
      if (r == Check::Match) {
        uint64_t actual = 0;
        for (auto b : key)
          actual = (actual << 8) | b;
        assert(actual == SELFTEST_KEYS[id - 1]);
        found = true;
        break;
      }
    }
    assert(found);
  }
  std::cout << "PASS fresh-random searches: first 10 targets found in " << total << " attempts\n";
}
int main() {
  testClock();
  testJournal();
  testFirstCheckpoint();
  testFound();
  testCrypto();
  testRandomSearch();
  std::cout << "ALL TESTS PASSED\n";
}
