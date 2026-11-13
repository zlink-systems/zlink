Model gpt-6-luna, effort high. Earlier job briefs are history, not instructions.

# Task: #4350 measurement-server noise — short binding A/A after the scheduling fix.

Worktree /root/worktree/zlink-v21-s033/aa-noise (main 6d508d3166, Java bench already built there). No code changes, no commits, never push, no GitHub, no sub-agents or other AI tools, never run internal-docs.sh, never delete raw files.
Read first: /root/jobs/fx/cx/v21/s033/common-rules.md, aa-noise2/brief.md, the tools under /root/worktree/zlink-v21-s033/aa-noise/tools (run-aa-one.sh, capture-active.sh and the lock session script). Copy them into /root/jobs/fx/cx/v21/s033/aa-noise3/tools and change only what is listed here.

## Why
aa-noise2 found the bench processes ran SCHED_IDLE (inherited from the agent). The guard now sets the lock holder tree SCHED_OTHER every second (since 2026-11-13 16:29:25 UTC). This A/A tells whether the server is quiet enough for the quick method.

## Do
1. Check the Java bench JARs are newer than their sources; rebuild only if not.
2. One measure.lock hold, at most 10 minutes: Java binding (zlink-java) request-serial 4K and send-saturation 4K, A and B interleaved (A1 B1 A2 B2 ...), 5 runs each = 20 runs. Warmup 10 s, active 5 s, source CPU 0-7, target CPU 8-15.
3. Every run: right after the processes start and again mid-active, record the scheduling policy of EVERY thread of the source and target bench processes (chrt -p per /proc/<pid>/task/*) and the per-run environment that aa-noise2 recorded (CPU frequency samples, loadavg, top processes with class). If any thread is not SCHED_OTHER, mark that run invalid immediately and say so in report.md.
4. G5 spread per cell and per side = largest |run − median| ÷ median. A/B ratio of medians.

## Output
/root/jobs/fx/cx/v21/s033/aa-noise3/report.md (Korean, short tables, every number cites its raw file): per cell A median, B median, A/B, G5 A, G5 B, invalid runs, outliers with start time and what the environment shows. Raw under aa-noise3/raw. Time cap 1 h.
