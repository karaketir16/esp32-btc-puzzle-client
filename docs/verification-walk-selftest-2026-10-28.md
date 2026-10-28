# Walk self-test verification — 2026-10-28

This extends the earlier reliability revision. `/selftest [1-10|all] [walk|random]` now defaults to walk mode. Explicit independent-random mode remains available. Normal searching still uses its separately configured 60-second chunks.

A diagnostic walk chunk has a maximum of 50 checked keys and a roughly two-second wall-clock bound, checked between operations. The upper range boundary also ends a chunk. New chunks choose fresh random starting keys; the known answer is not used to seed the search.

The walk uses the production `btc::Crypto::increment()` implementation. A separate `btc::Crypto` instance derives reference public points by scalar multiplication. The diagnostic compares the first increment of each chunk, its final point (including a pending point when its deadline expires), and every matching point. Known private keys are checked only after an address match. This is sampled on-device point validation, not a direct reference comparison at every intermediate step. Host differential tests compare 10,240 individual incremental points against OpenSSL.

Each pass reports attempts, actual incremental additions, reference comparisons, and random chunks. Reference comparisons can revisit a point and are not unique-point counts. Puzzle 1 has a singleton range and cannot perform an in-range increment; its walk-step count is zero.

## Compiled host verification

Both shared-core and actual-sketch integration suites passed with normal host limbs and portable 32-bit Mbed TLS limbs, with project-source address/undefined-behavior sanitizers enabled. The separately built Mbed TLS library was not sanitizer-instrumented.

Additional integration tests exercise:

- Explicit walk and independent-random commands, and walk as the default.
- Single/all puzzle completion while preserving the normal run and stored discoveries.
- An exactly 50-candidate no-match chunk ending with 49 increments and two reference checks.
- A fresh random restart on the next candidate after that cap.
- An expired two-second deadline causing a fresh random chunk before the key cap.
- The puzzle upper boundary ending a chunk without incrementing outside the range.
- A deliberately mismatched current key/public point: the diagnostic detects the discrepancy and stops itself and the normal run.

These tests compile the production sketch against host platform adapters; there is no separate transcription of the controller or curve arithmetic.

## Build checks

Arduino ESP32 core 3.3.12, ArduinoJson 7.4.3. All variants fit the default 1,310,720-byte app partition:

| Board | Build | Bytes |
| --- | --- | ---: |
| Nologo ESP32-C3 Super Mini | serial-only | 406,104 |
| Nologo ESP32-C3 Super Mini | Telegram | 1,274,061 |
| ESP32-S3, hwcdc/CDC/OPI | serial-only | 416,628 |
| ESP32-S3, hwcdc/CDC/OPI | Telegram | 1,180,887 |

The C3 serial-only application was flashed with hash verification using the USB-specific reset strategy. Its existing normal puzzle 72 and all ten previously stored discoveries survived.

## Device verification

The actual C3 command `/selftest all walk` passed through `tests/serial_selftest.py --id all --mode walk`. Raw serial output is retained in `build/device-walk-selftest.log`.

The diagnostics deliberately re-search even when a saved result exists. They retain the earlier discovery record instead of replacing it. A passing known-answer match alone is separate from the reference-point checks reported alongside it.

| Puzzle | Checked keys | Walk steps | Reference checks | Random chunks |
| --- | ---: | ---: | ---: | ---: |
| 1 | 1 | 0 | 1 | 1 |
| 2 | 2 | 1 | 2 | 1 |
| 3 | 4 | 3 | 2 | 1 |
| 4 | 3 | 0 | 3 | 3 |
| 5 | 17 | 13 | 7 | 4 |
| 6 | 16 | 15 | 2 | 1 |
| 7 | 5 | 4 | 2 | 1 |
| 8 | 85 | 83 | 4 | 2 |
| 9 | 269 | 261 | 16 | 8 |
| 10 | 4124 | 4036 | 176 | 88 |
| Total | 4526 | 4416 | 215 | 110 |

No reference mismatches occurred. Puzzle 10 ran at 39.72 checked keys/s including diagnostic reference overhead. Total wall time was approximately two minutes in this particular randomized run; solve times are probabilistic and chunk sampling has a different distribution from independent random sampling.

Normal puzzle 72 automatically resumed: its normal attempt count went from 558,706 to 558,707 while accumulated normal time stayed at 10,530 seconds. All previous found records remained present; `/found 10` retained the earlier discovery and its 730-attempt count. The completed diagnostic did not use that record to skip searching.

The two-second deadline and 50-key cap were both forced in host tests. This hardware run exercised key-count/range restarts; it does not claim that its wall-clock deadline was hit. Existing rate, persistence, TLS, and hardware-testing limitations from the initial report still apply.

Sketch SHA256: `cf50057dd7e0f9e87c0e75c924b2b733f7cd4fc3fc78246570c00d50e77332e8`.

Installed C3 application SHA256: `1d35ed2d01d3f64cb9e8ffa30820cb672e0cdc06665d53c502cdfdd6ef934388`.
