
- 2026-11-13T16:29:55Z kairos: measurement processes inherited SCHED_IDLE from their agent until the guard fix at 2026-11-13T16:29:25Z (now the guard sets the lock holder tree SCHED_OTHER every second). Any run whose active window began before 16:29:25Z ran at idle priority: mark it invalid in the report and re-run that cell in this hold if time allows (lad-java-m1: the first cell of this hold, L0 send-saturation 1K).

- 2026-11-13T16:31:39Z kairos (PM 16:32): common-rules.md has two new rules — SCHEDULING CHECK (record chrt -p of every bench thread at run start and mid-active; any thread not SCHED_OTHER = invalid run, re-run it) and PROVISIONAL WINDOW (all 12:05-16:29 UTC results are 잠정, pass and fail alike). Apply both from your next run.
