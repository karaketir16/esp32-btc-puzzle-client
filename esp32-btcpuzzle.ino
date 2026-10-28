#include <Arduino.h>
#ifndef SERIAL_ONLY
#define SERIAL_ONLY 0
#endif
#if !SERIAL_ONLY
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#if __has_include("secrets.h")
#include "secrets.h"
#else
#define WIFI_SSID "YOUR_WIFI_NAME"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#define TELEGRAM_BOT_TOKEN "PASTE_BOT_TOKEN_HERE"
#define TELEGRAM_OWNER_CHAT_ID "PASTE_NUMERIC_CHAT_ID_HERE"
#endif
#endif
#include <bootloader_random.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#if defined(ARDUINO)
#include <esp_partition.h>
#endif
#include "search_core.h"
#include "checkpoint_store.h"
#include "found_records.h"
#include "run_stats.h"
#include "selftest_vectors.h"
#include "puzzle_metadata.h"

static constexpr uint32_t CHECKPOINT_MS = 10000;
static constexpr uint32_t STATS_LOG_MS = 30000;
static constexpr uint32_t CHUNK_MS = 60000;
static constexpr uint16_t SELFTEST_CHUNK_KEYS = 50;
static constexpr uint32_t SELFTEST_CHUNK_MS = 2000;
static constexpr uint16_t MAX_PUZZLE_ID = 160;
struct Puzzle {
  uint16_t id = 0;
  String address;
  uint8_t target[20] = {};
  bool loaded = false;
};
class LittleFiles : public btc::FileStore {
public:
  bool read(const std::string &path, std::string &data) override {
    File f = LittleFS.open(path.c_str(), "r");
    if (!f || f.size() > 65536)
      return false;
    const size_t length = f.size();
    data.resize(length);
    const size_t got = length ? f.readBytes(&data[0], length) : 0;
    f.close();
    return got == length;
  }
  bool write(const std::string &path, const std::string &data) override {
    File f = LittleFS.open(path.c_str(), "w");
    if (!f)
      return false;
    const size_t written = f.write(reinterpret_cast<const uint8_t *>(data.data()), data.size());
    f.flush();
    f.close();
    return written == data.size(); // Journal also reopens and validates the full record.
  }
};
static LittleFiles files;
static btc::Journal runJournal(files, "/run-a.dat", "/run-b.dat");
static btc::Journal foundJournal(files, "/found-a.dat", "/found-b.dat");
static btc::FoundRecords foundRecords(foundJournal);
static btc::Crypto crypto;
static btc::RunStats runStats;
static Puzzle puzzle;
static bool searching = false;
static bool randomEveryKey = false; // Default: fresh random start every 60 seconds.
static bool ready = false;
static bool fsReady = false;
static bool foundReady = false;
static bool runCheckpointDirty = false;
static bool walkReady = false;
static bool stepValidated = false;
static uint8_t currentKey[32] = {};
static uint32_t chunkStartedMs = 0;
static uint32_t lastPersistMs = 0;
static uint32_t lastLogMs = 0;
static uint64_t lastLogTries = 0;
static uint32_t lastFoundRetryMs = 0;
struct SelfTestJob {
  bool active = false;
  bool all = false;
  bool walk = true;
  bool pointReady = false;
  uint8_t key[32] = {};
  uint16_t chunkCandidates = 0;
  uint32_t chunkStartedMs = 0;
  uint32_t chunks = 0;
  uint64_t walkSteps = 0;
  uint32_t pointChecks = 0;
  uint16_t id = 0;
  uint8_t target[20] = {};
  btc::RunStats stats;
  String summary;
};
static SelfTestJob selftest;
static String lastSelftestReport;
#if !SERIAL_ONLY
static int64_t telegramOffset = 0;
static uint32_t lastTelegramMs = 0;
static uint32_t lastWifiMs = 0;
static bool credentialsPresent = false;
static String pendingResumeMessage;
#endif

