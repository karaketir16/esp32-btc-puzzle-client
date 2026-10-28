#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <mbedtls/ecp.h>
#include <mbedtls/sha256.h>
#include "ripemd160.h"
#include "secrets.h"

// Standalone BTC Puzzle client: reads public puzzle metadata only, never asks
// the pool for work and never submits ranges or progress.
static constexpr uint16_t MAX_PUZZLE_ID = 160;
static constexpr uint16_t EMBEDDED_PUZZLE_MAX = 10;
static constexpr uint32_t TELEGRAM_POLL_MS = 10000;
static constexpr uint32_t STATS_LOG_MS = 30000;
static constexpr uint32_t KEY_CHECK_BLINK_MS = 25;

struct Puzzle {
  uint16_t id = 0;
  String startHex;
  String endHex;
  String address;
  bool completed = false;
  bool loaded = false;
};

static Puzzle puzzle;
static bool searching = false;
static uint64_t tries = 0;
static uint64_t runStartTries = 0;
static uint64_t accumulatedRunMs = 0;
static uint64_t activeRunStartMs = 0;
static uint16_t savedPuzzleId = 0;
static uint32_t lastPersistMs = 0;
static uint64_t lastPersistTries = 0;
static bool littleFsReady = false;
static uint64_t lastStatsTries = 0;
static uint32_t lastStatsMs = 0;
static uint32_t lastTelegramPollMs = 0;
static int64_t telegramOffset = 0;
static bool appReady = false;
static bool credentialsPresent = false;
static bool cryptoReady = false;
static uint32_t lastWifiReconnectMs = 0;
static String pendingFoundMessage;
static uint32_t lastFoundRetryMs = 0;
static mbedtls_ecp_group secpGroup;
static mbedtls_ecp_point publicPoint;
static mbedtls_mpi privateScalar;

static constexpr const char *STATE_PATH = "/state.json";
static constexpr uint32_t STATE_PERSIST_MS = 10000;

static void saveState(bool force = false) {
  const uint32_t now = millis();
  if (!force && now - lastPersistMs < STATE_PERSIST_MS && tries - lastPersistTries < 1000) return;
  if (!littleFsReady) return;
  File file = LittleFS.open("/state.tmp", "w");
  if (!file) return;
  JsonDocument doc;
  doc["puzzle_id"] = puzzle.loaded ? puzzle.id : savedPuzzleId;
  doc["tries"] = (unsigned long long)tries;
  doc["run_ms"] = (unsigned long long)(accumulatedRunMs + (searching ? now - activeRunStartMs : 0));
  doc["run_tries"] = (unsigned long long)runStartTries;
  doc["searching"] = searching;
  serializeJson(doc, file);
  file.flush();
  file.close();
  LittleFS.remove(STATE_PATH);
  LittleFS.rename("/state.tmp", STATE_PATH);
  lastPersistMs = now;
  lastPersistTries = tries;
}

static void loadState() {
  // Format the data partition on its first boot (or if its filesystem is damaged).
  // The partition table already allocates the SPIFFS-labeled region for LittleFS.
  littleFsReady = LittleFS.begin(true);
  if (!littleFsReady) {
    logLine("LittleFS mount failed; persistent run stats unavailable.");
    return;
  }
  if (!LittleFS.exists(STATE_PATH)) return;
  File file = LittleFS.open(STATE_PATH, "r");
  if (!file) return;
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  if (error) {
    logLine("Saved state is invalid; starting with fresh stats.");
    return;
  }
  savedPuzzleId = doc["puzzle_id"] | 0;
  tries = doc["tries"] | 0ULL;
  accumulatedRunMs = doc["run_ms"] | 0ULL;
  runStartTries = doc["run_tries"] | tries;
  searching = (doc["searching"] | false) && savedPuzzleId > 0 && savedPuzzleId <= MAX_PUZZLE_ID;
  activeRunStartMs = 0;
  if (searching) activeRunStartMs = millis();
  logLine("Restored LittleFS state: puzzle " + String(savedPuzzleId) + ", " +
    String((unsigned long long)tries) + " attempts, " +
    String((unsigned long long)(accumulatedRunMs / 1000)) + " seconds total, " +
    String(searching ? "search will resume." : "search stopped."));
}

static bool sendTelegram(const String &text);

static void logLine(const String &s) {
  Serial.println(s);
}

