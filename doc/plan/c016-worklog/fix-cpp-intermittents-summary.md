# C++ intermittent gate failures — diagnostic review

## Status and captured state

Phase 1 only. Two existing runtime defects (classification **B**) have deterministic reproductions. Runtime implementation is awaiting supervisor approval under repository `AGENTS.md` §3, lines 71–73. The original intermittent failures have not yet been conclusively reproduced and connected to these defects. No fix or post-fix gate is claimed.

- Branch: `main`; no branch operation, commit or push.
- Existing .NET edits and untracked directories were preserved. The separately created .NET campaign summary was also preserved.
- Repository change from this investigation: this requested summary only. Diagnostic sources, binaries and logs are under `/tmp/zlink-cpp-intermittents-20260906/`.
- Existing build: `framework/languages/cpp/build/linux-ninja-debug`, C++/Core packages 0.17.0. Diagnostic binaries used that preset's compiler/link commands and existing libraries. Sample diagnostic compilation used at most two workers; the sample runner used `--parallel 2`.
- Builds/tests held `/tmp/zlink-cpp-gate.lock`; sample compilation/execution additionally held `/tmp/zlink-samples-gate.lock`. Preflight checked the first `/proc/loadavg` value below 10 and `pgrep -c lto1 == 0`; measured values are in the run logs.
- An external `.NET` campaign repeatedly started 20 `yes` workers (`/dev/shm/zlink-mesh-admit-20260906/repeat.py gate-load 300 --load --stop-on-failure`). Our work initially waited for its load to fall. That workload restarted during the sample diagnostic run; it was not stopped or changed here.

## Fanout scope release

### Cause and evidence

`framework/languages/cpp/framework/src/runtime/host/app.cpp:2645` registers a shared coroutine-executor owner during configuration. Release is in `app_t::run()` at lines 2722/2750. `app_t::~app_t()` at line 902 and `app_state_t::~app_state_t()` at line 297 do not release a configured app that never ran. Consequently `runtime/dispatch/coroutine_executor.cpp:147–151` can return without draining after the last running app exits.

The earlier `AppFrameworkUsesConfiguredLocationStore` fixture (`test_cpp_framework_store_location_resolvers.cpp:1624–1641`) configures an app and destroys it without calling `run()`. Other configuration-only fixtures also precede Fanout. This is shared runtime ownership, rather than evidence that their providers share DI scope instances: `runtime/configuration/services.cpp:100–104` creates each child scope separately.

Fanout retains its scope in an asynchronous completion callback at `runtime/fanout/fanout_location_runtime.cpp:763`. Skipping the executor drain permits `app.run()` to return while that callback still owns a scope.

Deterministic interface reproduction: `repro_executor_owner.cpp` configures and destroys one unstarted app, runs and stops another, then queries the existing continuation-scheduler interface. `repro-executor-owner.log` records `app_exit=0 continuation_scheduler_after_last_run=1` (exit 1). It does not inspect the owner counter or use sleeps/retries to create the failure.

GDB evidence: `scope-gdb-now.log` records owner count **4 before decrement** at Fanout shutdown. The test created four dependencies, diagnostic IDs 3–6, at addresses `0x7ffd20002dd0`, `0x7ffd28003bd0`, `0x7ffd300247d0`, and `0x7ffd34000c90`. Constructor stacks show `automatic_handler_scope_filter_t` constructor injection through `service_provider_t::resolve()`. Thus the fourth allocation in this observed run was another current Fanout dispatch's filter dependency. All four were released in this traced run (`SCOPE ASSERT LIVE={}`, counters 4/4).

**Remaining evidence:** the particular unreleased scope in the original 4/3 failure has not been captured. Four full-label baseline repetitions passed. The deterministic owner leak establishes an existing defect but does not substitute for reproducing the original interleaving.

### Ownership, comparison and proposed fix

- Owner: Framework host execution lifetime; no Core/binding change.
- Contract: `framework-api` §10 (`00-foundation/06-framework-api.ko.md:480–487`) requires one scope per dispatch and exactly-once cleanup, including filter termination. Host shutdown must finish owned execution before reporting teardown complete.
- Cross-language: .NET `Runtime/Handlers/ZLinkHandlerDispatcher.cs:59–61` keeps the invocation scope and handler-instance owner in `await using` through dispatch completion. C++'s shared process executor adds the configuration-versus-execution ownership distinction responsible for this defect.
- Classification: **B — existing lifecycle defect**, implementation approval pending.
- Proposed change: register the coroutine-executor owner within actual `app.run()` execution and pair it with the existing normal/error releases. Configuration-only apps must not acquire a running executor owner. Do not add a Fanout-specific wait, counter, retry or extra executor.
- Proposed rule count: two mismatched lifetime rules (configuration acquires / execution releases) → one paired execution-lifetime rule.
- Regression candidate: integrate the deterministic configured-but-unstarted app reproduction through app/scheduler interfaces, and retain the original Fanout created/released equality assertion.

## ZoneWorld G4 terminal boundary