static String str(const std::string &s) { return String(s.c_str()); }
static void logLine(const String &s) { Serial.println(s); }
static int hardwareRng(void *, unsigned char *out, size_t length) {
  esp_fill_random(out, length);
  return 0;
}
static void keyLed(bool on) {
#if defined(RGB_BUILTIN)
  rgbLedWrite(RGB_BUILTIN, 0, on ? 24 : 0, 0);
#elif defined(LED_BUILTIN)
  digitalWrite(LED_BUILTIN, on ? LOW : HIGH);
#endif
}
#if !SERIAL_ONLY
static bool sendTelegram(const String &text) {
  if (!credentialsPresent || WiFi.status() != WL_CONNECTED)
    return false;
  // Certificate validity checks need a plausible UTC clock; never fall back to insecure TLS.
  if (time(nullptr) < 1700000000)
    return false;
  WiFiClientSecure client;
  client.useBuiltinCACertBundle();
  client.setHandshakeTimeout(15);
  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(client,
                  String("https://api.telegram.org/bot") + TELEGRAM_BOT_TOKEN + "/sendMessage"))
    return false;
  http.addHeader("Content-Type", "application/json");
  JsonDocument doc;
  doc["chat_id"] = TELEGRAM_OWNER_CHAT_ID;
  doc["text"] = text;
  String body;
  serializeJson(doc, body);
  const int code = http.POST(body);
  JsonDocument response;
  const bool ok =
      code == 200 && !deserializeJson(response, http.getString()) && (response["ok"] | false);
  if (!ok)
    logLine("Telegram send failed: HTTP " + String(code));
  http.end();
  return ok;
}
#endif
static void reply(const String &text) {
  logLine(text);
#if !SERIAL_ONLY
  // Found histories can exceed Telegram's per-message size limit.
  unsigned start = 0;
  while (start < text.length()) {
    unsigned end = start + 3500;
    if (end > text.length())
      end = text.length();
    sendTelegram(text.substring(start, end));
    start = end;
  }
#endif
}
static bool loadPuzzle(uint16_t id, Puzzle &p) {
  if (id < 1 || id > MAX_PUZZLE_ID)
    return false;
  p.id = id;
  p.address = PUZZLE_ADDRESSES[id - 1];
  p.loaded = btc::decodeAddress(p.address.c_str(), p.target);
  return p.loaded;
}
static String foundText(const btc::FoundRecord &r) {
  String out = "Stored result — puzzle " + String(r.puzzle);
  out += "\nAddress: " + String(PUZZLE_ADDRESSES[r.puzzle - 1]);
  out += "\nPrivate key (hex): " + str(btc::hex(r.key.data(), r.key.size()));
  out += "\nPrivate key (WIF): " + str(btc::wif(r.key.data(), r.compressed));
  out += "\nChecked: " + String((unsigned long long)r.attempts) + " candidates";
  out += "\nRun time: " + String((unsigned long long)(r.elapsedMs / 1000)) + " s";
  out += "\nTelegram delivery: " + String(r.pending ? "pending" : "not pending");
  return out;
}
static bool saveState(bool force = false) {
  const uint32_t now = millis();
  runStats.tick(now);
  if (force)
    runCheckpointDirty = true;
  if (!force && ((!searching || selftest.active) && !runCheckpointDirty))
    return true;
  if (!force && uint32_t(now - lastPersistMs) < CHECKPOINT_MS)
    return true;
  lastPersistMs = now;
  if (!fsReady)
    return false;
  JsonDocument doc;
  doc["schema"] = 2;
  doc["puzzle_id"] = puzzle.loaded ? puzzle.id : 0;
  doc["tries"] = (unsigned long long)runStats.attempts;
  doc["run_ms"] = (unsigned long long)runStats.elapsedMs;
  doc["searching"] = searching;
  doc["mode"] = randomEveryKey ? "random" : "chunk";
  std::string payload;
  runCheckpointDirty = true;
  if (!serializeJson(doc, payload) || !runJournal.save(payload)) {
    logLine("Checkpoint failed; previous valid checkpoint retained.");
    return false;
  }
  runCheckpointDirty = false;
  return true;
}
static bool restorePayload(const std::string &payload) {
  JsonDocument doc;
  if (deserializeJson(doc, payload) || !doc["puzzle_id"].is<unsigned>() ||
      !doc["tries"].is<uint64_t>() || !doc["run_ms"].is<uint64_t>())
    return false;
  const unsigned id = doc["puzzle_id"];
  if (id > MAX_PUZZLE_ID)
    return false;
  if (id && !loadPuzzle(id, puzzle))
    return false;
  runStats.attempts = doc["tries"].as<uint64_t>();
  runStats.elapsedMs = doc["run_ms"].as<uint64_t>();
  searching = id && (doc["searching"] | false);
  randomEveryKey = strcmp(doc["mode"] | "chunk", "random") == 0;
  return true;
}
static bool mountStorage() {
  if (LittleFS.begin(false))
    return true;
#if defined(ARDUINO)
  // First boot may need formatting. Never auto-format a nonblank partition:
  // a mount error must not erase the only copies of previously found keys.
  const esp_partition_t *part = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
  if (!part)
    return false;
  uint8_t block[512];
  for (size_t offset = 0; offset < part->size; offset += sizeof(block)) {
    const size_t length = part->size - offset < sizeof(block) ? part->size - offset : sizeof(block);
    if (esp_partition_read(part, offset, block, length) != ESP_OK)
      return false;
    for (size_t i = 0; i < length; ++i)
      if (block[i] != 0xff)
        return false;
    if ((offset % 32768) == 0)
      yield();
  }
  return LittleFS.begin(true); // Confirmed erased factory partition only.
#else
  return false;
#endif
}
static void loadState() {
  fsReady = mountStorage();
  if (!fsReady) {
    logLine("LittleFS mount failed; searches are disabled until storage is available.");
    return;
  }
  foundReady = foundRecords.load();
  if (!foundReady)
    logLine("Found-record journal is damaged; do not erase flash. Recovery required.");
  std::string payload;
  if (runJournal.load(payload)) {
    if (!restorePayload(payload))
      logLine("Invalid run payload; run remains stopped.");
  } else if (!runJournal.hasFiles()) {
    // Migrate old firmware checkpoints; recover the old temporary file if necessary.
    bool restored = files.read("/state.json", payload) && restorePayload(payload);
    if (!restored)
      restored = files.read("/state.tmp", payload) && restorePayload(payload);
    if (restored) {
      logLine("Migrated legacy run checkpoint.");
      saveState(true);
    }
  } else
    logLine("No valid run checkpoint; run remains stopped.");
  runStats.lastTick = millis();
  runStats.running = searching;
  // If power failed after saving a result but before saving the stopped flag, prefer the result.
  if (puzzle.loaded && foundRecords.get(puzzle.id)) {
    searching = false;
    runStats.stop(millis());
  }
}
static String formatStats() {
  runStats.tick(millis());
  String out;
  if (selftest.active)
    out += "SELFTEST active: puzzle " + String(selftest.id) +
           "\nAttempts: " + String((unsigned long long)selftest.stats.attempts) + "\nTest mode: " +
           String(selftest.walk ? "walk: 50 keys / 2 s per chunk" : "independent random") +
           "\nWalk steps: " + String((unsigned long long)selftest.walkSteps) +
           "\nReference checks: " + String(selftest.pointChecks) + "\n\n";
  if (puzzle.loaded) {
    out += "Puzzle: " + String(puzzle.id) + "\nTarget: " + puzzle.address;
    out += "\nState: " + String(selftest.active ? "paused for selftest"
                                : searching     ? "searching"
                                                : "stopped");
    out += "\nCandidates checked: " + String((unsigned long long)runStats.attempts);
    out += "\nAverage rate: " + String(runStats.rate(), 2) + " keys/s";
    out += "\nRun time: " + String((unsigned long long)(runStats.elapsedMs / 1000)) + " s";
    out += "\nSearch method: " + String(randomEveryKey
                                            ? "independent random key every attempt"
                                            : "sequential chunks, fresh random start every 60 s");
    if (const auto *r = foundRecords.get(puzzle.id))
      out += "\n\n" + foundText(*r);
  } else
    out += "No puzzle selected. Use /puzzle <ID>.";
  out += "\nPublic status: https://btcpuzzle.info/puzzle";
  out += "\nStored puzzle IDs:";
  if (foundRecords.all().empty())
    out += " none";
  for (const auto &r : foundRecords.all())
    out += " " + String(r.puzzle);
  if (!fsReady)
    out += "\nWARNING: LittleFS unavailable; existing flash was not reformatted.";
  else if (!foundReady)
    out += "\nWARNING: found-key journal is damaged; preserve flash for recovery.";
  if (foundRecords.dirty())
    out += "\nWARNING: found record is in RAM; flash save will be retried.";
  if (!selftest.active && !lastSelftestReport.isEmpty())
    out += "\n\n" + lastSelftestReport;
  return out;
}
static void resetSpeedWindow() {
  lastLogMs = millis();
  lastLogTries = runStats.attempts;
}
static void endSelftest(const String &message, bool resume = true) {
  selftest.stats.stop(millis());
  selftest.active = false;
  lastSelftestReport = selftest.summary + message;
  walkReady = false;
  if (resume && searching)
    runStats.start(millis());
  resetSpeedWindow();
  reply(lastSelftestReport + ((resume && searching) ? "\nNormal search resumed." : ""));
}
static bool validatePoint(const uint8_t key[32]) {
  btc::Crypto reference;
  if (!reference.begin(hardwareRng) || !reference.derive(key))
    return false;
  uint8_t a[65], b[65];
  size_t na = 0, nb = 0;
  return crypto.serialize(false, a, na) && reference.serialize(false, b, nb) && na == nb &&
         !memcmp(a, b, na);
}
static void failSelftest(const String &reason) {
  searching = false;
  endSelftest("SELFTEST FAILED: " + reason, false);
  saveState(true);
}
static bool verifySelftestPoint() {
  if (!validatePoint(selftest.key)) {
    failSelftest("walk point disagrees with fresh scalar multiplication.");
    return false;
  }
  ++selftest.pointChecks;
  return true;
}
static bool startTestPuzzle(uint16_t id) {
  if (id < 1 || id > 10)
    return false;
  selftest.id = id;
  selftest.pointReady = false;
  selftest.chunkCandidates = 0;
  selftest.chunks = 0;
  selftest.walkSteps = 0;
  selftest.pointChecks = 0;
  selftest.stats.reset(millis());
  selftest.stats.start(millis());
  return btc::decodeAddress(PUZZLE_ADDRESSES[id - 1], selftest.target);
}
static void selftestOneCandidate() {
  uint8_t key[32];
  bool compressed = true;
  // Check the time bound between operations (including after blocked Telegram I/O).
  if (selftest.walk && selftest.pointReady &&
      uint32_t(millis() - selftest.chunkStartedMs) >= SELFTEST_CHUNK_MS) {
    if (!verifySelftestPoint())
      return;
    selftest.pointReady = false;
  }
  keyLed(true);
  btc::Check result = btc::Check::Error;
  if (selftest.walk) {
    if (!selftest.pointReady) {
      selftest.chunkStartedMs = millis();
      selftest.chunkCandidates = 0;
      selftest.pointReady =
          btc::randomKey(selftest.id, selftest.key, hardwareRng) && crypto.derive(selftest.key);
      if (selftest.pointReady)
        ++selftest.chunks;
    }
    if (selftest.pointReady) {
      memcpy(key, selftest.key, 32);
      result = crypto.checkCurrent(selftest.target, compressed);
    }
  } else if (btc::randomKey(selftest.id, key, hardwareRng)) {
    result = crypto.check(key, selftest.target, compressed);
  }
  keyLed(false);
  selftest.stats.tick(millis());
  if (result == btc::Check::Error) {
    failSelftest("cryptographic operation failed.");
    return;
  }
  ++selftest.stats.attempts;
  if (selftest.walk)
    ++selftest.chunkCandidates;
  if (result != btc::Check::Match) {
    if (selftest.walk) {
      const bool boundary = selftest.chunkCandidates >= SELFTEST_CHUNK_KEYS ||
                            uint32_t(millis() - selftest.chunkStartedMs) >= SELFTEST_CHUNK_MS ||
                            btc::maximumKey(selftest.id, selftest.key);
      if (boundary) {
        if (!verifySelftestPoint())
          return;
        selftest.pointReady = false;
      } else {
        if (!crypto.increment()) {
          failSelftest("incremental arithmetic failed.");
          return;
        }
        for (int i = 31; i >= 0; --i)
          if (++selftest.key[i])
            break;
        ++selftest.walkSteps;
        // Check the first increment and the end of each chunk, plus every target match.
        if (selftest.chunkCandidates == 1 && !verifySelftestPoint())
          return;
      }
    }
    selftest.stats.tick(millis());
    return;
  }
  if (selftest.walk && !verifySelftestPoint())
    return;
  selftest.stats.tick(millis());
  uint8_t expected[32] = {};
  uint64_t known = SELFTEST_KEYS[selftest.id - 1];
  for (int i = 31; known; --i, known >>= 8)
    expected[i] = known;
  if (memcmp(expected, key, 32) != 0 || !compressed) {
    failSelftest("unexpected discovered key/encoding.");
    return;
  }
  // A self-test genuinely discovers the key through the same randomized search.
  // Keep that result independently, without replacing an earlier discovery record.
  if (!foundRecords.get(selftest.id)) {
    btc::FoundRecord record;
    record.puzzle = selftest.id;
    memcpy(record.key.data(), key, 32);
    record.compressed = compressed;
    record.pending = !SERIAL_ONLY;
    record.attempts = selftest.stats.attempts;
    record.elapsedMs = selftest.stats.elapsedMs;
    if (!foundReady || !foundRecords.put(record)) {
      searching = false;
      endSelftest("SELFTEST found key " + str(btc::hex(key, 32)) +
                      "; flash save failed. Result retained in RAM; keep power connected.",
                  false);
      saveState(true);
      return;
    }
  }
  const String resultText =
      "SELFTEST PASS puzzle " + String(selftest.id) + ": discovered " + str(btc::hex(key, 32)) +
      " after " + String((unsigned long long)selftest.stats.attempts) +
      (selftest.walk ? " walk attempts (" : " independent random attempts (") +
      String(selftest.stats.rate(), 2) + " keys/s)." +
      (selftest.walk ? " Walk steps: " + String((unsigned long long)selftest.walkSteps) +
                           "; reference checks: " + String(selftest.pointChecks) +
                           "; chunks: " + String(selftest.chunks) + "."
                     : "");
  selftest.summary += resultText + "\n";
  if (selftest.all && selftest.id < 10) {
    reply(resultText);
    if (!startTestPuzzle(selftest.id + 1))
      endSelftest("SELFTEST FAILED: invalid target.");
  } else
    endSelftest("SELFTEST completed.");
}
static unsigned parseId(const String &value, unsigned maximum = MAX_PUZZLE_ID) {
  if (value.isEmpty())
    return 0;
  unsigned id = 0;
  for (unsigned i = 0; i < value.length(); ++i) {
    if (value[i] < '0' || value[i] > '9')
      return 0;
    id = id * 10 + unsigned(value[i] - '0');
    if (id > maximum)
      return 0;
  }
  return id;
}
static void handleCommand(const String &text) {
  String command = text;
  command.trim();
  if (!command.startsWith("/"))
    command = "/" + command;
  const int space = command.indexOf(' ');
  String verb = space < 0 ? command : command.substring(0, space);
  String arg = space < 0 ? "" : command.substring(space + 1);
  arg.trim();
  int at = verb.indexOf('@');
  if (at >= 0)
    verb = verb.substring(0, at);
  verb.toLowerCase();
  if (verb == "/help" || verb == "/start") {
    reply("ESP32 BTC puzzle client\n"
          "/puzzle <1-160> - select embedded target; show any stored result\n"
          "/run, /stop, /status\n"
          "/mode chunk|random - 60-second random restarts or independent random keys\n"
          "/found [ID] - list saved keys\n"
          "/clear-found [ID|all] - delete saved keys (default all)\n"
          "/selftest [1-10|all] [walk|random] - default: all, walk; 50 keys / 2 s per chunk\n"
          "Selftests pause/resume the normal run. No pool is used.");
  } else if (verb == "/status")
    reply(formatStats());
  else if (verb == "/found") {
    const unsigned id = parseId(arg);
    if (!arg.isEmpty() && !id) {
      reply("Use /found [1-160].");
      return;
    }
    bool any = false;
    for (const auto &r : foundRecords.all())
      if (!id || id == r.puzzle) {
        reply(foundText(r));
        any = true;
      }
    if (!any)
      reply("No stored result for that selection.");
  } else if (verb == "/clear-found") {
    const unsigned id = parseId(arg);
    if (!arg.isEmpty() && arg != "all" && !id) {
      reply("Use /clear-found [1-160|all].");
      return;
    }
    if (!foundReady && id) {
      reply("Found journal is damaged. Use /clear-found all only if you want to discard it.");
      return;
    }
    if (!fsReady || !foundRecords.clear(id))
      reply("Could not save deletion. Found records retained.");
    else {
      foundReady = true;
      reply(id ? "Cleared stored result for puzzle " + String(id) + "."
               : "Cleared all stored results.");
    }
  } else if (verb == "/puzzle") {
    const unsigned id = parseId(arg);
    if (searching || selftest.active) {
      reply("Use /stop before changing puzzle.");
      return;
    }
    if (!id || !loadPuzzle(id, puzzle)) {
      reply("Puzzle ID must be 1 through 160.");
      return;
    }
    walkReady = false;
    runStats.reset(millis());
    saveState(true);
    reply("Selected puzzle " + String(id) + "\nTarget: " + puzzle.address + "\nRange: [2^" +
          String(id - 1) + ", 2^" + String(id) +
          "-1]\nPublic status: https://btcpuzzle.info/puzzle");
    if (const auto *r = foundRecords.get(id))
      reply(foundText(*r));
  } else if (verb == "/mode") {
    if (searching || selftest.active) {
      reply("Use /stop before changing mode.");
      return;
    }
    if (arg != "chunk" && arg != "random") {
      reply("Use /mode chunk or /mode random.");
      return;
    }
    randomEveryKey = arg == "random";
    walkReady = false;
    saveState(true);
    reply(randomEveryKey ? "Mode: independent random key every attempt."
                         : "Mode: new random start every 60 seconds; increment within each chunk.");
  } else if (verb == "/run") {
    if (selftest.active) {
      reply("Selftest is running. Use /stop to cancel.");
      return;
    }
    if (!puzzle.loaded) {
      reply("Use /puzzle <ID> first.");
      return;
    }
    if (!fsReady || !foundReady || foundRecords.dirty()) {
      reply("Found-key storage is unavailable or awaiting a retry. Resolve storage before "
            "searching.");
      return;
    }
    if (const auto *r = foundRecords.get(puzzle.id)) {
      reply(foundText(*r));
      return;
    }
    if (!searching) {
      searching = true;
      runStats.start(millis());
      walkReady = false;
      resetSpeedWindow();
      saveState(true);
    }
    reply("Search running.\n" + formatStats());
  } else if (verb == "/stop") {
    if (selftest.active)
      endSelftest("SELFTEST cancelled.", false);
    runStats.stop(millis());
    searching = false;
    saveState(true);
    reply("Search stopped.\n" + formatStats());
  } else if (verb == "/selftest") {
    if (!fsReady || !foundReady || foundRecords.dirty()) {
      reply("Resolve found-key storage before starting a selftest.");
      return;
    }
    if (selftest.active) {
      reply("A selftest is already running. Use /stop to cancel.");
      return;
    }
    String selection = arg;
    String testMode = "walk";
    const int split = selection.indexOf(' ');
    if (split >= 0) {
      testMode = selection.substring(split + 1);
      selection = selection.substring(0, split);
      testMode.trim();
    } else if (selection == "walk" || selection == "random") {
      testMode = selection;
      selection = "all";
    }
    const unsigned id = parseId(selection, 10);
    if ((!selection.isEmpty() && selection != "all" && !id) ||
        (testMode != "walk" && testMode != "random")) {
      reply("Use /selftest [1-10|all] [walk|random].");
      return;
    }
    runStats.stop(millis());
    saveState(true); // Preserve the normal resume flag; the diagnostic job itself is not persisted.
    selftest.active = true;
    selftest.all = !id;
    selftest.walk = testMode == "walk";
    selftest.summary = "";
    if (!startTestPuzzle(id ? id : 1)) {
      endSelftest("SELFTEST FAILED: invalid target.");
      return;
    }
    reply("SELFTEST started: " + String(selftest.all ? "puzzles 1-10" : "puzzle " + String(id)) +
          (selftest.walk ? ". Walk: random chunks of up to 50 keys or 2 seconds; first/end/matched "
                           "points checked against fresh multiplication."
                         : ". Independent random key each attempt; repeats are allowed.") +
          " Known results only verify a discovered match. Use /stop to cancel.");
  } else
    reply("Unknown command. Use /help.");
}
static void pollSerial() {
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (!line.isEmpty()) {
        logLine("> " + line);
        handleCommand(line);
        line = "";
      }
    } else if (line.length() < 160)
      line += c;
  }
}
#if !SERIAL_ONLY
static void pollTelegram() {
  if (!credentialsPresent || WiFi.status() != WL_CONNECTED || time(nullptr) < 1700000000)
    return;
  WiFiClientSecure client;
  client.useBuiltinCACertBundle();
  client.setHandshakeTimeout(15);
  HTTPClient http;
  http.setTimeout(10000);
  String url = String("https://api.telegram.org/bot") + TELEGRAM_BOT_TOKEN +
               "/getUpdates?offset=" + String((long long)telegramOffset) + "&timeout=0&limit=5";
  if (!http.begin(client, url))
    return;
  const int code = http.GET();
  JsonDocument doc;
  const bool ok = code == 200 && !deserializeJson(doc, http.getString()) && (doc["ok"] | false);
  http.end();
  if (!ok) {
    logLine("Telegram poll failed: HTTP " + String(code));
    return;
  }
  for (JsonObject update : doc["result"].as<JsonArray>()) {
    int64_t id = update["update_id"] | -1;
    if (id >= telegramOffset)
      telegramOffset = id + 1;
    JsonObject message = update["message"];
    if (message.isNull() ||
        (message["chat"]["id"] | 0LL) != strtoll(TELEGRAM_OWNER_CHAT_ID, nullptr, 10))
      continue;
    const char *text = message["text"] | "";
    if (text[0] == '/')
      handleCommand(String(text));
  }
}
#endif
static void retryFound() {
  if (!fsReady || !foundReady || !foundRecords.flush())
    return;
#if !SERIAL_ONLY
  for (const auto &r : foundRecords.all())
    if (r.pending) {
      const uint16_t id = r.puzzle;
      if (sendTelegram("TARGET KEY FOUND\n" + foundText(r))) {
        if (!foundRecords.markDelivered(id))
          logLine("Delivery succeeded; acknowledgment save will be retried.");
      }
      break; // Keep serial/search responsive when offline or many results are pending.
    }
#endif
}
static void reportFound(bool compressed) {
  runStats.stop(millis());
  searching = false;
  walkReady = false;
  btc::FoundRecord r;
  r.puzzle = puzzle.id;
  memcpy(r.key.data(), currentKey, 32);
  r.compressed = compressed;
  r.pending = !SERIAL_ONLY;
  r.attempts = runStats.attempts;
  r.elapsedMs = runStats.elapsedMs;
  const bool saved = fsReady && foundRecords.put(r);
  saveState(true);
  logLine("TARGET KEY FOUND\n" + foundText(r));
  if (!saved)
    logLine("WARNING: result save failed; retained in RAM and will retry. Keep power connected.");
  retryFound();
  memset(currentKey, 0, 32);
}