static void reply(const String &s) {
  Serial.println(s);
  sendTelegram(s);
}

static void configureTls(WiFiClientSecure &client) {
  // Temporary diagnostic: distinguish TLS certificate validation from TCP/network failure.
  client.setInsecure();
  client.setHandshakeTimeout(15);
}

static bool extractJsonString(JsonVariantConst obj, const char *key, String &out) {
  const char *value = obj[key] | nullptr;
  if (!value || !*value) return false;
  out = value;
  return true;
}

static bool fetchPuzzle(uint16_t id, Puzzle &result) {
  // Public puzzle metadata for the first ten entries. These puzzles are solved,
  // but remain selectable so the normal search path can be exercised on-device.
  static const char *const addresses[EMBEDDED_PUZZLE_MAX] = {
    "1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH",
    "1CUNEBjYrCn2y1SdiUMohaKUi4wpP326Lb",
    "19ZewH8Kk1PDbSNdJ97FP4EiCjTRaZMZQA",
    "1EhqbyUMvvs7BfL8goY6qcPbD6YKfPqb7e",
    "1E6NuFjCi27W5zoXg8TRdcSRq84zJeBW3k",
    "1PitScNLyp2HCygzadCh7FveTnfmpPbfp8",
    "1McVt1vMtCC7yn5b9wgX1833yCcLXzueeC",
    "1M92tSqNmQLYw33fuBvjmeadirh1ysMBxK",
    "1CQFwcjw1dwhtkVWBttNLDtqL7ivBonGPV",
    "1LeBZP5QCwwgXRtmVUvTVrraqPUokyLHqe"
  };
  if (id >= 1 && id <= EMBEDDED_PUZZLE_MAX) {
    result.id = id;
    // Range n is [2^(n-1), 2^n - 1].
    mbedtls_mpi start, end;
    mbedtls_mpi_init(&start);
    mbedtls_mpi_init(&end);
    mbedtls_mpi_lset(&start, 1);
    mbedtls_mpi_shift_l(&start, id - 1);
    mbedtls_mpi_lset(&end, 1);
    mbedtls_mpi_shift_l(&end, id);
    mbedtls_mpi_sub_int(&end, &end, 1);
    char startBuf[65], endBuf[65];
    size_t startLen = 0, endLen = 0;
    mbedtls_mpi_write_string(&start, 16, startBuf, sizeof(startBuf), &startLen);
    mbedtls_mpi_write_string(&end, 16, endBuf, sizeof(endBuf), &endLen);
    mbedtls_mpi_free(&start);
    mbedtls_mpi_free(&end);
    result.startHex = startBuf;
    result.endHex = endBuf;
    result.address = addresses[id - 1];
    result.completed = true;
    result.loaded = true;
    return true;
  }

  String url = "https://btcpuzzle.info/puzzle/" + String(id);
  WiFiClientSecure client;
  configureTls(client);
  HTTPClient http;
  http.setTimeout(20000);
  http.setUserAgent("ESP32-BTCPuzzle/1.0");
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.collectHeaders((const char *[]) {"Content-Type", "Content-Length"}, 2);
  if (!http.begin(client, url)) {
    logLine("Could not start puzzle page request.");
    return false;
  }
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    char tlsError[160] = {0};
    client.lastError(tlsError, sizeof(tlsError));
    logLine("Puzzle page request failed: HTTP " + String(code) + "; TLS/socket: " + tlsError);
    http.end();
    return false;
  }

  Stream *stream = http.getStreamPtr();
  if (!stream->find("id=\"__NEXT_DATA__\"")) {
    logLine("Puzzle page did not contain Next.js data. HTTP " + String(code) +
      ", content type: " + http.header("Content-Type") +
      ", length: " + http.header("Content-Length"));
    http.end();
    return false;
  }
  while (stream->available() && stream->read() != '>') {}
  bool gotCode = false, gotStart = false, gotEnd = false, gotAddress = false, gotCompleted = false;
  String token;
  token.reserve(128);
  String key;
  String value;
  bool inString = false, escaped = false, readingKey = false, waitingValue = false;
  int objectDepth = 0;
  size_t scanned = 0;
  while (stream->available() && scanned < 300000) {
    int raw = stream->read();
    if (raw < 0) break;
    ++scanned;
    char c = (char)raw;
    if (inString) {
      if (escaped) { (readingKey ? key : value) += c; escaped = false; }
      else if (c == '\\') escaped = true;
      else if (c == '"') {
        inString = false;
        if (readingKey) { readingKey = false; waitingValue = true; }
        else if (waitingValue) {
          if (key == "hexStart") { result.startHex = value; gotStart = true; }
          else if (key == "hexEnd") { result.endHex = value; gotEnd = true; }
          else if (key == "targetWalletAddress") { result.address = value; gotAddress = true; }
          key = ""; value = ""; waitingValue = false;
        }
      } else (readingKey ? key : value) += c;
      continue;
    }
    if (c == '{') { ++objectDepth; continue; }
    if (c == '}') { --objectDepth; continue; }
    if (c == '"') {
      if (!waitingValue) { inString = true; readingKey = true; key = ""; }
      else { inString = true; readingKey = false; value = ""; }
      continue;
    }
    if (!waitingValue && c == ':') { waitingValue = true; continue; }
    if (waitingValue && c != ' ' && c != '\t' && c != '\r' && c != '\n') {
      if (key == "code" && (c == '-' || (c >= '0' && c <= '9'))) {
        String number(""); number += c;
        while (stream->available()) {
          int peeked = stream->peek();
          if (peeked < '0' || peeked > '9') break;
          number += (char)stream->read(); ++scanned;
        }
        gotCode = number.toInt() == id;
        key = ""; waitingValue = false;
      } else if (key == "isCompleted" && (c == 't' || c == 'f')) {
        result.completed = c == 't';
        while (stream->available() && isalpha(stream->peek())) { stream->read(); ++scanned; }
        gotCompleted = true; key = ""; waitingValue = false;
      } else if (c == ',' || c == '}') {
        key = ""; waitingValue = false;
      }
    }
    if (gotCode && gotStart && gotEnd && gotAddress && gotCompleted) break;
  }
  http.end();
  if (!(gotCode && gotStart && gotEnd && gotAddress && gotCompleted)) {
    logLine("Puzzle metadata fields missing after scanning " + String(scanned) + " bytes.");
    return false;
  }
  result.id = id;
  // The published BTC puzzle keyspace for puzzle n is [2^(n-1), 2^n-1].
  // Reject unexpected metadata rather than searching outside the advertised range.
  mbedtls_mpi publishedStart, publishedEnd, expectedStart, expectedEnd;
  mbedtls_mpi_init(&publishedStart);
  mbedtls_mpi_init(&publishedEnd);
  mbedtls_mpi_init(&expectedStart);
  mbedtls_mpi_init(&expectedEnd);
  bool rangeOk = mbedtls_mpi_read_string(&publishedStart, 16, result.startHex.c_str()) == 0 &&
                 mbedtls_mpi_read_string(&publishedEnd, 16, result.endHex.c_str()) == 0 &&
                 mbedtls_mpi_lset(&expectedStart, 1) == 0 &&
                 mbedtls_mpi_shift_l(&expectedStart, id - 1) == 0 &&
                 mbedtls_mpi_lset(&expectedEnd, 1) == 0 &&
                 mbedtls_mpi_shift_l(&expectedEnd, id) == 0 &&
                 mbedtls_mpi_sub_int(&expectedEnd, &expectedEnd, 1) == 0 &&
                 mbedtls_mpi_cmp_mpi(&publishedStart, &expectedStart) == 0 &&
                 mbedtls_mpi_cmp_mpi(&publishedEnd, &expectedEnd) == 0;
  mbedtls_mpi_free(&publishedStart);
  mbedtls_mpi_free(&publishedEnd);
  mbedtls_mpi_free(&expectedStart);
  mbedtls_mpi_free(&expectedEnd);
  if (!rangeOk) {
    logLine("Published key range did not match this puzzle ID; refusing to search.");
    return false;
  }
  result.loaded = true;
  return true;
}

