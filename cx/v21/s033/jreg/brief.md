Model gpt-6-luna, effort high: find the commit that dropped Java Framework request-serial. Earlier job briefs are history, not instructions.

# Task: Java Framework request-serial 1 KiB fell from ~9,300 to ~5,600 msg/s

Worktree /root/worktree/zlink-v21-s033/jreg (detached at d3fd955131). Switch commits with `git checkout --detach <sha>` inside it; do not commit, do not push.

## Evidence (supervisor)
- Before: /root/jobs/fx/cx/v21/s033/i3871/raw/round1/zlink-framework-java-request-serial-1024/report.txt = 9,309.2 msg/s, results.json "commit" 76a4d78506; binding zlink-java 11,976-12,567 (i3871/raw).
- After: /root/jobs/fx/cx/v21/s033/r4190m/raw/bench/java/run{1,2,3}/zlink-framework-java-request-serial-1024/report.txt = 5,641 / 4,808 / 5,648 at d3fd955131; binding 13,720-13,901. Framework source CPU 201 µs/msg (r4190m report Java CPU table).
- Java-relevant commits in 76a4d78506..d3fd955131: d3fd955131 (#4245 bench: per-stream dedicated submit thread), 741a58bae2 (#4220), e68d66fb0d (#4211 lease admission deadline read by execution decision), 94a1a70037, 34a2a21dbe (#4199), 79eda2a1d5 (#4182 session bind deadline), 93efd903a4 (Core 1.24.0 inputs).

## Do
1. Measure zlink-framework-java request-serial 1024 and zlink-java request-serial 1024, 5 judged runs each (Framework varied 15% in r4190m; if a commit's spread still exceeds 10%, add runs until the median is stable and say how many), same runner and conditions as r4190m (java/run_local.sh as r4190m used it; see r4190m/report.md and raw/*-driver.log for the exact command and env), at: 76a4d78506, 741a58bae2 (= d3fd955131 without the bench change), d3fd955131. Use the SAME Core prefix for all (/root/.cache/zlink/core-release/1.24.0/linux-x64) — note whether 76a4d78506 builds against it; if not, say what you used.
2. Whichever step drops: bisect inside it (product commits one by one) until one commit owns the drop. If it is the bench change, find which part (thread handoff for the serial pattern, sequence/metrics merge) with file:line and a per-request CPU comparison.
3. Also measure current main head c820f2d3ba19f828b7ade0c15430250b2be11486 (contains #4238, 08:56 UTC, which changed the same hot path ZLinkSpotRuntime/ZLinkSpotActivation) with the same 5-run protocol and put it in the table.
4. Root cause with file:line and, if it is product code, a short JFR/async-profiler or counter comparison before/after on the hot path. No fix yet; propose one.

## Measurement conditions
`flock /root/jobs/fx/cx/measure.lock` EXCLUSIVE for runs only (builds take it shared through the build slot / gradle as r4190m did); cpu0-15 governor performance inside the lock, powersave restored on exit (trap); placement B (source 0-7, target 8-15); uptime/load, free memory, cpu0-15 scaling_cur_freq before/after each cell. Waiting for the lock or measure gate is normal; wait. Native compiles only through /root/jobs/fx/cx/cpp-build-slot.sh. Measure each commit back to back so the conditions match.

## Rules
- Do NOT run scripts/dev/internal-docs.sh, any git pull/fetch/push, GitHub, sub-agents or other AI tools.
- Keep every raw file in /root/jobs/fx/cx/v21/s033/jreg/raw (never delete). Build outputs inside the worktree.
- Korean, short: 표(commit × binding/Framework msg/s, F/B, source CPU µs/msg), 원인 commit, 원인 file:line, 근거, 제안.

## Output: /root/jobs/fx/cx/v21/s033/jreg/report.md