static void searchOneCandidate() {
  if (!searching || !puzzle.loaded)
    return;
  bool compressed = true;
  keyLed(true);
  btc::Check result = btc::Check::Error;
  if (randomEveryKey) {
    if (btc::randomKey(puzzle.id, currentKey, hardwareRng))
      result = crypto.check(currentKey, puzzle.target, compressed);
  } else {
    if (!walkReady || uint32_t(millis() - chunkStartedMs) >= CHUNK_MS) {
      walkReady = btc::randomKey(puzzle.id, currentKey, hardwareRng) && crypto.derive(currentKey);
      chunkStartedMs = millis();
      stepValidated = false;
    }
    if (walkReady)
      result = crypto.checkCurrent(puzzle.target, compressed);
  }
  if (result == btc::Check::Error) {
    keyLed(false);
    runStats.stop(millis());
    searching = false;
    saveState(true);
    reply("Search stopped: cryptographic operation failed. Attempt was not counted.");
    return;
  }
  ++runStats.attempts;
  if (result == btc::Check::Match) {
    keyLed(false);
    reportFound(compressed);
    return;
  }
  if (!randomEveryKey) {
    if (btc::maximumKey(puzzle.id, currentKey))
      walkReady = false;
    else {
      bool advanced = crypto.increment();
      for (int i = 31; i >= 0; --i)
        if (++currentKey[i])
          break;
      if (advanced && !stepValidated) {
        advanced = validatePoint(currentKey);
        stepValidated = advanced;
      }
      if (!advanced) {
        keyLed(false);
        walkReady = false;
        runStats.stop(millis());
        searching = false;
        saveState(true);
        reply("Search stopped: incremental point validation/arithmetic failed.");
        return;
      }
    }
  }
  keyLed(false);
  runStats.tick(millis());
}
void setup() {
  Serial.begin(115200);
  delay(300);
  logLine("ESP32 BTC puzzle client starting. No pool is used.");
#if defined(LED_BUILTIN) && !defined(RGB_BUILTIN)
  pinMode(LED_BUILTIN, OUTPUT);
#endif
  keyLed(false);
#if SERIAL_ONLY
  bootloader_random_enable(); // Supply fresh hardware entropy while radio is disabled; no ADC is
                              // used.
  logLine("SERIAL_ONLY=1: Wi-Fi/Telegram disabled. Hardware RNG entropy enabled.");
#else
  credentialsPresent = String(WIFI_SSID) != "YOUR_WIFI_NAME" && !String(WIFI_SSID).isEmpty() &&
                       String(TELEGRAM_BOT_TOKEN).length() >= 20 &&
                       String(TELEGRAM_BOT_TOKEN) != "PASTE_BOT_TOKEN_HERE" &&
                       !String(TELEGRAM_OWNER_CHAT_ID).isEmpty() &&
                       String(TELEGRAM_OWNER_CHAT_ID) != "PASTE_NUMERIC_CHAT_ID_HERE";
  if (credentialsPresent) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
    logLine("Connecting Wi-Fi; Telegram uses authenticated TLS after time synchronization.");
  } else {
    bootloader_random_enable();
    logLine(
        "Configure secrets.h for Telegram; serial control and hardware entropy remain available.");
  }