static int rngCallback(void *, unsigned char *output, size_t length) {
  esp_fill_random(output, length);
  return 0;
}

static String bytesToHex(const uint8_t *data, size_t length) {
  static const char hex[] = "0123456789abcdef";
  String out;
  out.reserve(length * 2);
  for (size_t i = 0; i < length; ++i) {
    out += hex[data[i] >> 4];
    out += hex[data[i] & 0x0f];
  }
  return out;
}

static bool makeRandomPuzzleKey(uint16_t bits, uint8_t key[32]) {
  if (bits == 0 || bits > 160) return false;
  esp_fill_random(key, 32);
  const uint16_t bytes = (bits + 7) / 8;
  const uint16_t first = 32 - bytes;
  for (uint16_t i = 0; i < first; ++i) key[i] = 0;
  const uint8_t highBits = bits % 8;
  if (highBits) {
    const uint8_t mask = (1U << highBits) - 1;
    key[first] &= mask;
    key[first] |= 1U << (highBits - 1);
  } else {
    key[first] |= 0x80;
  }
  return true;
}

static String base58Encode(const uint8_t *input, size_t length) {
  static const char alphabet[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
  size_t zeros = 0;
  while (zeros < length && input[zeros] == 0) ++zeros;
  uint8_t work[80];
  if (length > sizeof(work)) return "";
  memcpy(work, input, length);
  char encoded[120];
  size_t out = sizeof(encoded);
  size_t start = zeros;
  while (start < length) {
    unsigned remainder = 0;
    for (size_t i = start; i < length; ++i) {
      unsigned value = (remainder << 8) | work[i];
      work[i] = value / 58;
      remainder = value % 58;
    }
    encoded[--out] = alphabet[remainder];
    while (start < length && work[start] == 0) ++start;
  }
  while (out < sizeof(encoded) && encoded[out] == alphabet[0]) ++out;
  while (zeros--) encoded[--out] = alphabet[0];
  return String(encoded + out, sizeof(encoded) - out);
}

static String base58Check(const uint8_t *payload, size_t payloadLength) {
  uint8_t buffer[80];
  if (payloadLength + 4 > sizeof(buffer)) return "";
  memcpy(buffer, payload, payloadLength);
  uint8_t firstHash[32], secondHash[32];
  if (mbedtls_sha256(payload, payloadLength, firstHash, 0) != 0 ||
      mbedtls_sha256(firstHash, sizeof(firstHash), secondHash, 0) != 0) return "";
  memcpy(buffer + payloadLength, secondHash, 4);
  return base58Encode(buffer, payloadLength + 4);
}

static String addressForPublicKey(const uint8_t *pubkey, size_t pubkeyLength) {
  uint8_t sha[32], ripe[20], payload[21];
  if (mbedtls_sha256(pubkey, pubkeyLength, sha, 0) != 0) return "";
  CRIPEMD160 ripemd;
  ripemd.Write(sha, sizeof(sha));
  ripemd.Finalize(ripe);
  payload[0] = 0x00;  // Bitcoin mainnet P2PKH version byte.
  memcpy(payload + 1, ripe, sizeof(ripe));
  return base58Check(payload, sizeof(payload));
}

static String wifForPrivateKey(const uint8_t key[32], bool compressed) {
  uint8_t payload[34];
  payload[0] = 0x80;
  memcpy(payload + 1, key, 32);
  size_t length = 33;
  if (compressed) payload[length++] = 0x01;
  return base58Check(payload, length);
}

static bool testCandidate(const uint8_t key[32], String &matchedAddress,
                          bool &compressedMatch) {
  if (mbedtls_mpi_read_binary(&privateScalar, key, 32) != 0) return false;
  if (mbedtls_ecp_mul(&secpGroup, &publicPoint, &privateScalar,
                      &secpGroup.G, rngCallback, nullptr) != 0) return false;

  uint8_t encoded[65];
  size_t encodedLength = 0;
  if (mbedtls_ecp_point_write_binary(&secpGroup, &publicPoint,
          MBEDTLS_ECP_PF_COMPRESSED, &encodedLength, encoded, sizeof(encoded)) != 0) return false;
  String compressedAddress = addressForPublicKey(encoded, encodedLength);
  if (compressedAddress == puzzle.address) {
    matchedAddress = compressedAddress;
    compressedMatch = true;
    return true;
  }

  if (mbedtls_ecp_point_write_binary(&secpGroup, &publicPoint,
          MBEDTLS_ECP_PF_UNCOMPRESSED, &encodedLength, encoded, sizeof(encoded)) != 0) return false;
  String uncompressedAddress = addressForPublicKey(encoded, encodedLength);
  if (uncompressedAddress == puzzle.address) {
    matchedAddress = uncompressedAddress;
    compressedMatch = false;
    return true;
  }
  return false;
}

static void blinkKeyChecked() {
#if defined(RGB_BUILTIN)
  rgbLedWrite(RGB_BUILTIN, 0, 24, 0);
  delay(KEY_CHECK_BLINK_MS);
  rgbLedWrite(RGB_BUILTIN, 0, 0, 0);
#elif defined(LED_BUILTIN)
  // The Nologo C3 Super Mini's single-color onboard LED is active-low.
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
  delay(KEY_CHECK_BLINK_MS);
  digitalWrite(LED_BUILTIN, HIGH);
#endif
}

static String telegramUrl(const char *method) {
  return String("https://api.telegram.org/bot") + TELEGRAM_BOT_TOKEN + "/" + method;
}

static bool sendTelegram(const String &text) {
  if (WiFi.status() != WL_CONNECTED || String(TELEGRAM_BOT_TOKEN).length() < 20) return false;
  WiFiClientSecure client;
  configureTls(client);
  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(client, telegramUrl("sendMessage"))) return false;
  http.addHeader("Content-Type", "application/json");
  JsonDocument body;
  body["chat_id"] = TELEGRAM_OWNER_CHAT_ID;
  body["text"] = text;
  String payload;
  serializeJson(body, payload);
  const int code = http.POST(payload);
  if (code < 200 || code >= 300) logLine("Telegram send failed: HTTP " + String(code));
  http.end();
  return code >= 200 && code < 300;
}