### Cause and evidence

The preserved original source log reports `kind=7`. `contracts/errors/error.hpp:21` identifies this as `DeadlineExceeded`. The sample maps it unchanged in `player_actor_relocation_adapter.hpp:28`, and its failed-join callback constructs `CrashRelocationProbeRes` at lines 87–90. The client predicate at `Client/main.cpp:658–664` accepts only `Unavailable`. Actual receipt of a `DeadlineExceeded` response by the original client is **not yet proven**; its log only proves the wait timed out.

An independent existing defect is in `runtime/mesh/raw_mesh_node_owner.cpp`:

- Lines 996–1004 already own the correct termination predicate: no discovery expectation and no admitted peer → fail the target's operations as `route_unavailable`.
- Natural monitor disconnect at lines 3880–3887 removes the admitted peer without evaluating that predicate.
- Liveness expiry at lines 3975–3980 does the same.

If Location removes the expectation while an admitted peer still exists, its initial check correctly leaves the operation pending. The later removal of that last peer then fails to settle the operation.

`repro_join_owner_loss.cpp` uses the existing raw-owner interface, obtains a received ActorJoin request/reply token, removes the expectation, and then removes the peer through either natural disconnect or the public liveness tick. The liveness case advances the clock argument instead of waiting. `repro-owner-loss.log` records both failures:

- `boundary=liveness admitted_peer=0 join_terminal=0`, exit 1.
- `boundary=disconnect admitted_peer=0 join_terminal=0`, exit 1.

The admitted peer's removal is observed before checking terminal state. The two-second request deadline is only a bounded repro deadline; no production timeout or retry count was changed.

**Remaining evidence:** the original G4 run's ordering of expectation removal and admitted-peer removal has not been captured. This defect is a candidate explanation, not yet a conclusive attribution of that run.

### Ownership, comparison and proposed fix

- Owner: Framework logical peer lifecycle and durable-operation completion. Binding continues to own physical transport and native request completion.
- Contract: `03-spot-actor/04-actor-model.ko.md` §8.1, lines 568–578, defines logical intent removal plus absence of an admitted peer as target lifecycle termination. Physical disconnect alone remains transient. `05-location-relocation/06-failure-failover-policy.ko.md` §2/§4 requires a single terminal result and no replacement of the failed logical owner.
- Cross-language: Java `ZLinkJavaRawMeshNode.java:7054` evaluates the same expectation/ready-peer conjunction for durable requests. Java ZoneWorld's crash probe explicitly uses 30 seconds (`ZoneSpot.java:239`); C++ uses 15 seconds (`ZoneNode/main.cpp:83`). .NET uses its 15-second DefaultRequestTimeout for this deferred Join. Node's G4 proof issues a request after owner-loss observation, unlike this in-flight join. These scenario differences must not be hidden by copying Java's longer budget.
- Classification: **B — existing logical-lifecycle completion defect**, implementation approval pending.
- Proposed change: invoke the existing termination owner after successful monitor/liveness removal, retaining its single predicate. Add no state, timer, monitor, retry or error-kind remapping in the sample.
- Proposed rule count: order-dependent terminal behavior → one lifecycle predicate evaluated when either input fact changes.
- Regression candidate: integrate both deterministic peer-removal orders through raw-owner interfaces; retain existing tests that physical loss with a retained expectation ends at the admission-dependent deadline rather than premature `Unavailable`.

## Verification and remaining gates

| Run | Result | Evidence |
|---|---|---|
| Original provided Fanout full gate | 4 created / 3 released | `/tmp/zlink-cpp-cs-server-only-ready-20260906/unit-gate.log:155–163` |
| Four unchanged full `ctest -L framework-unit --parallel 1` repetitions | 51/51 each, zero failures | `unit-baseline-{1,2,3,4}.log` and corresponding `LastTest` copies |
| GDB complete store-location-resolvers target | 40/40; Fanout 4/4; stale executor owners observed | `scope-gdb-now.log` |
| Executor ownership interface repro | Expected red, exit 1 | `repro-executor-owner.log` |
| ActorJoin lifecycle interface repro | Expected red in both orders, exit 1 each | `repro-owner-loss.log` |
| ZoneWorld diagnostic run | Stopped before G4; HTTP readiness curl 28 after 30001 ms | `zoneworld-diagnostic-1.log`; preserved `/tmp/tmp.RiZO85WrhO/logs/` |

For sample diagnostics, copies of the sample sources under `/tmp` enabled the existing Normal message-flow setting, retained the console/file capture, and logged the decoded crash-probe reply in the existing typed client predicate. The predicate itself was unchanged. No repository sample source was modified. The readiness failure occurred before that client started, so it provides no new crash-boundary result.

Not run: post-fix targeted 20x, post-fix full unit gate, and final `bash samples/run_samples.sh` 7/7. No runtime fix has been applied; the requested root-fix job remains incomplete pending supervisor B approval and the outstanding original-interleaving evidence. All remaining investigation wrappers owned by this task were stopped; preserved logs and repro sources remain available.
