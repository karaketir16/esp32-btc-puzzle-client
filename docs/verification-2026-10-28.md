# Reliability and self-test verification — 2026-10-28 (initial revision)

This records the initial revision identified by the hashes below. The later walk-selftest extension is documented separately in `verification-walk-selftest-2026-10-28.md`.

## Implementation

- `search_core.h/.cpp`: shared Mbed TLS secp256k1 derivation/addition, random range sampling, HASH160 matching, Base58Check, and WIF. Errors are distinct from non-matches.
- `checkpoint_store.h/.cpp`: two-slot checksummed, sequence-numbered checkpoints. The previous valid slot remains intact during a write; read-back verifies the new slot.
- `found_records.h/.cpp`: puzzle-indexed found keys and pending-delivery flags, independent of the selected run. Supports recall and clearing one/all records.
- `run_stats.h`: aligned lifetime attempt/time counters with 32-bit clock deltas accumulated into 64 bits.
- `esp32-btcpuzzle.ino`: commands, platform adapters, HTTPS authentication, periodic 60-second random restarts, independent-random mode, and real randomized self-tests.

## Compiled host checks

Passed the shared-core and command-integration CTest suites with both normal host limbs and portable 32-bit Mbed TLS limbs. Also passed a clean dependency-fetch build using the documented CMake workflow.

The tests compile the production C++ files. The integration suite compiles the actual sketch against host serial/clock/filesystem adapters. There is no Python reproduction of the curve algorithm.

Coverage includes:

- 10,240 incremental point comparisons against independent OpenSSL multiplication.
- Key range sampling across IDs 1–160; all embedded target checksums; all 83 public solved-key vectors.
- Positive compressed/uncompressed matches, negative/error paths, and known WIF vectors.
- Actual fresh-random searches for the first ten puzzle targets.
- Interrupted checkpoint writes at each byte boundary and corruption at each envelope-byte location, recovering the previous checkpoint.
- Recovery after an interrupted first-ever checkpoint.
- Found-record reload, pending delivery, acknowledgment, clearing one/all, failed saves retained in RAM, and retry.
- Zero start timestamp, stop/start accounting, paused time, and millis rollover.
- Actual command handlers, result recall after reselect/reboot, 60-second random restarts, single/all self-tests, normal-run pause/resume, cancellation, diagnostic failure stopping the run, and old temporary-checkpoint migration.
- A nonblank filesystem mount failure must not format/erase existing data.

Address/undefined-behavior sanitizers instrumented project sources. Mbed TLS was built separately without sanitizer instrumentation; leak checking was disabled for these runs. Filesystem interruptions were simulated on the host, not induced by cutting hardware power during a flash write.

## ESP32-C3 USB serial checks

All device tests used `SERIAL_ONLY=1`; Wi-Fi and Telegram were disabled. Flash writes were verified. The ESP32-C3 USB-specific reset strategy worked when the ordinary reset strategy failed.

The old selected puzzle 72 and its persisted counters migrated successfully. `/selftest all` then performed real independent random sampling, consulted the expected keys only after discovering matches, and passed every target:

| Puzzle | Random attempts to match |
| --- | ---: |
| 1 | 1 |
| 2 | 1 |
| 3 | 1 |
| 4 | 3 |
| 5 | 13 |
| 6 | 30 |
| 7 | 158 |
| 8 | 25 |
| 9 | 996 |
| 10 | 730 |
| Total | 1,958 |

Fresh-random throughput was approximately 5.4 keys/s. These searches took about six minutes in total. At the end, normal puzzle 72 resumed. Its status changed from 472,192 to 472,193 normal attempts, while accumulated normal time remained 9,268 seconds: the diagnostic duration was excluded.

All ten discovered results were stored. Subsequent firmware updates/reboots preserved the record inventory and puzzle 10's key (`0x202`), WIF, and 730-attempt discovery count.

After the final storage/error guards were added, the final build additionally passed:

- `/clear-found 1`, followed by `/found 1` reporting no record.
- `/selftest 1`, which genuinely rediscovered key 1 in one independent random attempt and saved it again, despite the other nine records being present.
- `/status` retaining normal puzzle 72 and its counters.
- `/stop` then `/run` retaining an average of 51.98 keys/s on both replies, rather than resetting the numerator. This is a lifetime average including older firmware runs; it is distinct from current interval throughput.
- `/found 10` still returning the original stored key/WIF/count.
- A normal-search 30-second window at **70.35 keys/s** in minute-chunk mode.

A final bootloader-assisted hard restart passed: puzzle 72 resumed automatically, all ten found records were present, and both key 1 (cleared and rediscovered) and key 10 were recalled correctly. Raw serial transcripts are in the workspace's ignored `build/device-selftest.log`, `build/device-final-smoke.log`, and `build/device-final-reboot.log`.

## Final compile checks

Arduino ESP32 core 3.3.12, ArduinoJson 7.4.3. All four builds fit the default 1,310,720-byte application partition:

| Target | Mode | Sketch bytes |
| --- | --- | ---: |
| Nologo ESP32-C3 Super Mini | serial only | 403,442 |
| Nologo ESP32-C3 Super Mini | Wi-Fi/Telegram | 1,271,429 |
| ESP32-S3 (`hwcdc`, CDC on boot, OPI PSRAM) | serial only | 414,600 |
| ESP32-S3 (`hwcdc`, CDC on boot, OPI PSRAM) | Wi-Fi/Telegram | 1,177,979 |

The C3 has the final serial-only build installed. No S3 was connected for runtime testing.

## Remaining practical limits

Telegram send/poll code uses the built-in trusted CA bundle and hostname verification, requires synchronized time, and retains pending found notifications for retry. It was compile-checked on C3/S3; a live Wi-Fi/Telegram TLS session was not exercised because device tests were intentionally serial-only. Delivery is at least once, so a reset after Telegram accepts a message but before its acknowledgment is saved can produce a duplicate.

Run checkpoints can lose up to approximately ten seconds of recent counters on sudden power loss. Found records are saved immediately before delivery; failed writes are reported and retained in RAM for retry, so power must remain connected after a reported failure. Dual-slot tests establish recovery from interrupted writes under the tested storage model, not immunity to all physical flash damage.

Minute chunks are random-start sequential searches, not independent samples at each attempt and not a count of unique keys. Independent-random mode and every self-test use a fresh random candidate each time. None of these checks establishes exhaustive mathematical verification of all possible curve states.

Final sketch SHA256: `0e41e0a52ab4572549f2841351371bae5f90909c5122ac50a51f8c7e0c303ed8`.

Installed C3 application binary SHA256: `4e14bd4b2efe539e25be859c52966dde4c44e34e2087df1abe6b2b2d9ab9b1c6`.
