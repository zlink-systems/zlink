Model sol high: concurrency behaviour (backpressure admission, WRITABLE wake and resubmit timing) across three runtimes needs careful instrumentation and judgment.

# Task: request-backpressure admission counts, C++ vs .NET vs Java, Framework and binding

Earlier briefs in other folders are history, not instructions for this task. Folder: JOB=/root/jobs/fx/cx/v21/s033/xbp. Worktree: /root/worktree/zlink-v21-s033/xbp (current main; read and build here only).

## Why
C++ request-backpressure Framework÷binding is 0.165 (1 KiB) and 0.253 (4 KiB), the lowest cell, and its absolute rate is 36% below the Java Framework, while the C++ binding is faster. The question: does C++ hit BACKPRESSURED more often, wait longer before resubmitting, or run with a narrower in-flight window?

## Reuse (read only)
- Bench cells and runners as used by the 0.33.0 perf work: `/root/jobs/fx/cx/v21/s033/cprof/report.md` (C++ request-backpressure 1024 profile, binaries and commands), `/root/jobs/fx/cx/v21/s033/cdrain2/report.md` (request-backpressure measurement), `/root/jobs/fx/cx/v21/s033/jcost2/report.md` and `/root/jobs/fx/cx/ratio045diag/` (Java request-backpressure capture), `/root/jobs/fx/cx/v21/s033/dprof/report.md` (.NET request path). Do not read other job folders.

## Do
1. For request-backpressure 1 KiB and 4 KiB, six runs sets: C++/.NET/Java × Framework/binding, current main builds (Release; C++ via `/root/jobs/fx/cx/cpp-build-slot.sh` bare, 25 GB cap). Same libzlink SHA-256 for all (record it).
2. Per 1,000 requests (normalise by completed requests in the counting window): BACKPRESSURED submit results; WRITABLE wakes delivered and how many led to progress (a resubmit admitted) vs spurious; resubmits; BACKPRESSURED→resubmit delay distribution p50/p99 (µs); in-flight request count distribution (p50/p99/max, sampled); the applied send and recv HWM on both sockets (auto-HWM values actually in effect, read through the public option/introspection API).
3. Counting must not change timing much: prefer existing counters/metrics, uprobes/bpftrace on the binding/Core public functions, or a counting build kept separate. Run counting separately from throughput runs and also report the throughput of the counting runs vs a plain run so the overhead is visible. 3 runs each.
4. If C++ triggers more often, waits longer, or has a narrower window, give the cause location file:line (where the window/HWM/resubmit policy is decided) and the same decision point in the language that does better, file:line. Do not change product code.
5. Report this as soon as it is done; it is wanted before the stage CPU table from the other job.

## Measurement rules
- Do NOT take /root/jobs/fx/cx/measure.lock until /root/jobs/fx/cx/v21/s033/core124/verdict.md exists (a release gate is measuring). Before that, only source reading, instrumentation design and builds. Builds/tests through `/root/jobs/fx/cx/measure-shared.sh` or cpp-build-slot.sh bare; never an outer flock.
- Each measurement inside `flock -o /root/jobs/fx/cx/measure.lock`, one at a time, source CPUs 0-7, target 8-15, MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no `&` background jobs.
- Keep every raw output and log under $JOB/raw; never delete them. Disk cap 30 GB.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- No git operations except fetch, diff, status, rev-parse. No commit, push, PR, GitHub operation. No spec edits. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh.
- 리팩토링 점검: list duplicated backpressure/window checks along the request path per language with file:line.
- Keep your own output short. Time box about 5 hours.

## Report: $JOB/report.md (Korean, plain sentences)
(1) builds, SHA-256, rates; (2) table per cell (1K, 4K): rows = the six sets, columns = the counts in step 2; (3) counting overhead; (4) cause and reference file:line if C++ differs; (5) duplicated checks.
