#include "fakes/Arduino.h"
#include "fakes/LittleFS.h"
#include <cassert>
#include <random>
#include <iostream>
#include <new>
uint32_t fakeMillis = 0;
FakeSerial Serial;
FakeLittleFS LittleFS;
static std::mt19937_64 randomSource(0x434c49454e54);
void esp_fill_random(void *p, size_t n) {
  auto b = static_cast<uint8_t *>(p);
  while (n--)
    *b++ = randomSource();
  ++fakeMillis;
}
#define SERIAL_ONLY 1
#include "../esp32-btcpuzzle.ino"
static bool outputContains(const std::string &s) {
  for (const auto &line : Serial.lines)
    if (line.find(s) != String::npos)
      return true;
  return false;
}
static void rebootState() {
  foundRecords.~FoundRecords();
  runJournal.~Journal();
  foundJournal.~Journal();
  new (&runJournal) btc::Journal(files, "/run-a.dat", "/run-b.dat");
  new (&foundJournal) btc::Journal(files, "/found-a.dat", "/found-b.dat");
  new (&foundRecords) btc::FoundRecords(foundJournal);
  puzzle = Puzzle{};
  searching = false;
  runStats = btc::RunStats{};
  walkReady = false;
  loadState();
}
int main() {
  setup();
  assert(ready && fsReady);
  handleCommand("puzzle 10");
  handleCommand("/mode random");
  handleCommand("run");
  for (int i = 0; searching && i < 20000; ++i)
    searchOneCandidate();
  assert(!searching && foundRecords.get(10));
  auto known = foundRecords.get(10)->key;
  assert(known[30] == 2 && known[31] == 2);
  Serial.lines.clear();
  handleCommand("status");
  assert(outputContains("Stored result") && outputContains(btc::hex(known.data(), 32)));
  handleCommand("/puzzle 4");
  assert(foundRecords.get(10));
  Serial.lines.clear();
  handleCommand("/puzzle 10");
  assert(outputContains(btc::hex(known.data(), 32)));
  handleCommand("/run");
  assert(!searching);
  rebootState();
  assert(foundRecords.get(10) && foundRecords.get(10)->key == known && !searching);
  handleCommand("/clear-found 10");
  rebootState();
  assert(!foundRecords.get(10));
  handleCommand("/puzzle 72");
  handleCommand("/mode chunk");
  handleCommand("/run");
  searchOneCandidate();
  uint8_t before[32];
  memcpy(before, currentKey, 32);
  searchOneCandidate();
  for (int i = 31; i >= 0; --i)
    if (++before[i])
      break;
  assert(!memcmp(before, currentKey, 32));
  fakeMillis += 60000;
  searchOneCandidate();
  assert(memcmp(before, currentKey, 32));
  const auto attempts = runStats.attempts;
  const auto ms = runStats.elapsedMs;
  handleCommand("/selftest 4 walk");
  assert(selftest.active && searching && !runStats.running);
  for (int i = 0; selftest.active && i < 20000; ++i)
    selftestOneCandidate();
  assert(!selftest.active && searching && runStats.running && puzzle.id == 72 &&
         runStats.attempts == attempts && runStats.elapsedMs == ms && foundRecords.get(4));
  assert(lastSelftestReport.find("SELFTEST PASS puzzle 4") != String::npos);
  handleCommand("/selftest all");
  for (int i = 0; selftest.active && i < 50000; ++i)
    selftestOneCandidate();
  assert(!selftest.active && searching && puzzle.id == 72 && runStats.attempts == attempts);
  for (int id = 1; id <= 10; ++id)
    assert(lastSelftestReport.find("SELFTEST PASS puzzle " + std::to_string(id) + ":") !=
           String::npos);
  handleCommand("/selftest 4 random");
  assert(selftest.active && !selftest.walk);
  for (int i = 0; selftest.active && i < 20000; ++i)
    selftestOneCandidate();
  assert(!selftest.active && searching && selftest.walkSteps == 0 && selftest.pointChecks == 0);
  assert(lastSelftestReport.find("independent random attempts") != String::npos);
  handleCommand("/selftest 10");
  assert(selftest.walk); // Default now exercises the walk, independent of normal /mode.
  handleCommand("/stop");
  assert(!selftest.active && !searching && !runStats.running);
  handleCommand("/run");
  runStats.tick(millis());
  saveState(true);
  uint64_t count = runStats.attempts, elapsed = runStats.elapsedMs;
  rebootState();
  assert(searching && puzzle.id == 72 && runStats.attempts == count &&
         runStats.elapsedMs == elapsed);
  handleCommand("/stop");
  LittleFS.files.clear();
  LittleFS.files["/state.tmp"] = std::make_shared<std::string>(
      "{\"puzzle_id\":72,\"tries\":12345,\"run_ms\":9999,\"searching\":true}");
  rebootState();
  assert(searching && puzzle.id == 72 && runStats.attempts == 12345 && runStats.elapsedMs == 9999);
  LittleFS.mountOk = false;
  LittleFS.formatted = false;
  auto kept = LittleFS.files.size();
  rebootState();
  assert(!fsReady && !LittleFS.formatted && LittleFS.files.size() == kept);
  LittleFS.mountOk = true;
  rebootState();
  assert(fsReady && searching && puzzle.id == 72);
  handleCommand("/selftest 4");
  selftest.id = 0; // Fault injection: invalid internal range must halt both jobs.
  selftestOneCandidate();
  assert(!selftest.active && !searching && !runStats.running);
  handleCommand("/run");
  const auto normalAttempts = runStats.attempts;
  handleCommand("/selftest 10 walk");
  assert(selftest.active && selftest.walk);
  // Force a no-match target solely in the host fixture to exercise both restart limits.
  memset(selftest.target, 0, 20);
  memset(selftest.key, 0, 32);
  selftest.key[30] = 2; // scalar 512, well below the 10-bit maximum
  assert(crypto.derive(selftest.key));
  selftest.pointReady = true;
  selftest.chunkStartedMs = millis();
  selftest.chunkCandidates = 0;
  selftest.chunks = 1;
  for (unsigned i = 0; i < 50; ++i)
    selftestOneCandidate();
  assert(selftest.active && !selftest.pointReady && selftest.stats.attempts == 50);
  assert(selftest.walkSteps == 49 && selftest.pointChecks == 2 && selftest.chunks == 1);
  selftestOneCandidate();
  assert(selftest.chunks == 2 && selftest.stats.attempts == 51);
  // Expired time bound forces another seed even before fifty candidates are checked.
  memset(selftest.key, 0, 32);
  selftest.key[30] = 2;
  assert(crypto.derive(selftest.key));
  selftest.pointReady = true;
  selftest.chunkStartedMs = millis() - 2000;
  const auto chunksBeforeTimeout = selftest.chunks;
  selftestOneCandidate();
  assert(selftest.chunks == chunksBeforeTimeout + 1);
  assert(runStats.attempts == normalAttempts);
  // A small-range upper boundary ends the chunk without stepping outside it.
  memset(selftest.key, 0, 32);
  selftest.key[30] = 3;
  selftest.key[31] = 255; // scalar 1023, the 10-bit maximum
  assert(crypto.derive(selftest.key));
  selftest.pointReady = true;
  selftest.chunkCandidates = 0;
  selftest.chunkStartedMs = millis();
  const auto beforeMaximumSteps = selftest.walkSteps;
  selftestOneCandidate();
  assert(selftest.active && !selftest.pointReady && selftest.walkSteps == beforeMaximumSteps);
  // Deliberately desynchronize key/point at a chunk edge; comparison must fail closed.
  memset(selftest.key, 0, 32);
  selftest.key[30] = 2;
  uint8_t wrongKey[32] = {};
  wrongKey[30] = 2;
  wrongKey[31] = 1;
  assert(crypto.derive(wrongKey));
  selftest.pointReady = true;
  selftest.chunkStartedMs = millis();
  selftest.chunkCandidates = 49;
  selftestOneCandidate();
  assert(!selftest.active && !searching && !runStats.running);
  assert(lastSelftestReport.find("walk point disagrees") != String::npos);
  std::cout << "PASS command integration: saved key/status/reselect/reboot/clear; minute random "
               "restarts; walk/random selftests, 50-key/time caps, deliberate point-corruption "
               "detection; pause/resume/cancel; active reboot; "
               "legacy temporary-checkpoint migration\n";
}
