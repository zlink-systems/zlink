# Core flow-state paired diagnosis and fix

The WEIGHT failure is a Core notification defect: admission can install a received weight silently before its queued owner command runs. The command then sees an unchanged scheduler value and emits no `PEER_WEIGHT_CHANGED`. The teardown timeout is a separate test-fixture ownership defect: Unity's longjmp skips closing the public monitor handles. Both defects are fixed in the uncommitted worktree diff.

- Worktree: `/home/hep7/project/zlink-core-tests`, branch `core-tests-rebase`, base `eda6ef24e59d2ca4002d9d8f18db41b762129c4f`.
- Patch: `/tmp/zlink-core-tests/flow-state-fix.patch`.
- Evidence directory: `/tmp/zlink-core-tests/flow-state/`.
- Scope: 11 Core source/test files. No specification changes or commits. The pre-existing untracked `core-tests-rebase-summary.md` is untouched. This requested report is the only file written in the MAIN tree.

## Root cause and ownership

The following source locations refer to the base commit unless marked “patched.”

### Missing public WEIGHT event

`socket_base_dispatch.cpp:426-435` records the network WEIGHT on its exact Application pipe from the I/O thread before submitting the owner command. `router_recv_path.cpp:108-112` and `dealer.cpp:89-92` can read that cache while registering the route. `socket_base_api.cpp:550-555` also installs cached weight during readiness through a deliberately monitor-silent initializer.

Consequently, `router_admission.cpp:464-468` or `dealer.cpp:491-496` later compares the command with an already equal scheduler value and returns without emitting an event. In the passing order, registration starts at the default 100 and the owner command applies the non-default value afterward, producing the event. This affects both initial admission and reconnection, in either direction.

Concrete failure evidence is `diagnostic-load/012.log`:

1. Initial ROUTER connection 17 reports dealer weights 0 and then 19. DEALER connection 15 reports router weights 23 and then 71.
2. Reconnect creates DEALER connection 22 and ROUTER connection 23; the public histories contain the old disconnect and the new READY records.
3. The ROUTER registers its new pipe with `initial=19`; its initializer sees `old=19 new=19`; the subsequent owner apply also sees `old=19 new=19`.
4. DEALER reports WEIGHT 71 on connection 22. ROUTER reports READY on connection 23, but its history has no WEIGHT 19 for that connection. The assertion fails.

`diagnostic-load/018.log` shows the same defect on the initial DEALER connection with weight 23. `diagnostic-load/022.log` covers the other silent path: registration starts at 100, readiness silently changes it to 71, and the later owner command emits nothing. Passing order is preserved in `diagnostic-load/002.log`.

These traces establish that the WEIGHT reaches Core and changes the scheduler; the missing operation is notification at application of the cached value. Monitor opening occurs before bind/connect. The original WEIGHT-only subscriptions already reproduce the failure; the diagnostic ALL-event subscriptions expose the successful reconnect around it.

The governing contracts are:

- `core/doc/spec/core/socket/07-router.en.md`, §5, lines 139-146: configured value after readiness, actual-change notification, current value on reconnect; verification requirement at line 454.
- `core/doc/spec/core/protocol/01-zmp.en.md`, “Peer-weight control,” lines 462-485: the exact Application pipe owns the weight; owner-command recording and scheduler application; an actual applied change emits the monitor event.
- `core/doc/spec/core/06-monitoring.en.md`, §4, lines 115-123: bounded lossy FIFO, dropping newly arriving records only when full, with no event coalescing. The alleged monitor one-slot coalescing rule does not apply. The failing trace contains only 9 ROUTER records, and the internal trace shows the notification was skipped before monitor enqueue.

The silent-admission code is attributed by `git blame` to `4006ec35b0c`, before `973ebe30d5`; this is not a bisect claim about scheduling frequency. No endpoint-release, duplicate-admission, or hold-credit correction was needed for this failure. The recorded reconnect reaches READY and the new weight is already applied, which distinguishes it from D-094/D-098 endpoint progress or D-118 missing credit.

### Assertion-to-teardown timeout

`test_flow_state_paired.cpp:322` invokes Unity failure, which longjmps past the function-local fixtures and the monitor-close calls at lines 366-367. `testutil_unity.cpp:200-261` forcibly closes sockets registered through `test_context_socket`, then calls context shutdown/term. The public monitor pull sockets opened by `testutil_monitoring.cpp:300-330` are not in that registry. The receiver probes were also stored in stack objects whose lifetime has been abandoned.

