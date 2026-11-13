# Task: C++ request-backpressure 1KiB — the caller floor when the Framework skips encode/registration/C1 (measurement-only)

## Situation
Goal Framework ÷ binding ≥ 0.6 (never lowered): the single caller (submit) thread must be ≤ 6.6 µs/request; today ~17.5–17.9.
Two attempts removed Framework work and barely moved it: object trimming -0.87 µs (/root/jobs/fx/cx/v21/s033/ccall2/report.md),
deleting contract-allowed steps -0.44 µs (/root/jobs/fx/cx/v21/s033/cproto/report.md). Before any binding/Core contract change,
the PM wants the caller FLOOR: how fast the caller is when the Framework does none of its own request work and the call layer
calls the binding directly — split by layer. Caller-split method and stage mapping: /root/jobs/fx/cx/v21/s033/ccall/ (scripts/).
Binding caller ≈ 3.7 µs/request.

## Do (worktree /root/worktree/zlink-cfloor, main 2f8b8e635a; measurement-only; behaviour may be wrong; never commit; deleted after)
Build a ladder of variants, each a small env-var or compile switch, measured separately:
- V0: unmodified main.
- V1: Framework call layer (public channel call → request task) kept, but from the point where the Framework encodes/registers, call
  the binding request directly with a pre-encoded constant payload (skip typed encode, envelope/service framing, C1/C2 registration,
  operation owner turn); complete the public reply from the binding reply without decode.
- V2: like V1 but also bypass the Framework request_task/receiver objects: return the binding's own awaitable/result to the bench
  through the thinnest task wrapper the call API allows.
- V3: bench framework driver calling the binding request API directly (no Framework call layer at all), same payload and in-flight
  logic as the framework driver — this separates bench-driver cost from Framework layers.
For each, under `flock -o /root/jobs/fx/cx/measure.lock`, condition B, RUNS=3: caller µs (submit_thread_cores × 1e6 / throughput),
req/s, source process CPU µs/request. Then one profile per variant with the ccall method to split the caller into: bench driver,
Framework call layer, task/coroutine machinery, binding call itself, kernel/other.

## Build
Package + bench builds as in /root/jobs/fx/cx/ratio048p1/run.sh (local package build-wsl.sh cpp, then framework/bench/grpc
cpp/build.sh) with the CPU-pin patch applied (`git apply /root/jobs/fx/cx/ratio040/pin.patch`). C++ builds via
`/root/jobs/fx/cx/cpp-build-slot.sh ...` (no outer flock); other shared steps via `/root/jobs/fx/cx/measure-shared.sh`.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- No git operations except git apply/diff; no commits; no spec edits; no GitHub operations. Do not read Core C headers (core/include).
  Do not modify bindings/ or core/.
- MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &. Wait on locks if held.
- 리팩토링 점검: not applicable (measurement-only).
- Time box about 3 hours; report what is measured, mark the rest "측정 못 함".

## Output: /root/jobs/fx/cx/v21/s033/cfloor/report.md (Korean, plain sentences)
First table: variant | caller µs | req/s | source CPU µs. Then the per-layer split per variant. Then plainly: the caller floor with
Framework request work removed (V1/V2), how much of 6.6 µs it leaves, and which layer holds the rest.