static String formatStats() {
  uint64_t elapsedMs = accumulatedRunMs + (searching ? millis() - activeRunStartMs : 0);
  uint64_t runAttempts = tries - runStartTries;
  double rate = elapsedMs ? (double)runAttempts * 1000.0 / elapsedMs : 0.0;
  String msg = "Puzzle: " + String(puzzle.id) + "\n";
  msg += "Target: " + puzzle.address + "\n";
  msg += "State: " + String(searching ? "searching" : "stopped") + "\n";
  msg += "Random candidates checked: " + String((unsigned long long)tries) + "\n";
  msg += "Rate: " + String(rate, 2) + " keys/s\n";
  msg += "Run time: " + String((unsigned long)(elapsedMs / 1000)) + " s\n";
  msg += "Search method: random sampling in [2^" + String(puzzle.id - 1) +
         ", 2^" + String(puzzle.id) + "-1]";
  return msg;
}

static void handleCommand(const String &text) {
  String command = text;
  command.trim();
  const int space = command.indexOf(' ');
  String verb = space < 0 ? command : command.substring(0, space);
  String arg = space < 0 ? "" : command.substring(space + 1);
  const int at = verb.indexOf('@');
  if (at >= 0) verb = verb.substring(0, at);
  verb.toLowerCase();

  if (verb == "/help" || verb == "/start") {
    reply("ESP32 standalone BTC puzzle client\n"
      "/puzzle <ID> - load public puzzle target (1-160; 1-10 are embedded)\n"
      "/run - start randomized local search\n"
      "/stop - stop search\n"
      "/status - show tries and local keys/s\n"
      "Serial and Telegram commands are supported. No pool work is requested or submitted.");
  } else if (verb == "/puzzle") {
    int id = arg.toInt();
    if (searching) {
      reply("Stop the search before changing puzzle.");
    } else if (id < 1 || id > MAX_PUZZLE_ID) {
      reply("Puzzle ID must be 1 through 160.");
    } else {
      Puzzle next;
      reply("Loading public metadata for puzzle " + String(id) + "...");
      if (fetchPuzzle(id, next)) {
        puzzle = next;
        savedPuzzleId = puzzle.id;
        tries = 0;
        runStartTries = 0;
        accumulatedRunMs = 0;
        activeRunStartMs = 0;
        saveState(true);
        reply("Loaded puzzle " + String(puzzle.id) + "\nTarget: " + puzzle.address +
          "\nRange: " + puzzle.startHex + " .. " + puzzle.endHex +
          String(puzzle.completed ? "\nSite status: solved" : "\nSite status: unsolved") +
          "\nUse /run to start random search.");
      } else {
        reply("Could not load puzzle " + String(id) +
          ". Check the USB serial log for the HTTP status or parsing error. "
          "Puzzle IDs 11-70 currently return 404 from btcpuzzle.info; try an embedded ID 1-10 or an online ID 71+.");
      }
    }
  } else if (verb == "/run") {
    if (!puzzle.loaded) {
      reply("Choose a puzzle first with /puzzle <ID>.");
    } else if (!searching) {
      searching = true;
      activeRunStartMs = millis();
      runStartTries = tries;
      saveState(true);
      lastStatsTries = tries;
      lastStatsMs = millis();
      reply("Random search started for puzzle " + String(puzzle.id) +
        ". Candidates stay inside its published key range.");
    } else {
      reply("Search is already running.\n\n" + formatStats());
    }
  } else if (verb == "/stop") {
    searching = false;
    if (activeRunStartMs) accumulatedRunMs += millis() - activeRunStartMs;
    activeRunStartMs = 0;
    saveState(true);
    reply("Search stopped.\n\n" + formatStats());
  } else if (verb == "/status") {
    reply(puzzle.loaded ? formatStats() : "No puzzle selected. Use /puzzle <ID>.");
  } else {
    reply("Unknown command. Send /help.");
  }
}