#endif
  if (!crypto.begin(hardwareRng)) {
    logLine("secp256k1 initialization failed.");
    return;
  }
  loadState();
  if (!fsReady || !foundReady) {
    searching = false;
    runStats.stop(millis());
  }
  resetSpeedWindow();
  ready = true;
  if (searching) {
    const String resumed = "Search resumed after reboot.\nPuzzle: " + String(puzzle.id) +
                           "\nAttempts: " + String((unsigned long long)runStats.attempts);
    logLine(resumed);
#if !SERIAL_ONLY
    pendingResumeMessage = resumed;
#endif
  }
  reply("ESP32 BTC puzzle client online. Use /help. Serial: 115200 baud.");
}
void loop() {
  if (!ready) {
    delay(100);
    return;
  }
  runStats.tick(millis());
  pollSerial();
  const uint32_t now = millis();
#if !SERIAL_ONLY
  if (credentialsPresent && WiFi.status() != WL_CONNECTED && uint32_t(now - lastWifiMs) >= 10000) {
    lastWifiMs = now;
    WiFi.reconnect();
  }
  if (uint32_t(now - lastTelegramMs) >= 10000) {
    lastTelegramMs = now;
    if (!pendingResumeMessage.isEmpty() && sendTelegram(pendingResumeMessage))
      pendingResumeMessage = "";
    pollTelegram();
  }
#endif
  if (uint32_t(now - lastFoundRetryMs) >= 10000) {
    lastFoundRetryMs = now;
    retryFound();
  }
  if (selftest.active)
    selftestOneCandidate();
  else if (searching)
    searchOneCandidate();
  else
    delay(5);
  saveState();
  if (uint32_t(millis() - lastLogMs) >= STATS_LOG_MS) {
    if (selftest.active)
      logLine("SELFTEST puzzle " + String(selftest.id) + ": " +
              String((unsigned long long)selftest.stats.attempts) +
              (selftest.walk ? " walk attempts." : " independent random attempts."));
    else if (searching) {
      const uint32_t elapsed = millis() - lastLogMs;
      const double rate = elapsed ? double(runStats.attempts - lastLogTries) * 1000.0 / elapsed : 0;
      logLine("Puzzle " + String(puzzle.id) + ": " + String((unsigned long long)runStats.attempts) +
              " attempts, " + String(rate, 2) + " keys/s");
    }
    resetSpeedWindow();
  }
}
