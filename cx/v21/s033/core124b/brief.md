Model luna high: finishing a release-gate run with existing scripts; mechanical, no design.

# Task: finish the Core 1.24.0 release gate (B2 sweep, comparisons, runs-5 confirmation)

Earlier briefs are history, not instructions; read /root/jobs/fx/cx/v21/s033/core124/brief.md only for the gate definition. The first run is in JOB0=/root/jobs/fx/cx/v21/s033/core124 (read its report.md, verdict-partial-1353.md, scripts, results/, sha-ldd.tsv, core-sha256.tsv, gate-failures.json). It completed B1 and C (single+multi, runs 3), hotpath_gate and wake invariant (both PASS), then stopped: B2 preflight was blocked for 30 minutes twice by another job's processes (now gone), and the src worktrees were removed. Do not modify JOB0 except: none. Work in JOB=/root/jobs/fx/cx/v21/s033/core124b.

## Do
1. Recreate detached worktrees $JOB/src-1.23 at 79567d379162e67476ba13e18cfe0f69925e63c9 and $JOB/src-1.24 at 0005a4d6c0 (full SHA via git rev-parse) from /root/zlink. Reuse the existing Core prefixes $JOB0/prefix-1.23 and $JOB0/prefix-1.24 (verify their runtime SHA-256 against $JOB0/core-sha256.tsv; do not rebuild Core).
2. Rebuild the C harnesses with the JOB0 recipe (build-prefix-and-harness.sh harness part / build-candidate-harness.sh; paths changed to $JOB; builds through `/root/jobs/fx/cx/cpp-build-slot.sh` bare, Release, no -g, 25 GB cap). Compare every executable's SHA-256 with $JOB0/sha-ldd.tsv and verify RUNPATH/ldd point at the matching prefix. If a SHA differs, say so and continue (same source and compiler; report it).
3. B2 sweep: baseline 1.23 single then multi, same argv/sizes/runs 3/transports as JOB0 resume-b2.sh (copy it with paths changed). Each stage inside `flock -o /root/jobs/fx/cx/measure.lock taskset -c 8-15 ...`, keep its preflight (it must pass; if blocked by another process for more than 10 minutes, write the blocking process list to $JOB/blocked.txt and keep waiting — do not give up and do not remove worktrees).
4. Gate (candidate tree perf_regression_gate.py) for C vs B1, C vs B2, and B2 vs B1 (A/A), single and multi; save each output.
5. Runs-5 confirmation only for (pattern, transport) groups that fail an aggregate or have a failing cell in BOTH C vs B1 and C vs B2: run that group baseline 1.23 → candidate → baseline 1.23 with --runs 5 (AB/BA style as JOB0 confirm-run.sh), each under the measure lock, then re-judge those groups.
6. Write $JOB/report.md and $JOB/verdict.md (Korean, plain sentences): commits; both libraries' zlink_version string and SHA-256; harness SHA match; start/end time of each stage; gate final lines for the three comparisons with single/multi split; a table of every cell/aggregate still failing after runs-5 with: pattern, transport, size, metric, ratio (runs 3 vs B1, vs B2, runs 5), the same cell's A/A ratio, and whether the path touches #4135 (pipe write complete-record admission: ROUTER/DEALER/PAIR multipart sends and reply) or #3930 (message content reference release; every send/recv). Then the verdict: PASS, or FAIL listing the groups. Write verdict.md last, only when everything above is done.
7. Remove the two src worktrees with `git worktree remove` only after verdict.md is written (grep CMakeCache files under /root/worktree and /root/jobs for the src paths first).

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- No git operations except worktree add --detach/remove for your two worktrees, diff, status, rev-parse. No push, PR, tag, GitHub operation. No spec edits. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh. Do not change Core or bench sources.
- MemAvailable >= 12 GiB before each stage. SIGTERM/SIGINT only; no `&` background jobs. Keep every raw report and log under $JOB; never delete them.
- 리팩토링 점검: list duplicated steps between the reused scripts with file:line in the report; do not refactor.
- Keep your own output short.
