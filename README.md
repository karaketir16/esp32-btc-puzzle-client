# ESP32 BTC Puzzle Client

An experimental standalone client for the public BTC Puzzle challenge. It embeds public puzzle targets and searches locally on an ESP32. The default search starts at a new random key every 60 seconds and increments within each chunk; an independent-random-key mode is also available. It does not request work from a pool, submit ranges, or report progress to a pool.

This is a learning project, not a practical way to find a key. The ESP32 has no secp256k1 accelerator, so its search rate is extremely small compared with the size of the puzzle ranges. Searches may revisit keys; attempt counts are not a count of unique keys or proof of covered space.

## Features

- Telegram bot commands: `/help`, `/puzzle <ID>`, `/run`, `/stop`, and `/status`.
- USB serial command interface, with the same commands (without the leading slash also accepted).
- Public target addresses for all 160 puzzle IDs embedded in the firmware; ranges are computed locally.
- Public puzzle-list URL included in puzzle/status replies for manual solved-status checks.
- Local checks for compressed and uncompressed legacy P2PKH addresses.
- LittleFS persistence for selected puzzle, candidate count, accumulated search time, and whether a search was active. An active search resumes after reboot. A sudden power loss may lose changes since the last checkpoint.
- LED pulse while checking a candidate.
- Found keys are saved independently of the selected puzzle, recalled by `/status`, `/found [ID]`, or selecting that puzzle again, and retained until `/clear-found [ID|all]`.
- Pending Telegram deliveries survive reboot and are retried; delivery is at least once, so a crash around acknowledgment can produce a duplicate message.
- `/mode chunk` (default) chooses a new random start every minute; `/mode random` chooses an independent random key each attempt.
- `/selftest [1-10|all] [walk|random]` actually searches the first ten targets. The default walk test uses random chunks of at most 50 checked keys or roughly 2 seconds, verifies curve points against fresh multiplication, stores discoveries, and resumes the normal run.
- Shared C++ modules and compiled host tests exercise crypto, storage, statistics, and the sketch command handlers.

## Hardware

The sketch supports ESP32-S3 and ESP32-C3 Arduino targets. `SERIAL_ONLY` defaults to `0` (false), which enables Wi-Fi and Telegram. Set it to `1` to disable both and use USB serial only. Embedded targets and ranges work offline, so serial-only mode needs no Wi-Fi credentials or `secrets.h`. The firmware does not query the puzzle site for solved status; puzzle replies include its URL so you can check manually.

Select the exact board definition for your module where available. Example FQBNs:

| Board | Arduino FQBN |
| --- | --- |
| ESP32-S3 Super Mini used during development | `esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi` |
| Generic ESP32-C3 Dev Module | `esp32:esp32:esp32c3` |
| Nologo ESP32-C3 Super Mini | `esp32:esp32:nologo_esp32c3_super_mini` |

The Telegram build uses most of the default 1.25 MiB app partition within the 4 MB flash. Serial-only builds are considerably smaller. If a C3 board has a smaller flash chip or a different partition layout, the standard build may not fit.

## Install Arduino CLI and board support