`before-load/005.gdb`, `006.gdb`, and `009.gdb` show the main thread at:

`UnityDefaultTestRun → teardown_test_context → zlink_ctx_term → mailbox_t::recv → signaler_t::wait → poll(timeout=-1)`.

The pipe logs show raw-socket teardown and termination acknowledgements. Context termination still waits for the unclosed monitor handles. `01-context.en.md`, `zlink_ctx_term`, lines 192-195 requires closing every socket; `06-monitoring.en.md` §2 and `zlink_monitor_close` require the caller to serialize receive and close. This evidence does not identify a Core close/linger deadlock when all handles are normally closed.

A diagnostic build with only the fixture ownership correction plus temporary tracing, still using the defective weight behavior, produced 6 assertion failures in 40 loaded runs. All six exited with status 1 in about 3.02-3.08 seconds; none hung or crashed. This independently verifies the teardown correction without hiding the WEIGHT failure.

## Change

The pipe owner command is now the sole writer of received weight. The I/O pre-recording function and its duplicate validation are removed. Cached admission and later commands both reach the existing `accept_peer_weight` / `apply_peer_weight` transition, so the same delta comparison governs scheduling, writable notification, and the monitor event.

A newly registered scheduler entry uses its ordinary default and applies the pipe-owned value through that transition. Pair readiness applies cached state before releasing the Application write hold; late ROUTER identity adoption applies it before publishing readiness/progress. Mailbox mutation owns the public API synchronization (`socket_base_lifecycle.cpp:408-417`), also used by public send selection (`socket_send_submit.cpp:295-302`), so this does not expose an intermediate default-weight send decision.

Compared alternatives: keeping silent initialization and adding separate notification history would retain two rules and add state. Unifying initialization with the existing applied-change transition removes the special case and requires no new Core state, timer, retry, or option.

Patched owner locations:

- `core/src/runtime/core/pipe.cpp:1978`: generation-checked owner recording.
- `core/src/runtime/sockets/common/socket_base_dispatch.cpp:440,482`: common readiness and cached-value application.
- `core/src/runtime/sockets/common/socket_base_api.cpp:553`: application before hold release.
- `core/src/runtime/sockets/dealer/dealer.cpp:89,478`: registration and existing delta transition.
- `core/src/runtime/sockets/router/router_recv_path.cpp:138,197` and `router_admission.cpp:452`: registration/late identity and existing delta transition.

Changed files are those implementations and their declarations in `pipe.hpp`, `socket_base.hpp`, `dealer.hpp`, and `router.hpp`, plus `core/tests/integration/test_flow_state_paired.cpp`.

The public C API fixture is owned by Unity setup/teardown instead of by the case stack. Both monitor receiver threads are joined and both monitors closed before raw sockets and context termination. The test also exercises inproc reconnection, verifies fresh connection IDs on both monitors, and dumps public event histories only on failure. Its 3-second WEIGHT deadline, CTest timeout, existing waits, and weight assertions are unchanged. All temporary runtime diagnostic statements are removed from the patch.

## Validation

Builds used the configured Release+LTO Ninja directory. Before each build, `/proc/loadavg` was below 10 and `pgrep -c lto1` was 0 (observed load 5.96 for the diagnostic build and 2.57 for the fix build). The tested binary resolves `libzlink.so.0` from this worktree's `core/build-gate/lib`.