static void pollSerial() {
  static String line;
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c == '\r' || c == '\n') {
      if (!line.isEmpty()) {
        Serial.println(String("> ") + line);
        handleCommand(line);
        line = "";
      }
    } else if (line.length() < 160) {
      line += c;
    }
  }
}

static void pollTelegram() {
  if (WiFi.status() != WL_CONNECTED || String(TELEGRAM_BOT_TOKEN).length() < 20) return;
  const uint32_t pollStartedMs = millis();
  WiFiClientSecure client;
  configureTls(client);
  HTTPClient http;
  http.setTimeout(10000);
  String url = telegramUrl("getUpdates") + "?offset=" + String((long long)telegramOffset) + "&timeout=0&limit=5";
  if (!http.begin(client, url)) return;
  const int code = http.GET();
  const uint32_t pollDurationMs = millis() - pollStartedMs;
  if (pollDurationMs >= 1000) {
    logLine("Telegram poll HTTP " + String(code) + " took " + String(pollDurationMs) +
      " ms; search pauses during polling.");
  }
  if (code != HTTP_CODE_OK) {
    logLine("Telegram poll failed: HTTP " + String(code));
    http.end();
    return;
  }
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, *http.getStreamPtr());
  http.end();
  if (error) {
    logLine("Telegram update parse failed: " + String(error.c_str()));
    return;
  }
  JsonArray updates = doc["result"].as<JsonArray>();
  for (JsonObject update : updates) {
    int64_t updateId = update["update_id"] | -1;
    if (updateId >= telegramOffset) telegramOffset = updateId + 1;
    JsonObject message = update["message"];
    if (message.isNull()) continue;
    int64_t chatId = message["chat"]["id"] | 0;
    if (chatId != strtoll(TELEGRAM_OWNER_CHAT_ID, nullptr, 10)) continue;
    const char *incoming = message["text"] | "";
    if (incoming[0] == '/') handleCommand(String(incoming));
  }
}

