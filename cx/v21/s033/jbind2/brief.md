Model gpt-6-luna, effort high: rerun of an A/B measurement with pinned setup; no design or code change.

# Task: rerun the Java binding main vs PR #4073 measurement under stable conditions (#4060)

## Situation
Job jbind (/root/jobs/fx/cx/v21/s033/jbind/report.md; scripts, scratch bench copies jbind/main and jbind/pr, and jar hashes there) measured zlink-java request-serial, request-backpressure, send-saturation 1 KiB, main 6b24e01426 vs PR b42f978451, 5 runs each, taskset source 0-7 / target 8-15. Supervisor finding: throughput stepped down ~30-40% on both sides between run 2 (00:07:53) and run 3 (00:08:19), so the runs were not taken under equal conditions; cells were only 2 s warmup + ~8 s active. Time-adjacent pairs showed request-backpressure lower on the PR in 4 of 5 pairs (pair median -3.1%), which needs a clean answer.

## Do
1. Reuse jbind's scratch bench copies and built jars as they are if their jar SHA-256 still matches jbind/report.md (main 30c6dd5b..., PR df0ac6f5...); rebuild only if not.
2. Before each run record `uptime`, the top 10 processes by CPU (`ps -eo pid,pcpu,comm --sort=-pcpu`), and CPU frequency (`grep MHz /proc/cpuinfo | sort | uniq -c` or cpupower if present). Do not start a run while the 1-minute load average is above 4; wait (poll every 30 s, up to 20 minutes, then run and flag it).
3. Cells: warmup 10 s, active 10 s (state how you set this in the runner copy), RUNS=6 per side as adjacent pairs with alternating order (main,PR),(PR,main),... Same taskset pinning and affinity check as jbind. Each pair inside one `flock -o /root/jobs/fx/cx/measure.lock` hold.
4. Report per cell: all values, medians, and the per-pair PR/main ratio with the median and the min-max of the six pair ratios. A difference counts only if all six pair ratios have the same sign or the pair-ratio median is outside the spread of main's own adjacent-run ratios; state which.
5. If a step change shows up again, find what ran at that moment (from your per-run process lists) and report it.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Do not read other job folders under /root/jobs/fx/cx/v21/s033 except jbind/; write only in jbind2/ (copies of jbind scripts go here).
- Do not edit tracked files in any worktree. No git operations except diff/status/log/rev-parse. No GitHub operations; no spec edits. Do not read core/include. Do not run scripts/dev/internal-docs.sh.
- MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &. Keep your own output short.
- Time box about 2 hours.

## Output: /root/jobs/fx/cx/v21/s033/jbind2/report.md (Korean, plain sentences): (1) setup and hashes; (2) per-run load/process/frequency summary; (3) table per cell with values, medians, pair ratios; (4) verdict per cell.