| Validation | Result | Evidence under the evidence directory |
|---|---|---|
| Original integration lane, original order | 126/127; only `test_flow_state_paired` timed out | `integration-before-1.log` |
| Original selected case, 200 runs with 20 `yes` processes | 170 pass, 26 timeouts, 4 SIGSEGV after assertion | `before-load-summary.log`, `before-load/results.json`, per-run logs and gdb stacks |
| Diagnostic weight behavior with corrected fixture, 40 loaded runs | 34 pass, 6 ordinary assertion exits; no teardown hang | `diagnostic-load-summary.log`, `diagnostic-load/` |
| Related tests | 4/4 CTest executables pass: `test_flow_state_paired`, `unittest_flow_state_socket`, `unittest_flow_state_monitor`, `unittest_router_peer_weight` | `focused-after.log` |
| Fixed full `test_flow_state_paired`, 300 runs with 20 `yes` processes | **300/300 pass**, all 6 Unity cases per run: 1,800 case executions | `after-load-summary.log`, `after-load/results.json`, per-run logs |
| First fixed integration lane | 126/127; target passes; unrelated `test_reconnect_options` bind fails with EADDRINUSE at lines 176 and 227 | `integration-after.log` |
| That unrelated test alone | 1/1 pass | `reconnect-options-isolated.log` |
| Final integration lane, same order, existing `ZLINK_TEST_PORT_OFFSET=20000` isolation | **127/127 pass**, 189.74 s | `integration-after-isolated.log` |
| `git diff --check`, patch reverse-apply check, patch/worktree byte comparison | PASS | `/tmp/zlink-core-tests/flow-state-fix.patch` |

The loaded repeat and the first post-fix lane overlapped; another Core test process was also observed on the host. The EADDRINUSE port owner was not captured at the failing instant, so external interference is not asserted as proven. The final lane ran after the owned load/repeat processes ended, using the test suite's existing port offset to isolate fixed endpoints; test code and expectations were not changed for the port error.

Reproduction commands:

```bash
ZLINK_DEBUG_PIPE_TERM=1 ZLINK_DEBUG_ROUTER_ROUTE=1 ZLINK_MONITOR_TASK_DIAG=1   bash core/tests/run_test_lanes.sh --build-dir core/build-gate --lanes integration

# This harness owns and terminates exactly its 20 yes processes, saves every run,
# and attaches gdb to the first three hung children before the original 10 s cap.
python3 /tmp/zlink-core-tests/flow-state/repeat.py   /tmp/zlink-core-tests/flow-state/after-load 300 20 all

ZLINK_TEST_PORT_OFFSET=20000 ZLINK_DEBUG_PIPE_TERM=1   ZLINK_DEBUG_ROUTER_ROUTE=1 ZLINK_MONITOR_TASK_DIAG=1   bash core/tests/run_test_lanes.sh --build-dir core/build-gate --lanes integration
```

### Remaining gate failure

`hotpath_gate` was run because the changed files include hot paths. It retains the known `pair_inproc` lower-bound failure; the other three cells pass. No reference or tolerance was changed.

| Cell | Reference instructions/message | Patched measurement | Ratio | Gate |
|---|---:|---:|---:|---|
| dealer_dealer_inproc | 3455.381 | 3418.8952 | 0.9894 | PASS |
| dealer_router_reqrep_inproc | 12054.895 | 12126.2796 | 1.0059 | PASS |
| pair_inproc | 2681.957 | 2518.3441 | 0.9390 | FAIL |
| router_router_tcp | 2972.882 | 2974.0465 | 1.0004 | PASS |

A control measurement of `pair_inproc` with the preserved original shared library gives 2518.3624 instructions/message, also ratio 0.9390 and FAIL. The patched/original difference is approximately -0.00073%; the inherited failure is the symmetric ±5% reference rule applied to an approximately 6.10% instruction reduction. Logs: `hotpath-after.log` and `hotpath-baseline-pair.log`. This gate is not reported as passing and remains for supervisor disposition.

## Runtime classification

- 소유 계층: Core exact Application pipe owner command and existing DEALER/ROUTER scheduler transition; Unity fixture owns test handle cleanup.
- Spec 조항: ROUTER §5 weight rules and reconnect verification; ZMP “Peer-weight control” steps 1-5; Monitoring §2/§4; Context `zlink_ctx_term`.
- 교차언어 대조: C++ `Runtime/Eventing/monitor.cpp:148,170`, .NET `Runtime/Eventing/SocketMonitor.cs:20,42`, and Java `runtime/eventing/NativeMonitorSocket.java:54,89` directly consume/close the same Core C monitor. This is a shared Core defect, not a language-specific Framework adjustment; the comparison is source inspection, not a binding test run.
- 변경 분류: **B — existing Core notification defect and existing test-fixture cleanup defect.**
- 수정 전/후 규칙 수: received-weight recording owners **2 → 1**; scheduler application rules **2 (silent initial / notifying dynamic) → 1 (existing applied-change transition)**.

Patch SHA-256: `ca9eadc792154f309f8d08de505a67d82a553597668fe33733c86d5340ea84a9`.