static void reportFound(const uint8_t key[32], const String &address, bool compressed) {
  pendingFoundMessage = "TARGET KEY FOUND\nPuzzle: " + String(puzzle.id) +
    "\nAddress: " + address + "\nPrivate key (hex): " + bytesToHex(key, 32) +
    "\nPrivate key (WIF): " + wifForPrivateKey(key, compressed) +
    "\nChecked: " + String((unsigned long long)tries) + " candidates";
  searching = false;
  if (activeRunStartMs) accumulatedRunMs += millis() - activeRunStartMs;
  activeRunStartMs = 0;
  saveState(true);
  if (sendTelegram(pendingFoundMessage)) {
    pendingFoundMessage = "";
    logLine("Target match found and sent to Telegram.");
  } else {
    logLine("Target match found; keeping the result in RAM and retrying Telegram delivery.");
  }
}

static void searchOneCandidate() {
  if (!searching || !puzzle.loaded) return;
  uint8_t key[32];
  if (!makeRandomPuzzleKey(puzzle.id, key)) {
    searching = false;
    return;
  }
  String matchedAddress;
  bool compressed = true;
  ++tries;
  saveState();
  const bool found = testCandidate(key, matchedAddress, compressed);
  blinkKeyChecked();
  if (found) {
    reportFound(key, matchedAddress, compressed);
    memset(key, 0, sizeof(key));
    return;
  }
  memset(key, 0, sizeof(key));
}

