# Job: Node request-serial 1024 round-trip latency (#3872, 0.33.0 #1639)

Worktree: /root/worktree/zlink-3872 (branch perf/3872-node-request; HEAD 8d0ce1278f has a test-only commit, keep it)
Report: write /root/jobs/fx/cx/v21/s033/nserial/report.md (absolute path; nowhere else)
Rules: no sub-agents or other models (no Astra). Measurements under `flock -o /root/jobs/fx/cx/measure.lock`;
builds and tests under `flock -s` on the same file. Commit locally; do not push.

## Background
Previous job: /root/jobs/fx/cx/v21/s033/nfix/report.md and /root/jobs/fx/cx/v21/s033/nprof/report.md.
Node Framework/binding: request-serial 1024 = 0.546 (target >= 0.6); the other three cells are 0.60-0.64.
request-serial sends one request, waits for its reply, then sends the next, so the ratio is set by
one round trip's latency on both processes.

## Task
1. Count, for one request-serial round trip (source call -> target handler -> reply -> source completion),
   every event-loop macrotask, microtask hop, setImmediate/nextTick, native callback and promise layer the
   Framework adds over the binding path. Use async_hooks or a temporary trace; remove it before commit.
   Give a table: step, file:function, Framework count, binding count.
2. Remove the hops that the spec does not require (spec root framework/doc/framework/common/spec/server/,
   01-execution/01 §11 new execution turn, 08 §3 E1-E5 and §4.1-§4.4). Keep the one new turn §11 requires.
   Add no new queue, flag or counter unless you remove an equivalent one; report before/after counts.
3. Measure request-serial 1024 and 4096 plus request-backpressure 1024, 4 rounds each, branch vs main,
   same-day binding medians; condition B pinning as in the previous job.
4. Done: full Node suite passes, `format.sh --check node` clean, report has the hop table, the A/B table,
   and keep/drop per change (drop anything that does not improve its target cell).
