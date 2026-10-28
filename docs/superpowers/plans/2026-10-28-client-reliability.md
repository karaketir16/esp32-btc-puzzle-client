# Client reliability and testability implementation plan

Goal: preserve found keys independently of runs, recover checkpoints after interrupted writes, correct statistics, authenticate Telegram HTTPS, and exercise shared C++ search code on host and device.

User decisions: normal search uses random starts every 60 seconds; fresh independent random keys remain selectable. /selftest [1-10] always uses fresh random candidates and the real matching path.

- [x] Extract cryptography into search_core.h/.cpp; keep tested affine arithmetic and expose explicit Error/Miss/Match results. Build against Mbed TLS on host, compare with OpenSSL and public solved vectors.
- [x] Add shared run_stats.h with uint32_t clock deltas, a 64-bit accumulated duration, and matching lifetime attempt/time denominators. Test rollover and pause/resume.
- [x] Add checkpoint_store.h/.cpp with two checksummed sequence-numbered slots. Always write the inactive slot; recover the latest valid slot after truncation/corruption. Test interruption at every byte boundary, failed writes, and recovery.
- [x] Add found_records.h/.cpp independent of run state. Store key, compression, attempts, duration, and pending-delivery status per puzzle. Test reload, reselect, clear one/all, failed writes, and delivery acknowledgments.
- [x] Integrate /status, /found [ID], /clear-found [ID|all], /mode random|chunk, and /selftest [ID|all]; pause normal search during tests, retain genuinely discovered test keys, then resume. Migrate existing JSON checkpoints.
- [x] Enable Telegram CA bundle authentication and SNTP time synchronization; retain fixed chat authorization; persist delivery retries.
- [x] Run native compiled C++ tests; compile serial-only C3 and S3 plus default network build. Flash C3 serial-only; exercise normal found records, reboot recovery, clear, and selftest; restore selected normal puzzle afterward.
- [x] Document modes, performance tradeoff, test expectations, storage, TLS, and exact verification results.

No changes to credentials, no commit/push requested. Preserve staged pre-existing firmware changes.

## Follow-up: exercise walks in the device diagnostic

- [x] Default /selftest to 50-key / 2-second random-start chunks; retain explicit random mode.
- [x] Compare first increments, chunk endpoints, and matches against fresh multiplication; report reference-check and walk-step counts.
- [x] Add compiled host tests for key/time/range limits and deliberately mismatched points.
- [x] Rebuild C3/S3 variants, flash serial-only C3, run all ten walk diagnostics, and verify normal run resumes.
