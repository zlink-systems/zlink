Model gpt-6.1-sol, effort high: this needs a concurrency judgement on close versus an in-flight native reply, a C++ build, a race test and timing measurements.

# Task: C++ Framework reply submitted outside the port mutex, with close kept correct (#3867 follow-up)

## Situation
Read /root/jobs/fx/cx/v21/s033/creply/report.md fully, and reuse its runner/, tools/ and measurement scripts (do not rebuild what it already built unless the source changed).
It found: framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp:506 holds the socket mutex across the native reply submit; 130,530 of 1,200,000 replies waited on it (0.883 µs/request); the Framework sends 0.886 reply data packets per request vs binding 0.683-0.693 at 40k req/s.
Its unverified proposal is /root/jobs/fx/cx/v21/s033/creply/prototype.diff: keep the mutex while building the binding operation, unlock before `.submit()`.
Supervisor review found a gap: once submit runs outside the mutex, the port owner can destroy the binding socket while a submit is in flight. bindings/cpp/src/Runtime/Sockets/socket.cpp:53 (`socket_t::~socket_t`) ignores the close result, and Core returns EBUSY for close while an admitted API is in flight on the same handle (core/doc/spec/core/socket/README.ko.md §2). The native socket would then stay open.
Java fixed the same case like this (PM decision): reply checks once whether close has started and rejects it if so; the closing side retries a BUSY close every 1 ms, at most 8 times, then reports the failure through the existing contract. See the Java diff /root/jobs/fx/cx/v21/s033/jdr4/change.diff for the shape and its tests.
Worktree: /root/worktree/zlink-3867-creply2 = main a0056577c4 + CPU-pin patch.

## Do
1. Find every place that ends the C++ binding socket used by raw_route_port_t (explicit close and destruction, file:line). Say whether each can run while a reply submit is in flight after the prototype unlock.
2. Apply prototype.diff. At the closing side, close the binding socket explicitly before destruction, retrying close_error_t BUSY every 1 ms at most 8 times, then the existing failure path. Do not add a new lock, queue, counter, flag or state; the existing `_socket == nullptr` check under the mutex is the "close has started" check.
3. Tests (framework C++ test tree): (a) 100 repetitions of continuous replies racing port close; each close must succeed and the native socket must be closed afterwards; (b) a deterministic test that holds one reply in submit, sees close get BUSY once, then succeed after the submit leaves. Then run the full C++ ctest unit and contract suites (PM condition) and record their logs. Test (a) must fail on the prototype without the close retry, showing the native socket left open through socket.cpp:53.
4. Measure, binding in the same session: unpaced RUNS=5 request-backpressure 1KiB and 4KiB, request-serial 1KiB, main vs prototype; and reply packets/request plus mutex wait at the same paced rate as before.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- No git operations except git diff and git status; no spec edits; no GitHub operations. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh.
  Do not modify bindings/ or core/. Changes only in framework/ and bench files of this worktree.
- C++ builds: `/root/jobs/fx/cx/cpp-build-slot.sh <cmd>` bare (no outer flock). Other builds/tests: `/root/jobs/fx/cx/measure-shared.sh <cmd>`. Measurements: `flock -o /root/jobs/fx/cx/measure.lock` (exclusive; other measurements are queued, keep waiting), condition B (source CPUs 0-7, target 8-15).
- MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &.
- 리팩토링 점검: list duplicate work (same check/encode/copy/handoff twice) you see, with file:line, and whether your change removed it; no new queue, lock, cache, flag or state.
- Report "over budget by X µs" per request (budget = binding µs / 0.6), never as a percentage. Do not suggest lowering the 0.6 target.
- Time box about 5 hours; time spent waiting for the measurement lock does not count, keep waiting instead of stopping.

## Output: /root/jobs/fx/cx/v21/s033/creply2/report.md (Korean, plain sentences): (1) close sites and how each is safe; (2) tests run with their log paths; (3) RUNS=5 table with Framework÷binding and over budget by X µs; (4) decision-site count before/after. Save the final diff as creply2/change.diff (framework/ only, without bench measurement edits) and bench edits separately.
