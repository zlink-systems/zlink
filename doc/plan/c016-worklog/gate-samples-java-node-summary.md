# Java and Node common-sample gate

## Result

The seven Java and seven Node common samples were run individually so one
failure could not prevent the remaining samples from running. Core and local
packages were not rebuilt, and no source, test, or protected documentation
file was changed.

### Java

| Sample | Result | Exit | Duration |
|---|---|---:|---:|
| TicTacToe | FAIL | 1 | 63.192s |
| Bingo | FAIL | 1 | 73.817s |
| DeliveryDispatch | FAIL | 1 | 47.752s |
| SupportChat | FAIL | 1 | 33.512s |
| GameQuest | FAIL | 1 | 55.027s |
| ShoppingMall | PASS | 0 | 20.920s |
| ZoneWorld | FAIL | 1 | 459.279s |

### Node

| Sample | Result | Exit | Duration |
|---|---|---:|---:|
| TicTacToe | PASS | 0 | 12.322s |
| Bingo | PASS | 0 | 10.815s |
| DeliveryDispatch | PASS | 0 | 8.402s |
| SupportChat | FAIL | 1 | 15.478s |
| GameQuest | PASS | 0 | 6.204s |
| ShoppingMall | PASS | 0 | 5.686s |
| ZoneWorld | FAIL | 1 | 120.483s |

## Failure classification

No failure has an observed `BACKPRESSURED`, `EAGAIN`, or DONTWAIT admission
result (A), and no log identifies a REQUEST `submit` result of that form or an
otherwise-unsettled awaitable at the binding port (E). Therefore these results
do not establish a D-B85 binding-port dependency.

| Language/sample | Bucket | First failing assertion or log excerpt | Evidence / classification |
|---|---|---|---|
| Java TicTacToe | C | `tictactoe-placement=completed`, then `play-a`: `ZLINK_FRAMEWORK_TERMINATION outcome=FORCE_STOPPED reason=TEARDOWN_FAILED` | Functional marker preceded teardown failure. This is the exact previously recorded sample condition in `doc/plan/c016-worklog/handoff-2026-09-04/fw-gate-java-summary.md:57-62`; its framework mapping site is `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/host/ZLinkFrameworkRuntime.java:1482-1486`. |
| Java Bingo | C | `Timed out waiting for 1 matches for 'bingo-lifecycle session-disconnect actor=player-1 destroy=false'` | Exact runner assertion: `framework/languages/java/samples/java/Bingo/run_sample.sh:277-279`. The prior evidence explicitly names the same Java Bingo `session-disconnect` issue as a separate termination/lifecycle follow-up: `doc/plan/c016-worklog/decisions.ko.md:808`. `bingo=completed` was printed after the initial wait failure, but the required lifecycle assertion still failed. |
| Java DeliveryDispatch | B | `java.util.concurrent.CompletionException: java.util.concurrent.TimeoutException` | First timeout is the client stream sequence completion, logged at `gate-samples-java-DeliveryDispatch.log:52-75`; first wait site is `DeliveryDispatchClientScenario.java:64-73`. This is a terminal timeout, not a backpressure result; no native errno/status mapping is logged. |
| Java SupportChat | C | `supportchat=completed`, then session role `FORCE_STOPPED reason=TEARDOWN_FAILED` | Functional marker preceded failure. Exact prior sample condition and lifecycle mapping evidence: `fw-gate-java-summary.md:57-62`; current runner’s failed lifecycle check is in `runner-common.sh:123-147`. |
| Java GameQuest | B | `IllegalStateException: Ensure failed` at `GameQuestClientScenario.java:215` | The final server-evidence assertion `ensure(assertion.passed())` failed (`GameQuestClientScenario.java:214-215`, helper at `:398-401`). This is an application assertion/error result, with no DONTWAIT/backpressure evidence. |
| Java ZoneWorld | B | `CompletionException: TimeoutException` from `ScenarioSupport$Game.moveTo`, `Scenarios.a2:67`; `scenario ZW-A2 failed` | First failure is a client-visible completion timeout (`gate-samples-java-ZoneWorld.log:68-80`) at `ScenarioSupport.java:142` / `Scenarios.java:64-68`. Further scenarios also timed out and the completion marker was withheld. This is terminal timeout behavior, not E: no REQUEST submission returned `BACKPRESSURED`/`EAGAIN`. |
| Node SupportChat | B | `SupportChat.Ts browser scenario failed: ZlinkStreamException: Wait for stream message timed out.` | Chromium did launch; failure is after browser scenario execution at `framework/languages/node/scripts/browser-e2e/run-sample.mjs:63`, propagated by `samples/run-sample.mjs:147`. The precise current failure is a stream-message timeout, so it is not classified C from the prior, different `expectFailure` rejection observation (`fw-gate-node-summary.md:31-33`). |
| Node ZoneWorld | D | `Error [ERR_MODULE_NOT_FOUND]: Cannot find package 'vite'` | The runner had built `samples/ZoneWorld/dist` and ran runtime verdicts, then shared-browser build failed. `npm exec vite build` site: `framework/languages/node/samples/ZoneWorld/Runner/sample-runner.mjs:716,768`; missing module comes from `framework/languages/shared_sample/zoneworld/client/vite.config.ts:1-2`. This is runner/dependency environment failure, not a sample protocol failure. |

## Logs

The complete captured stdout/stderr and appended exit/duration records are in:

- `zlink-work/c016/logs/gate-samples-java-*.log`
- `zlink-work/c016/logs/gate-samples-node-*.log`

The Java failure runner also preserved role logs under
`zlink-work/c016/logs/{tictactoe-java-*,sample-*}`.

## Commands

```bash
mkdir -p zlink-work/c016/logs /dev/shm/zlink-tmp-java /dev/shm/zlink-tmp-node

flock -w7200 /tmp/zlink-jvm-gate.lock bash -s
# TMPDIR=/dev/shm/zlink-tmp-java
# ZLINK_LIBRARY_PATH=/home/hep7/project/zlink/.artifacts/wsl/install/zlink-core/0.17.0/lib/libzlink.so
# for each of: TicTacToe Bingo DeliveryDispatch SupportChat GameQuest ShoppingMall ZoneWorld
#   bash framework/languages/java/samples/java/$sample/run_sample.sh 2>&1 \
#     | tee zlink-work/c016/logs/gate-samples-java-$sample.log

flock -w7200 /tmp/zlink-node-gate.lock bash -s
# TMPDIR=/dev/shm/zlink-tmp-node
# for each of: TicTacToe.Ts Bingo.Ts DeliveryDispatch.Ts SupportChat.Ts GameQuest.Ts ShoppingMall.Ts ZoneWorld
#   bash framework/languages/node/samples/$sample/run_sample.sh 2>&1 \
#     | tee zlink-work/c016/logs/gate-samples-node-${sample%.Ts}.log
```

## BLOCKERS

- Java: three known lifecycle failures (TicTacToe, Bingo, SupportChat); three observed terminal/application-timeout failures (DeliveryDispatch, GameQuest, ZoneWorld). The Java common-sample gate is 1/7.
- Node: SupportChat’s browser scenario reaches Chromium but times out waiting for a stream message; ZoneWorld’s shared-browser step cannot resolve `vite`. The Node common-sample gate is 5/7.
