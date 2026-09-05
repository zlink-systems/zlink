# C++ + Node 7-sample gates, 12:54 packages (2026-09-05)

## Scope

No source, test, runner, Core, binding package, or protected-document changes were
made.  C++ was completely finished before Node started; no C++/Node port overlap
occurred.  The supplied C++ build directory was reconfigured and built against the
installed packages.  Node used one held `flock -w7200 /tmp/zlink-node-gate.lock`,
`TMPDIR=/dev/shm/zlink-tmp-node`, and an unset `ZLINK_LIBRARY_PATH` for its build,
individual runs, retries, and aggregate.  The package set included Core
`0145f5a59a`, `7cbf12de41`, and `cb62cb89f8`.

The first Node preflight invocation was a shell-argument error (`env` was given
`TMPDIR=...` before `-u`) and exited 127 without running npm; it is retained at
the top of the build log.  The immediately following, correctly formed invocation
ran the required `npm run build` once and exited 0.

## C++ results

| sample | exit | duration | marker | result |
|---|---:|---:|---|---|
| TicTacToe | 0 | 12 s | `tictactoe-placement=completed` | PASS |
| Bingo | 0 | 18 s | `bingo-placement=completed` | PASS |
| DeliveryDispatch | 0 | 28 s | `deliverydispatch-placement=completed` | PASS |
| SupportChat | 0 | 40 s | `supportchat-placement=completed` | PASS |
| GameQuest | 0 | 9 s | `gamequest-placement=completed` | PASS* |
| ShoppingMall | 0 | 39 s | `shoppingmall-placement=completed` | PASS |
| ZoneWorld | 0 | 126 s | `zoneworld=completed`, `PASS ZoneWorld.Cpp` | PASS |
| aggregate | 0 | 179 s | `sample all result=passed` | PASS* |

`*` GameQuest prints `Killed` in both its individual and aggregate logs, but this
is not teardown: `framework/languages/cpp/samples/GameQuest/run_sample.sh:343`
intentionally sends SIGKILL to the owner role (`mission-a` or `mission-b`) for the
owner-loss scenario, then `:346` requires status 137.  It is followed by the
completion marker and exit 0, so no retry was required.

## Node results

| sample | exit (retry) | duration (retry) | marker / teardown SIGKILL roles | result |
|---|---:|---:|---|---|
| TicTacToe.Ts | 1 (1) | 15 s (10 s) | `play-b` (`play-b`) | FAIL (B), deterministic |
| Bingo.Ts | 1 (1) | 11 s (10 s) | `matchmaking`, `play-a` (same) | FAIL (B), deterministic |
| DeliveryDispatch.Ts | 0 | 5 s | `deliverydispatch-placement=completed` | PASS |
| SupportChat.Ts | 1 (1) | 17 s (17 s) | `support`, `session` (same); retry also browser exit 1 | FAIL (B), deterministic teardown failure |
| GameQuest.Ts | 1 (1) | 6 s (6 s) | `mission-a` (`mission-b`) | FAIL (B), deterministic class |
| ShoppingMall.Ts | 0 | 5 s | `shoppingmall-placement=completed` | PASS |
| ZoneWorld | 1 (1) | 40 s (43 s) | `ops`, `zone-node-1`, `zone-node-2` (same) | FAIL (B), deterministic |
| aggregate | 1 | 10 s | stopped at TicTacToe.Ts: `play-a` teardown SIGKILL | FAIL (B) |

## Classification and failures

| bucket | finding | evidence / owner |
|---|---|---|
| A — DONTWAIT/backpressure | none | All gate logs have no `BACKPRESSURED`, `EAGAIN`, `DONTWAIT`, or `would block` match. |
| B — terminal/error classification | Node teardown SIGKILLs are real failures | `framework/languages/node/samples/run-sample.mjs:483-505` records an already-killed role, sends SIGINT, waits 500 ms, then records forced SIGKILL and throws at `:503-505`. The repeated roles are listed in the Node table and logs. |
| C — known pre-existing | none asserted | This execution did not establish a prior known-defect source location. |
| D — environment/runner | none | C++ and Node were serialized; Node flock was held, and no bind/endpoint failure was observed. The initial `env` typo occurred before npm or any sample process and was corrected immediately. |
| E — binding-port dependency | none | No submit/backpressure evidence was emitted. |

The Node failures reproduce the primary expected regression despite the installed
Core D-089 fix (`cb62cb89f8`): roles did not terminate within the runner's SIGINT
grace period and were killed by cleanup.  This gate classifies the symptom at the
runner's terminal teardown boundary; it does not claim a deeper Core/binding root
cause without a dedicated diagnosis.

## Commands

```bash
# C++ (first; completed before Node)
cd framework/languages/cpp
cmake --preset linux-ninja-debug -B build/linux-ninja-c-e2e
cmake --build build/linux-ninja-c-e2e -j4
TMPDIR=/dev/shm/zlink-tmp-cpp \
  ZLINK_CPP_BUILD_DIR=/home/hep7/project/zlink/framework/languages/cpp/build/linux-ninja-c-e2e \
  framework/languages/cpp/samples/run_samples.sh <Sample>
# Then once without a selector for aggregate.

# Node (one flock held across build, all samples/retries, and aggregate)
cd framework/languages/node
TMPDIR=/dev/shm/zlink-tmp-node env -u ZLINK_LIBRARY_PATH npm run build
TMPDIR=/dev/shm/zlink-tmp-node env -u ZLINK_LIBRARY_PATH \
  flock -w7200 /tmp/zlink-node-gate.lock bash samples/run_samples.sh <Sample.Ts|ZoneWorld>
# Then once without a selector for aggregate.
```

## Logs

All command logs are under `zlink-work/c016/logs/`:

- `gate-final-samples2-cpp-configure.log`, `gate-final-samples2-cpp-build.log`,
  `gate-final-samples2-cpp-<sample>.log`, and
  `gate-final-samples2-cpp-aggregate.log`
- `gate-final-samples2-node-build.log`, `gate-final-samples2-node-<sample>.log`,
  `gate-final-samples2-node-<sample>-retry.log` for each failed individual, and
  `gate-final-samples2-node-aggregate.log`

## BLOCKERS

1. Node is not green: five of seven individual samples and the aggregate fail due
   to teardown SIGKILLs, reproducible on every requested retry.
2. This job is execution/classification only.  The underlying process-lifecycle
   cause is not diagnosed or modified here.
