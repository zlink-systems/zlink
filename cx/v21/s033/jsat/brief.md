# Task: Java request-backpressure 1KiB — which thread is the bottleneck? (measurement-only, no product change)

## Situation
Java request-backpressure 1KiB (main b5254137b2): Framework 63,500 req/s vs binding 126,075 (0.504). The Framework caller (submit)
thread costs 11.7 µs/request, inside its 13.2 µs budget (1e6 / (0.6 × binding)), so the caller is not the limit; some other resource is.
The PM needs that resource named with numbers, to scope the next Java design. Background: /root/jobs/fx/cx/ratio050java/ (results.json
per run), /root/jobs/fx/cx/v21/s033/jcost/report.md and jcost2/report.md (earlier Java cost split, pump thread, wakes),
/root/jobs/fx/cx/v21/s033/cbind/report.md (C++ method: binding paced to the Framework's rate to compare at equal rate; per-request CPU
rises 4–5x at low rate, so compare at equal rate).

## Do
Worktree /root/worktree/zlink-jsat (main b5254137b2 with the CPU-pin patch; measurement-only; it will be deleted; never commit).
1. Add a pacing option (env var, default off) to the Java binding bench driver that spaces request submissions to a target rate with
   the same in-flight cap (same idea as cbind's ZLINK_CPP_BENCH_REQUEST_RATE). Bench files only.
2. Build (`/root/jobs/fx/cx/measure-shared.sh taskset -c 0-7 bash java/build.sh` from framework/bench/grpc). Under
   `flock -o /root/jobs/fx/cx/measure.lock`, condition B (target CPUs 8-15, source 0-7), request-backpressure 1024, RUNS=1 each:
   Framework unpaced; binding paced to the Framework's measured rate; binding unpaced.
3. For each run, per thread in both processes (source and target), over a ~10 s active window: CPU cores used (schedstat delta / wall),
   µs/request, voluntary/involuntary context switches per request, thread name and role (caller/submit, binding receive or I/O,
   completion/pump, Framework dispatcher/executor, GC/JIT, other). A thread at ≥0.9 cores is saturated. Also run-queue wait per thread
   if available (schedstat wait time) to show threads that are runnable but not running.
4. Answer plainly: which thread or resource limits Framework throughput (saturated thread, or a serial chain of handoffs whose
   latency × in-flight bounds the rate), its µs/request vs the same role in the paced binding, and the gap to what 0.6 (75.6k req/s)
   needs from it. Name the Framework code that runs on that thread (file:function, from a short async-profiler or JFR sample on that
   thread if needed).

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- No git operations; no spec edits; no GitHub operations. Do not read Core C headers (core/include). Do not modify bindings/ or core/.
- Only bench files may change, in this worktree only; list every file you edited.
- MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &. Wait on measure.lock if held.
- 리팩토링 점검: not applicable (measurement-only, no product change).
- Time box about 2 hours.

## Output: /root/jobs/fx/cx/v21/s033/jsat/report.md (Korean, plain sentences; per-thread table first)
