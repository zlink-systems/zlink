# G-7 gate progress

- 2026-09-07: G-7 mailbox patch applied cleanly; release lib-only and dev builds completed with JOBS=4.
- 2026-09-07: Starting the required full dev ctest run, then the focused repeat and performance gates.
- 2026-09-07: Full dev ctest is still running foreground (`-j2`); no gate failure has been observed yet.
- 2026-09-07: The full suite remains active; G-7 follows the required `-j2` limit for the known request-timeout scheduler intermittent test.
- 2026-09-07: Full ctest completed without a recorded failure. The required 56-test focused suite is now in its five-run foreground repeat.
- 2026-09-07: All five focused-suite repetitions completed. Lost-wake repeat sets (`until-fail:20`) are running foreground next.
- 2026-09-07: The lost-wake and additional repeat checks remain active. `test_wake_invariants` has a roughly 30-second run duration, so its required 20 repetitions are intentionally long; no failure has been recorded.
- 2026-09-07: All requested repeat, close, stream/pipe, hotpath, mirror, and local-release with_stream gates completed; summary written.