Install [Arduino CLI](https://arduino.github.io/arduino-cli/) and the Espressif Arduino core. Then install the JSON dependency:

```sh
arduino-cli config init
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.3.12
arduino-cli lib install ArduinoJson@7.4.3
```

If the ESP32 core's package index is not configured on a fresh CLI installation, add it first:

```sh
arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
```

## Configure credentials

Copy `secrets.example.h` to `secrets.h`, then set your Wi-Fi name/password, Telegram bot token, and numeric Telegram chat ID. The real `secrets.h` is ignored by Git and must not be committed or shared.

```sh
cp secrets.example.h secrets.h
```

To find your chat ID, stop or unplug the ESP32 first so it does not compete with the script for Telegram updates. Send `/start` to your bot, then run:

```sh
python3 get_chat_id.py
```

The script reads the bot token from `TELEGRAM_BOT_TOKEN` in `secrets.h`; if that file is unavailable, it prompts for the token without displaying it. It waits up to 30 seconds for updates and prints the chat ID(s). The script uses only Python's standard library. If another bot client is polling at the same time, Telegram may return a conflict error; stop that client and retry.

Without Telegram credentials, serial control remains available. In `SERIAL_ONLY=1`, the radio is disabled and an ADC-based hardware entropy source is enabled; this project does not otherwise use the ADC. Found keys are stored unencrypted in device flash and are returned through serial and the configured Telegram chat.

## Compile

Run these commands from the repository directory. Change the FQBN to match your exact board if needed.

ESP32-S3 Super Mini:

```sh
arduino-cli compile --fqbn 'esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi' .
```

Generic ESP32-C3:

```sh
arduino-cli compile --fqbn 'esp32:esp32:esp32c3' .
```

ESP32-C3 Super Mini (Nologo board definition):

```sh
arduino-cli compile --fqbn 'esp32:esp32:nologo_esp32c3_super_mini' .
```

Serial-only C3 Super Mini build (Wi-Fi and Telegram disabled):

```sh
arduino-cli compile --fqbn 'esp32:esp32:nologo_esp32c3_super_mini' --build-property 'compiler.cpp.extra_flags=-DSERIAL_ONLY=1' --output-dir build/serial-only .
```

Flash those compiled serial-only binaries:

```sh
arduino-cli upload -p /dev/cu.usbmodemXXXX --fqbn 'esp32:esp32:nologo_esp32c3_super_mini' --input-dir build/serial-only
```

Omit the build property for the default `SERIAL_ONLY=0` build. For an S3 serial-only build, use the S3 FQBN above with the same build property.

To see installed board definitions, run `arduino-cli board listall esp32c3` or `arduino-cli board listall esp32s3`.

## Flash

Connect the board by USB and find its serial port:

```sh
arduino-cli board list
```

On macOS, ports commonly look like `/dev/cu.usbmodem...` or `/dev/cu.wchusbserial...`. Replace the port and FQBN below with the ones for your board.

ESP32-S3 Super Mini:

```sh
arduino-cli upload -p /dev/cu.usbmodemXXXX --fqbn 'esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi' .
```

Generic ESP32-C3:

```sh
arduino-cli upload -p /dev/cu.usbmodemXXXX --fqbn 'esp32:esp32:esp32c3' .
```

### C3 native USB reset fallback

On the Nologo ESP32-C3 Super Mini's native USB Serial/JTAG connection, the ordinary uploader's `--before default-reset` sometimes failed with `No serial data received`. Using esptool's `--before usb-reset` connected successfully without pressing BOOT or RESET. This was tested with esptool 5.3.1 on macOS.

Close any serial monitor, compile the serial-only build above, and run this from the repository directory. Replace the port with the one reported by `arduino-cli board list`:

```sh
ESPTOOL="$HOME/Library/Arduino15/packages/esp32/tools/esptool_py/5.3.1/esptool"
"$ESPTOOL" --chip esp32c3 --port /dev/cu.usbmodemXXXX --baud 921600 \
  --before usb-reset --after hard-reset \
  write-flash -z 0x10000 build/serial-only/esp32-btcpuzzle.ino.bin
```

Adjust the esptool version/path if your Arduino installation differs. This command writes only the application at `0x10000`; it assumes the standard C3 bootloader and partition table have already been flashed. For another build, substitute its application `.bin` file. It preserves the existing filesystem partition.

Successful output includes `Connected to ESP32-C3`, `Hash of data verified`, and `Hard resetting via RTS pin`. The hash check confirms that the written application matches the supplied binary. Open the serial monitor afterward to check startup and restored state.

If USB reset still cannot connect, hold **BOOT**, tap **RESET** (or unplug/replug while holding BOOT), start the upload, then release BOOT when the uploader begins connecting. Board button labels and timing vary.

## Use the serial interface

Open a serial monitor at 115200 baud:

```sh
arduino-cli monitor -p /dev/cu.usbmodemXXXX --config baudrate=115200
```

Enter commands such as:

```text
help
puzzle 4
mode chunk
run
status
stop
found 10
selftest 10
```

The same commands can be sent to the configured Telegram bot. Choose a puzzle, then issue `run` to start local search. If a saved result already exists for that puzzle, it is reported immediately; clear that record before deliberately searching for it again. Use `status` to see local attempts and rate. Site-wide/pool statistics are not this board's progress.

## Puzzle ranges and limitations

Puzzle `n` searches the inclusive range `[2^(n-1), 2^n - 1]`. Addresses for all 160 puzzles are embedded in `puzzle_metadata.h`, captured from the [public puzzle listing](https://btcpuzzle.info/puzzle). Targets work offline. The firmware does not fetch solved status; `/puzzle` and `/status` responses give you the listing URL to check manually. Update the embedded address list if the challenge adds puzzles or changes a target.

The S3 performs secp256k1 operations in software. The C3 is slower still. The chip's cryptographic peripherals do not provide the secp256k1 point multiplication needed by this search, so this project should be treated as a demonstration and not a realistic strategy for finding a key.


## Search modes and device self-tests

Stop a run before changing its mode:

```text
/stop
/mode chunk
/run
```

`chunk` is the default: pick a uniformly sampled starting key within the puzzle range, step sequentially for 60 seconds, then pick another random starting key. Reaching the upper boundary also triggers a new random start. Integer curve arithmetic has no rounding-error limit on the length of a walk. The firmware checks the first incremental step of every chunk against a separate scalar multiplication.

`/mode random` performs a new scalar multiplication for every independently random candidate. It is much slower. On the development C3 the new minute-chunk build measured 70.35 keys/s in a 30-second window, while the fresh-random self-test measured about 5.4 keys/s. Do not use chunk attempts as an independent-random-sampling coverage estimate.

```text
/selftest 10
/status
/stop
/selftest all
```

`/selftest` with no argument searches puzzles 1 through 10 in **walk mode**. Select one puzzle or the independent-random test explicitly:

```text
/selftest 10 walk
/selftest all walk
/selftest 10 random
/selftest all random
```

Walk mode uses the same incremental curve-addition implementation as normal chunk searching. Each diagnostic chunk starts randomly and ends after **50 checked keys, roughly two seconds, or the upper range boundary**, whichever comes first. The time limit is checked between operations; a scalar multiplication or network call already in progress can overrun it.

The first increment, each chunk's final point (including a timed-out pending point), and every matching point are compared with a separate fresh scalar multiplication. A mismatch stops the diagnostic and normal run. A final address match is also checked against the public known private key. These extra reference checks add overhead, so diagnostic speed is not normal search speed. Pass replies report attempts, incremental steps, reference checks, and chunks. Reference checks can check the same point more than once; they are not a unique-point coverage count. Puzzle 1 has only one possible key and therefore cannot exercise an in-range increment; its walk-step count is correctly zero.

Random mode samples a fresh independent key for every attempt. For example, puzzle 10 has 512 possible keys, so its expected independent-random solve time is 512 attempts divided by the observed fresh-random rate; an individual run can take considerably longer. Walk chunks have a different search distribution, so that independent-sampling estimate does not apply to them.

Both modes use the production C++ crypto and matching code. Known keys are consulted only after a matching address is discovered. Existing saved records never skip or seed a diagnostic. `/status` and `/stop` remain available. The selected normal puzzle and its counters are preserved, its time is paused, and a previously active run resumes after a successful test. Discoveries are saved without overwriting an earlier discovery for the same puzzle. A reboot cancels an unfinished diagnostic and restores the normal saved run.

## Found keys and recoverable checkpoints

`/status` shows a selected puzzle's saved private key/WIF, the stored puzzle IDs, and run statistics. `/found` reports all records; `/found 10` reports one. `/puzzle 10` and `/run` report an existing record immediately.

`/clear-found 10` deletes that puzzle's saved result. `/clear-found` or `/clear-found all` deletes all found records and pending notifications. Clearing found records does not reset normal run counters.

Found records and run state use separate two-slot journals (`/found-a.dat`, `/found-b.dat`, `/run-a.dat`, `/run-b.dat`). Each checkpoint has a sequence number and SHA256 integrity check. A save writes the inactive slot and reads it back before reporting success; the previous valid slot is retained throughout. On reboot the latest valid slot is chosen. A torn write falls back to the previous checkpoint. Ordinary run state is checkpointed every 10 seconds while active; a sudden reset can lose that interval of counters. Results are checkpointed immediately before attempted delivery. A flash-write failure is reported and retained in RAM for retry; keep power connected in that situation.

A mount failure on a nonblank filesystem disables searching and never triggers automatic formatting. Only a confirmed erased factory partition is automatically formatted.

Existing `/state.json` checkpoints are migrated. If the old firmware left only a complete `/state.tmp`, that can also be recovered. Lifetime average speed uses total attempts divided by the same accumulated active time, across stop/start and reboot. Clock deltas use 32-bit subtraction and are accumulated into a 64-bit duration to handle `millis()` rollover.

## Telegram server authentication

The fixed chat ID authorizes a Telegram chat to issue commands and identifies the recipient. HTTPS server authentication checks the certificate chain and hostname for `api.telegram.org`, so the ESP32 knows where it is sending its bot token and results.

This version uses `WiFiClientSecure::useBuiltinCACertBundle()` from Arduino ESP32 core 3.3.12 and SNTP time synchronization for certificate date checks. It does not disable certificate verification. If Wi-Fi, time synchronization, or certificate verification fails, Telegram stays unavailable and saved notifications wait for retry. Serial and embedded targets remain usable. Keep the board core updated to update its trusted CA bundle.

## Compiled C++ host tests

The production modules are `search_core.*`, `checkpoint_store.*`, `found_records.*`, and `run_stats.h`. Tests compile these files directly, and additionally compile the actual sketch against small host serial/clock/filesystem adapters. They do not reproduce the curve algorithm in Python.

Install CMake, a C++17 compiler, and OpenSSL development headers/libraries. CMake fetches checksummed Mbed TLS 3.6.6 unless `MBEDTLS_SOURCE_DIR` points to its source; it fetches checksummed ArduinoJson 7.4.3 unless `ARDUINOJSON_INCLUDE_DIR` points to its headers.

```sh
cmake -S tests -B build/host-tests -DSANITIZERS=ON
cmake --build build/host-tests -j
ctest --test-dir build/host-tests --output-on-failure

cmake -S tests -B build/host-tests32 -DTEST_32BIT_LIMBS=ON -DSANITIZERS=ON
cmake --build build/host-tests32 -j
ctest --test-dir build/host-tests32 --output-on-failure
```

Tests cover differential curve checks against OpenSSL, 83 public solved keys, both public-key encodings, WIF, range boundaries, error reporting, independent random searches, interrupted/corrupted checkpoints, found-record reload/clear/notifications, clock rollover, stop/start accounting, commands, self-test pause/resume, 50-key and time restart caps, deliberate walk-point corruption detection, and legacy checkpoint migration. Sanitizers instrument the project code, not the separately built Mbed TLS library. Host platform adapters cannot establish physical flash behavior or a live Telegram connection; device checks complement them.

For an optional automated serial diagnostic, install `pyserial` in a virtual environment and run:

```sh
python3 tests/serial_selftest.py --port /dev/cu.usbmodemXXXX --id all --mode walk --timeout 900
```

The script issues `/selftest all walk` (or use `--mode random`), checks the pass messages, verifies the original puzzle remains selected, and queries the saved result. It does not inject known keys into the solver.

## License and attribution

The project is licensed under GPL-3.0-or-later. `ripemd160.cpp` and `ripemd160.h` are derived from VanitySearch, with the x86-only rotate operation replaced by a portable equivalent. See `LICENSE` for the license text.
