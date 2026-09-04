# Final C++ + Node 7-sample gates (2026-09-05)

## Scope and setup

Committed `main` was used without source or test edits.  The pre-existing modified
`bindings/node/provenance/core-package-provenance.json` and existing untracked
files were not changed.  Core and local packages were not rebuilt.  C++ was run
against the supplied `ZLINK_CPP_BUILD_DIR`; its required preflight
`cmake --build ... -j4` exited 0 with `ninja: no work to do`.  Node's one required
`npm run build` exited 0.  Node runs used `TMPDIR=/dev/shm/zlink-tmp-node`, an
unset `ZLINK_LIBRARY_PATH`, and one held `flock -w7200 /tmp/zlink-node-gate.lock`.

The C++ and Node gates were run concurrently.  This matters for the two transient
Node runner/environment failures recorded below; individual sample ports are only
reserved by the Node runner, not globally coordinated with C++ sample processes.

## C++ results

| sample | exit | duration | final completion evidence | result |
|---|---:|---:|---|---|
| TicTacToe | 0 | 6 s | `tictactoe-placement=completed` | PASS |
| Bingo | 0 | 5 s | `bingo-placement=completed` | PASS |
| DeliveryDispatch | 0 | 12 s | `deliverydispatch-placement=completed` | PASS |
| SupportChat | 0 | 30 s | `supportchat-placement=completed` | PASS |
| GameQuest | 0 | 5 s | `gamequest-placement=completed` | PASS |
| ShoppingMall | 0 | 33 s | `shoppingmall-placement=completed` | PASS |
| ZoneWorld | 0 | 113 s | `zoneworld=completed`, `PASS ZoneWorld.Cpp` | PASS |
| aggregate | 0 | 203 s | `sample all result=passed` | PASS |

## Node results

| sample | exit | duration | final completion evidence | result |
|---|---:|---:|---|---|
| TicTacToe | 0 | 12 s | `tictactoe-placement=completed` | PASS |
| Bingo | 0 | 11 s | `bingo-placement=completed` | PASS |
| DeliveryDispatch | 0 | 9 s | `deliverydispatch-placement=completed` | PASS |
| SupportChat | 0 | 29 s | `supportchat-placement=completed` | PASS |
| GameQuest | 1 → retry 0 | 30 s → 7 s | retry: `gamequest-placement=completed` | PASS on retry (D) |
| ShoppingMall | 0 | 5 s | `shoppingmall-placement=completed` | PASS |
| ZoneWorld | 0 | 169 s | `zoneworld=completed`, `PASS ZoneWorld` | PASS |
| aggregate | 1 | 10 s | stopped in TicTacToe before its client ran | FAIL (D) |

## Failure classification

| bucket | finding | first failure evidence / classification |
|---|---|---|
| A — DONTWAIT/backpressure | none | All final-gate logs were scanned for `BACKPRESSURED`, `EAGAIN`, `DONTWAIT`, and `would block`; no match. |
| B — terminal/error classification | none | No sample application terminal protocol error occurred. |
| C — known pre-existing | none identified | No prior-evidence `file:line` classification was used for these runs. |
| D — environment/runner | GameQuest first individual attempt | `gate-final-samples-node-GameQuest.log:20` — `Timed out waiting for endpoint tcp://127.0.0.1:28064`; the runner is waiting for its Docker Redis endpoint at `framework/languages/node/samples/run-sample.mjs:295`, and raises at `:417`.  The one required deterministic retry passed. |
| D — environment/runner | Node aggregate | `gate-final-samples-node-aggregate.log:26-39` — `BindError: bind failed`, `nativeErrno: 98` (`EADDRINUSE`) while starting TicTacToe `api-b`; the runner reports the child stop at `run-sample.mjs:423`, reached from `samples/TicTacToe.Ts/Runner/sample-runner.mjs:62-63`.  This is a bind/port collision, not a protocol verdict. |
| E — binding-port dependency | none | There was no REQUEST/SEND submit reporting `BACKPRESSURED`/`EAGAIN`, hence no unresubmitted submit site to report. |

## Exact commands

```bash
# C++ preflight and individual samples (selectors in table order), then aggregate
TMPDIR=/dev/shm/zlink-tmp-cpp \
ZLINK_CPP_BUILD_DIR=/home/hep7/project/zlink/framework/languages/cpp/build/linux-ninja-c-e2e \
cmake --build /home/hep7/project/zlink/framework/languages/cpp/build/linux-ninja-c-e2e -j4

TMPDIR=/dev/shm/zlink-tmp-cpp \
ZLINK_CPP_BUILD_DIR=/home/hep7/project/zlink/framework/languages/cpp/build/linux-ninja-c-e2e \
framework/languages/cpp/samples/run_samples.sh <TicTacToe|Bingo|DeliveryDispatch|SupportChat|GameQuest|ShoppingMall|ZoneWorld>
TMPDIR=/dev/shm/zlink-tmp-cpp \
ZLINK_CPP_BUILD_DIR=/home/hep7/project/zlink/framework/languages/cpp/build/linux-ninja-c-e2e \
framework/languages/cpp/samples/run_samples.sh

# Node preflight, individual samples (selectors in table order), then aggregate
cd framework/languages/node
TMPDIR=/dev/shm/zlink-tmp-node env -u ZLINK_LIBRARY_PATH npm run build
TMPDIR=/dev/shm/zlink-tmp-node env -u ZLINK_LIBRARY_PATH \
flock -w7200 /tmp/zlink-node-gate.lock bash samples/run_samples.sh <TicTacToe.Ts|Bingo.Ts|DeliveryDispatch.Ts|SupportChat.Ts|GameQuest.Ts|ShoppingMall.Ts|ZoneWorld>
TMPDIR=/dev/shm/zlink-tmp-node env -u ZLINK_LIBRARY_PATH \
flock -w7200 /tmp/zlink-node-gate.lock bash samples/run_samples.sh
```

The actual Node gate held the flock across build, all individual selectors, retry,
and aggregate so no other Node gate could interleave.

## Logs

All logs are under `zlink-work/c016/logs/`:

- `gate-final-samples-cpp-build.log`, `gate-final-samples-cpp-<sample>.log`, and `gate-final-samples-cpp-aggregate.log`
- `gate-final-samples-node-build.log`, `gate-final-samples-node-<sample>.log`, `gate-final-samples-node-GameQuest-retry.log`, and `gate-final-samples-node-aggregate.log`

## BLOCKERS

1. The Node aggregate gate is not green: its first TicTacToe server startup received `EADDRINUSE` (`nativeErrno: 98`).  Since the requested aggregate was run once, it was not rerun.
2. The individually retried GameQuest result is green, but its first attempt could not reach its dynamically published Docker Redis endpoint within the runner's 30-second wait.