void setup() {
  Serial.begin(115200);
  delay(300);
  logLine("ESP32 standalone BTC puzzle client starting.");
  logLine("No pool requests or progress submissions are made.");
  if (!strcmp(WIFI_SSID, "YOUR_WIFI_NAME") || !strcmp(TELEGRAM_BOT_TOKEN, "PASTE_BOT_TOKEN_HERE") ||
      !strcmp(TELEGRAM_OWNER_CHAT_ID, "PASTE_NUMERIC_CHAT_ID_HERE") ||
      String(WIFI_SSID).isEmpty() || String(TELEGRAM_BOT_TOKEN).length() < 20 ||
      String(TELEGRAM_OWNER_CHAT_ID).isEmpty()) {
    logLine("Configure WIFI_SSID, WIFI_PASSWORD, TELEGRAM_BOT_TOKEN, and TELEGRAM_OWNER_CHAT_ID in secrets.h.");
    return;
  }
  credentialsPresent = true;

  mbedtls_ecp_group_init(&secpGroup);
  mbedtls_ecp_point_init(&publicPoint);
  mbedtls_mpi_init(&privateScalar);
  int ecpResult = mbedtls_ecp_group_load(&secpGroup, MBEDTLS_ECP_DP_SECP256K1);
  if (ecpResult != 0) {
    logLine("This Arduino core was built without secp256k1 support.");
    return;
  }
  cryptoReady = true;

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  logLine("Connecting to Wi-Fi...");
  uint32_t wifiWait = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiWait < 30000) {
    delay(250);
  }
  if (WiFi.status() != WL_CONNECTED) {
    logLine("Wi-Fi connection failed.");
    return;
  }
  logLine("Wi-Fi connected; TLS uses the built-in certificate bundle.");
  logLine("Serial console ready at 115200 baud. Type /help and press Enter.");
  loadState();
  const bool resumeAfterBoot = searching;
  if (savedPuzzleId > 0 && savedPuzzleId <= MAX_PUZZLE_ID) {
    Puzzle loaded;
    if (fetchPuzzle(savedPuzzleId, loaded)) {
      puzzle = loaded;
      logLine("Restored puzzle " + String(savedPuzzleId) + " target " + puzzle.address);
      if (resumeAfterBoot && searching) {
        reply("Search resumed after reboot.\nPuzzle: " + String(puzzle.id) +
          "\nAttempts so far: " + String((unsigned long long)tries));
      }
    } else {
      searching = false;
      logLine("Could not restore selected puzzle; search remains stopped.");
    }
  }
  reply("ESP32 BTC puzzle client online. No pool is used.\nSend /help.");
  appReady = true;
}

void loop() {
  if (!appReady) {
    if (credentialsPresent && cryptoReady && WiFi.status() != WL_CONNECTED &&
        millis() - lastWifiReconnectMs >= 10000) {
      lastWifiReconnectMs = millis();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
    delay(1000);
    return;
  }
  pollSerial();
  if (WiFi.status() != WL_CONNECTED && millis() - lastWifiReconnectMs >= 10000) {
    lastWifiReconnectMs = millis();
    WiFi.reconnect();
  }
  const uint32_t now = millis();
  if (!pendingFoundMessage.isEmpty() && now - lastFoundRetryMs >= 10000) {
    lastFoundRetryMs = now;
    if (sendTelegram(pendingFoundMessage)) {
      pendingFoundMessage = "";
      logLine("Pending result delivered to Telegram.");
    }
  }
  saveState();
  if (now - lastTelegramPollMs >= TELEGRAM_POLL_MS) {
    lastTelegramPollMs = now;
    pollTelegram();
  }
  if (searching) {
    searchOneCandidate();
    if (millis() - lastStatsMs >= STATS_LOG_MS) {
      uint32_t elapsed = millis() - lastStatsMs;
      uint64_t delta = tries - lastStatsTries;
      double rate = elapsed ? (double)delta * 1000.0 / elapsed : 0.0;
      logLine("Puzzle " + String(puzzle.id) + ": " + String((unsigned long long)tries) +
              " attempts, " + String(rate, 2) + " candidates/s");
      lastStatsTries = tries;
      lastStatsMs = millis();
    }
  } else {
    delay(10);
  }
}
