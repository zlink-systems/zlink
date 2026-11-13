# Task: C++ request caller — measurement-only prototype that deletes the steps the current contract allows (no PR)

## Situation
Goal: Framework ÷ binding ≥ 0.6 (never lowered). C++ request-backpressure 1KiB: the single caller (submit) thread limits throughput;
budget 6.6 µs/request, today 16.6 µs. The design study /root/jobs/fx/cx/v21/s033/cdes/report.md (read all of it) found no design
inside the current binding contract that reaches 6.6, and listed per-request steps that the current contract allows to delete
(class C) but whose µs were never measured. The PM approved measuring them by actually deleting them in a throwaway copy:
the result must say "caller µs reachable inside the current contract" and "what remains to 6.6".
Do not repeat rejected work: object/allocator trimming (PR 1, /root/jobs/fx/cx/v21/s033/ccall2/report.md) bought 0.87 µs only —
no intrusive owners, pools or arenas.

## Do
Worktree /root/worktree/zlink-cproto (main 2f8b8e635a; measurement-only; it will be deleted; never commit, never push).
Delete these per-request steps (cdes report "리팩토링 점검과 삭제 목록" and the step table rows marked C), keeping every
observable behaviour and the spec (cdes lists the clauses: S01 §9–11, S08 §4.4 and :68-70/:96-113, lane §5, async-execution-model
:117-122, S05:218-234):
1. Transport name/variant lookup and declared-type serializer selection per request → resolve once per bind/first use
   (channel_runtime.cpp:1750-1766, channel.hpp:910-916; reuse the existing bounded serializer cache, no new cache).
2. TLS header-plan matching and static fragment selection per request → bind/route codec plan (envelope_codec.cpp:323-372,425-440).
3. Building a JSON header message and then re-packing it into the service payload → write the canonical bytes once into the final
   service frame (client_call_codec.cpp, service_wire_codec.cpp:3114-3180). Wire bytes must stay byte-identical.
4. The value-conversion chain raw bool admission → host submit_result task → mesh observer → admitted
   (raw_mesh_node_owner.cpp:413,1585-1586, public_host_runtime.cpp:3390-3431, mesh_node_runtime.cpp:4417-4490): connect the binding's
   admitted stage to the final admitted directly.
5. Pure forwarding layers on the caller path (mesh → public host → raw owner) where a layer only forwards.
Keep C1/C2/typed delivery/admission decision owners one each; add no flag, counter, queue, poller or cache that is not replacing one.
Work step by step and keep a per-step build so each step's effect can be measured.

## Build, test, measure
- Builds: `/root/jobs/fx/cx/cpp-build-slot.sh cmake --build --preset linux-ninja-release --parallel 3 -- -k 0` (no outer flock).
- After all steps: the C++ unit + contract tests must pass (ctest in the release preset, under `/root/jobs/fx/cx/measure-shared.sh`);
  wire byte-identity: compare encoded request bytes before/after for the bench request (write a small check).
- Measurement: apply /root/jobs/fx/cx/ratio040/pin.patch (git apply) for the runs; package + bench build as in
  /root/jobs/fx/cx/ratio048p1/run.sh (local package build-wsl.sh cpp, then framework/bench/grpc cpp/build.sh), then under
  `flock -o /root/jobs/fx/cx/measure.lock`, condition B: zlink-framework-cpp request-backpressure 1024, RUNS=3, for
  (a) the unmodified main build and (b) the final prototype; if time allows, (c) after steps 1–2 only and (d) after 1–4.
  Caller µs = submit_thread_cores × 1e6 / throughput_per_second (results.json). Also report request-serial 1024 round trip for (a)/(b).

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- No git operations except `git apply` of the pin patch and `git diff` to save your diff; no commits; no spec edits; no GitHub
  operations. Do not read Core C headers (core/include). Do not modify bindings/ or core/.
- MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &. Wait on locks if held.
- 리팩토링 점검: list each deleted mechanism and confirm no new flag/counter/queue without a removal.
- Save the final diff to /root/jobs/fx/cx/v21/s033/cproto/prototype.diff.
- Time box about 4 hours; report whatever is measured, mark the rest "측정 못 함".

## Output: /root/jobs/fx/cx/v21/s033/cproto/report.md (Korean, plain sentences)
First table: build | caller µs/request | req/s | Δ caller vs main. Then: "현행 계약 안 도달 caller µs" and "6.6까지 남은 몫",
per-step µs, test and wire-identity results, files changed.
