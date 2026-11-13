Model gpt-6-luna, effort high. Earlier job briefs are history, not instructions.

# Task: #4350 measurement-server noise — short binding A/A after the scheduling fix.

Worktree /root/worktree/zlink-v21-s033/aa-noise (main 6d508d3166, Java bench already built there). No code changes, no commits, never push, no GitHub, no sub-agents or other AI tools, never run internal-docs.sh, never delete raw files.
Read first: /root/jobs/fx/cx/v21/s033/common-rules.md, aa-noise2/brief.md, the tools under /root/worktree/zlink-v21-s033/aa-noise/tools (run-aa-one.sh, capture-active.sh and the lock session script). Copy them into /root/jobs/fx/cx/v21/s033/aa-noise5/tools and change only what is listed here.

## Why
aa-noise2 found the bench processes ran SCHED_IDLE (inherited from the agent). The guard now sets the lock holder tree SCHED_OTHER every second (since 2026-11-13 16:29:25 UTC). This A/A tells whether the server is quiet enough for the quick method.

## Do
1. Check the Java bench JARs are newer than their sources; rebuild only if not.
2. One measure.lock hold, at most 15 minutes: Java binding (zlink-java) request-serial 4K and send-saturation 4K, A and B interleaved (A1 B1 A2 B2 ...), 5 runs each = 20 runs. Warmup 10 s, active 5 s, source CPU 0-7, target CPU 8-15.
3. Every run: right after the processes start and again mid-active, record the scheduling policy of EVERY thread of the source and target bench processes (chrt -p per /proc/<pid>/task/*) and the per-run environment that aa-noise2 recorded (CPU frequency samples, loadavg, top processes with class). If any thread is not SCHED_OTHER, mark that run invalid immediately and say so in report.md.
4. G5 spread per cell and per side = largest |run − median| ÷ median. A/B ratio of medians.

## Output
/root/jobs/fx/cx/v21/s033/aa-noise5/report.md (Korean, short tables, every number cites its raw file): per cell A median, B median, A/B, G5 A, G5 B, invalid runs, outliers with start time and what the environment shows. Raw under aa-noise5/raw. Time cap 1 h.

## Since aa-noise4 (kairos, 17:42 UTC)
- aa-noise4 (report aa-noise4/report.md, raw aa-noise4/raw): with C2/C3 off, request-serial 4K A 12,943 / B 13,082, G5 A 26.6% (one run A4 9,505), B 7.1%; send 4K A/B 0.996. Reuse aa-noise4/tools (copy into aa-noise5/tools).
- Guard changes since then: (1) a fork race that left new bench processes SCHED_IDLE at start is fixed; (2) CPU split during holds (PM decision): the measured process tree is restricted to CPUs 0-5 (source side) and 8-13 (target side) by the guard automatically — keep the tools' SOURCE_CPUS=0-7 / TARGET_CPUS=8-15, the guard narrows them; every other process and all movable IRQs run on 6-7,14-15.
- Per run additionally record: taskset -cp of the source and target bench processes (must be 0-5 and 8-13) and of one outside process (e.g. the codex agent: must be 6,7,14,15), and /proc/interrupts deltas per CPU over the active window.
- Do all 20 runs (5 A + 5 B per cell, interleaved), one hold ≤ 15 min. Report G5 per cell and side, and compare with aa-noise3 and aa-noise4 in one table.
