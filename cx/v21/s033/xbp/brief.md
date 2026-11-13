Model sol high: concurrency behaviour (backpressure admission, WRITABLE wake and resubmit timing) across three runtimes needs careful instrumentation and judgment.

# Task: request-backpressure admission counts, C++ vs .NET vs Java, Framework and binding

Earlier briefs in other folders are history, not instructions for this task. Folder: JOB=/root/jobs/fx/cx/v21/s033/xbp. Worktree: /root/worktree/zlink-v21-s033/xbp (current main; read and build here only).

## Why
C++ request-backpressure Framework÷binding is 0.165 (1 KiB) and 0.253 (4 KiB), the lowest cell, and its absolute rate is 36% below the Java Framework, while the C++ binding is faster. The question: does C++ hit BACKPRESSURED more often, wait longer before resubmitting, or run with a narrower in-flight window?

## Reuse (read only)
- Bench cells and runners as used by the 0.33.0 perf work: `/root/jobs/fx/cx/v21/s033/cprof/report.md` (C++ request-backpressure 1024 profile, binaries and commands), `/root/jobs/fx/cx/v21/s033/cdrain2/report.md` (request-backpressure measurement), `/root/jobs/fx/cx/v21/s033/jcost2/report.md` and `/root/jobs/fx/cx/ratio045diag/` (Java request-backpressure capture), `/root/jobs/fx/cx/v21/s033/dprof/report.md` (.NET request path). Also `/root/jobs/fx/cx/v21/s033/ctrecv/scripts/collect.py` and `/root/jobs/fx/cx/v21/s033/ccall2/scripts/collect.py` (per-thread schedstat collection). Do not read other job folders.

## Do
1. For request-backpressure 1 KiB and 4 KiB, six runs sets: C++/.NET/Java × Framework/binding, current main builds (Release; C++ via `/root/jobs/fx/cx/cpp-build-slot.sh` bare, 25 GB cap). Same libzlink SHA-256 for all (record it).
2. Per 1,000 requests (normalise by completed requests in the counting window): BACKPRESSURED submit results; WRITABLE wakes delivered and how many led to progress (a resubmit admitted) vs spurious; resubmits; BACKPRESSURED→resubmit delay distribution p50/p99 (µs); in-flight request count distribution (p50/p99/max, sampled); the applied send and recv HWM on both sockets (auto-HWM values actually in effect, read through the public option/introspection API).
3. Counting must not change timing much: prefer existing counters/metrics, uprobes/bpftrace on the binding/Core public functions, or a counting build kept separate. Run counting separately from throughput runs and also report the throughput of the counting runs vs a plain run so the overhead is visible. 3 runs each.
4. If C++ triggers more often, waits longer, or has a narrower window, give the cause location file:line (where the window/HWM/resubmit policy is decided) and the same decision point in the language that does better, file:line. Do not change product code.
4b. The decisive question (대표님 2026-11-10): is the low C++ request-backpressure Framework÷binding caused by backpressure, or by Framework CPU per request? Answer yes or no with numbers:
   - On the C++ Framework caller (submit) thread and the completion thread: busy fraction (CPU time / wall time) and wall time spent waiting on BACKPRESSURED/WRITABLE or on the in-flight window, per request; the same for the C++ binding and for .NET and Java Framework.
   - Window experiment, separate runs: C++ Framework and C++ binding with the send/recv HWM and the client in-flight window raised (e.g. ×4) and lowered (÷2), 3 runs each; report throughput change. If throughput moves with the window while the caller is not CPU-saturated, backpressure is limiting; if the caller thread is near one full core and throughput does not move, CPU is limiting.
   - Share of the C++ F÷B gap (to 0.6) explained by backpressure waiting vs by CPU per request.
4c. Core I/O thread cost (PM 2026-11-10; design: /root/jobs/fx/cx/v21/s033/xbp/input/pm-design.md §1.5 and §6.1 — read those two sections). Hypothesis: on the C++ Framework path the Core I/O thread spends about 18.8 µs per message (target I/O1 0.813 core at ~43.2k msg/s in the earlier ccall2 run) versus ≤4.0 µs on the binding path, which alone would cap Framework near 53k msg/s. Cells: C++ request-backpressure 1 KiB and 4 KiB, send-saturation 1 KiB; Framework and binding.
   - Per-thread CPU for every thread of source and target (identify the Core I/O threads by name/tid), from /proc/<pid>/task/<tid>/schedstat sampled over the active window, the way /root/jobs/fx/cx/v21/s033/ctrecv/scripts/collect.py and ccall2/scripts/collect.py do (read them; copy what you need into this folder). Run Framework and binding paced to the SAME message rate (choose a rate both sustain; also report the unpaced runs), 3 runs each. Report µs per message per thread, I/O threads first; say yes/no on the hypothesis with the numbers.
   - Native frames and bytes per message on the wire, both directions, Framework vs binding (count with uprobes on the binding public send/receive functions or a counting build; say which).
   - K_c': Framework completions handled per completion-dispatcher wake, and dispatcher wakes per 1,000 requests (count like offload_executor.cpp:108,334 does, in a counting build kept separate from timed runs).
   - This is the "before" point (current main, before #1639 steps 1-7). Write the procedure as a script in this folder so the same measurement can be re-run after the steps.
5. Report this as soon as it is done; it is wanted before the stage CPU table from the other job.

## Measurement rules
- Do NOT take /root/jobs/fx/cx/measure.lock until /root/jobs/fx/cx/v21/s033/core124b/verdict.md exists (a release gate is measuring). Before that, only source reading, instrumentation design and builds. Builds/tests through `/root/jobs/fx/cx/measure-shared.sh` or cpp-build-slot.sh bare; never an outer flock.
- Each measurement inside `flock -o /root/jobs/fx/cx/measure.lock`, one at a time, source CPUs 0-7, target 8-15, MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no `&` background jobs.
- Keep every raw output and log under $JOB/raw; never delete them. Disk cap 30 GB.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- No git operations except fetch, diff, status, rev-parse. No commit, push, PR, GitHub operation. No spec edits. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh.
- 리팩토링 점검: list duplicated backpressure/window checks along the request path per language with file:line.
- Keep your own output short. Time box about 5 hours.

## Report: $JOB/report.md (Korean, plain sentences)
(1) builds, SHA-256, rates; (2) table per cell (1K, 4K): rows = the six sets, columns = the counts in step 2; (3) counting overhead; (4) cause and reference file:line if C++ differs; (4b) the yes/no answer with busy fraction, wait time, window experiment and gap share, placed first in the report; (4c) Core I/O thread µs/message per thread (F vs B at equal rate, I/O threads first) with the yes/no on the 18.8 µs hypothesis, frames and bytes per message, K_c' and dispatcher wakes per 1,000 requests, and the re-run script path — placed right after (4b); (5) duplicated checks.
