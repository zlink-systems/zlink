Model gpt-6.1-sol, effort high. Earlier job briefs are history, not instructions.

# Task: issue #4420 — C++ `app_t`: `shutdown()` / `stop()` / `request_stop()` called after `run()` has returned (start-up failure) must not start a new termination worker that reads framework resources already closed by `run()`.

Never push, no GitHub (read the issue text below, not via gh), no sub-agents or other AI tools, never run internal-docs.sh or any git fetch/pull, never delete raw files. Do not touch other worktrees or their build outputs. Do not take /root/jobs/fx/cx/measure.lock yourself except through the build-slot script.

## Facts (main 6350275fac, file `framework/languages/cpp/framework/src/runtime/host/app.cpp`)
- `run()` failure paths `:3462-3474` (catch(...) rethrow) and the outer `catch` `:3480-3503` return 1 after `close_framework_resources` and `teardown_complete = true`, `run_active = false`, but leave `termination_operation.terminal == false` and `started == false`.
- `shutdown()` `:4370-4408` checks only `operation.terminal`; otherwise sets `started`, stores draining state and starts `run_shared_shutdown` on a new thread.
- `run_shared_shutdown` `:4410-…`, at `:4512-4516`, calls `channel_runtime_t::from (state.zlink.message_bus ()).pending_count ()` (→ `runtime/channels/channel_runtime.cpp:690` → `runtime/execution/state_lane.hpp:249`) on the already closed runtime. The process then ends with exit code -11 instead of a normal return.
- Repro evidence (previous job, bench side): /root/jobs/fx/cx/v21/s033/cbind2/report.md and raw/ (gdb-run-01.log has the stack; before-*-results.json: 10/10 exit -11 with the bench stats port occupied). The bench calls `_app.request_stop()` from `stop()` after seeing the failed run: `framework/bench/grpc/cpp/common/bench_stats_server.hpp:358-374`.
- `app_t::stop()` `:4695-4704` and `request_stop()` `:4707-4710` both go through `shutdown()`.

## Goal
After `run()` has finished its teardown (success or failure), a later `shutdown()` returns an already completed termination result and starts no worker; `stop()`/`request_stop()` do nothing harmful; destroying the `app_t` afterwards is clean. A `shutdown()` that races with `run()`'s own failure teardown must also be safe (either it observes the finished teardown and returns the terminal result, or it was started before teardown and `run()`'s teardown path waits/joins it as today). Decide one owner of the "run is over" fact: reuse the existing state (`termination_operation` under its mutex, `teardown_complete`/`run_active` under `termination_teardown_mutex`) — do not add a new flag, counter, queue, lock or timer. Make the run-failure path publish the terminal termination result through the same place the normal path uses (`:4680-4686` pattern), so `shutdown()` has one check.
Check what result value a post-failure `shutdown()` should report: read how `termination_result_t` / `termination_outcome_t` are documented and what the .NET implementation does for shutdown after a failed run (`framework/languages/dotnet/src/` — search for the host run/StopAsync/shutdown after start failure) and follow .NET (reference implementation). Write the .NET file:line you followed in the report.

## Steps
1. Read the code above and the .NET counterpart. Write the planned rule (who decides "run over", where the terminal result is published, what shutdown returns) into /root/jobs/fx/cx/v21/s033/c4420/progress.md before editing.
2. Unit tests in `framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_http_startup.cpp` (or the closest app lifecycle test file):
   a. occupied HTTP port → `app.run(0,nullptr) == 1` → `app.request_stop()` → `app.shutdown(1s)` task completes immediately with the terminal result → `app_t` destroyed; process stays alive. Must fail before the fix (run it on the unfixed build first and keep the log), pass after.
   b. same with `stop()` only, and with `shutdown()` called twice.
   c. normal run that stops via shutdown, then a second `shutdown()` after `run()` returned → returns the same terminal result (behaviour unchanged).
   Use ports from an occupied acceptor like the existing `OccupiedPortReturnsFailureAndUnhealthy` test.
3. Fix in app.cpp only (plus header if a private declaration is needed). Rule-count table before/after: decision locations for "run is over" and for "terminal result published"; must not grow.
4. Build only via `/root/jobs/fx/cx/cpp-build-slot.sh` (call it bare, no outer flock): Release preset, `-DUSE_BINDINGS_SOURCE=ON`, Core prefix 1.25.0 at /root/.cache/zlink/core/1.25.0/linux-x64, -j3, build dir inside this worktree, ≤25 GB. Pin your processes to CPUs 6-7,14-15. Check build rc 0 and test binary newer than sources before running tests.
5. Run the new tests and the existing host/lifecycle/termination/http unit tests (`ctest -R` on names containing Http, Shutdown, Termination, Lifecycle, App; list what you ran). Re-run the bench repro from cbind2 (occupied stats port, same args as its report) 10 times with the bench source at main (no bench change): expect a normal nonzero exit, never -11. Keep logs.
6. `bash scripts/format/format.sh --check cpp` from the worktree root.
7. Local commit(s) only, Korean message like recent repo fix commits, ending with (#4420), trailers not needed.

## Output
/root/jobs/fx/cx/v21/s033/c4420/report.md (Korean, short): cause with file:line, the .NET behaviour followed (file:line), the change, rule-count table, tests with fail-before/pass-after log paths, bench repro before/after counts, format result, commit hash. Raw logs under /root/jobs/fx/cx/v21/s033/c4420/raw. Time cap 2 h.
