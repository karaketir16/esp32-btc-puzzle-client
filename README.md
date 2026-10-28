# ESP32 BTC Puzzle Client

An experimental standalone client for the public BTC Puzzle challenge. It downloads public puzzle metadata and searches randomized private-key candidates locally on an ESP32. It does not request work from a pool, submit ranges, or report progress to a pool.

This is a learning project, not a practical way to find a key. The ESP32 has no secp256k1 accelerator, so its search rate is extremely small compared with the size of the puzzle ranges. Candidates are selected randomly and may be checked more than once.

## Features

- Telegram bot commands: `/help`, `/puzzle <ID>`, `/run`, `/stop`, and `/status`.
- USB serial command interface, with the same commands (without the leading slash also accepted).
- Public target addresses for all 160 puzzle IDs embedded in the firmware; ranges are computed locally.
- Public puzzle-list URL included in puzzle/status replies for manual solved-status checks.
- Local checks for compressed and uncompressed legacy P2PKH addresses.
- LittleFS persistence for selected puzzle, candidate count, accumulated search time, and whether a search was active. An active search resumes after reboot. A sudden power loss may lose changes since the last checkpoint.
- LED pulse while checking a candidate.
- A found result is sent to the configured Telegram chat.

## Hardware

The sketch supports ESP32-S3 and ESP32-C3 Arduino targets. `SERIAL_ONLY` defaults to `0` (false), which enables Wi-Fi and Telegram. Set it to `1` to disable both and use USB serial only. Embedded targets and ranges work offline, so serial-only mode needs no Wi-Fi credentials or `secrets.h`. The firmware does not query the puzzle site for solved status; puzzle replies include its URL so you can check manually.

Select the exact board definition for your module where available. Example FQBNs:

| Board | Arduino FQBN |
| --- | --- |
| ESP32-S3 Super Mini used during development | `esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi` |
| Generic ESP32-C3 Dev Module | `esp32:esp32:esp32c3` |
| Nologo ESP32-C3 Super Mini | `esp32:esp32:nologo_esp32c3_super_mini` |

The C3 build has less performance headroom than the S3 and uses most of the default 4 MB app partition. If a C3 board has a smaller flash chip or a different partition layout, the standard build may not fit.

## Install Arduino CLI and board support

Install [Arduino CLI](https://arduino.github.io/arduino-cli/) and the Espressif Arduino core. Then install the JSON dependency:

```sh
arduino-cli config init
arduino-cli core update-index
arduino-cli core install esp32:esp32
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

Puzzle solving can run without Telegram credentials, but Wi-Fi and Telegram features will be unavailable. Use a private Telegram chat: if a key is found, the program sends the private key and WIF in plaintext.

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

Flash that serial-only build with the same option:

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

If upload cannot connect, hold **BOOT**, tap **RESET** (or unplug/replug while holding BOOT), start the upload, then release BOOT when the uploader begins connecting. Board button labels and timing vary.

## Use the serial interface

Open a serial monitor at 115200 baud:

```sh
arduino-cli monitor -p /dev/cu.usbmodemXXXX --config baudrate=115200
```

Enter commands such as:

```text
help
puzzle 4
run
status
stop
```

The same commands can be sent to the configured Telegram bot. Choose a puzzle, then issue `run` to start local search. Use `status` to see local attempts and rate. Site-wide/pool statistics are not this board's progress.

## Puzzle ranges and limitations

Puzzle `n` searches the inclusive range `[2^(n-1), 2^n - 1]`. Addresses for all 160 puzzles are embedded in `puzzle_metadata.h`, captured from the [public puzzle listing](https://btcpuzzle.info/puzzle). Targets work offline. The firmware does not fetch solved status; `/puzzle` and `/status` responses give you the listing URL to check manually. Update the embedded address list if the challenge adds puzzles or changes a target.

The S3 performs secp256k1 operations in software. The C3 is slower still. The chip's cryptographic peripherals do not provide the secp256k1 point multiplication needed by this search, so this project should be treated as a demonstration and not a realistic strategy for finding a key.

## License and attribution

The project is licensed under GPL-3.0-or-later. `ripemd160.cpp` and `ripemd160.h` are derived from VanitySearch, with the x86-only rotate operation replaced by a portable equivalent. See `LICENSE` for the license text.
