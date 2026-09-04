# Bucket F — Node sample contract-test fixes

## Item 1 — SupportChat Spot close grace lifecycle

### Cause

`framework/languages/node/test/contract/sample-spot-lifecycle.test.js:49-53` used the former
`3001 ms` idle and `1001 ms` grace offsets. The sample budgets are now `idleTimeout: 10000` and
`closeGraceTimeout: 2000` in
`framework/languages/node/samples/SupportChat.Ts/Server/Configuration/sample-names.ts:11-15`.
`ConversationSpot.onTimer()` applies those values at
`framework/languages/node/samples/SupportChat.Ts/Server/Support/Infrastructure/ZLink/Spots/ConversationSpot/conversation-spot.ts:170-186`.
Consequently, the first test timer no longer entered `WaitingForClose`, and the second returned
`undefined` rather than the assigned agent ID. The pre-fix focused run reproduced the exact
`undefined !== 'agent-1'` failure.

### Fix

`framework/languages/node/test/contract/sample-spot-lifecycle.test.js:19-22,49-53` now loads the
compiled sample's `SampleTimings` and derives both timer instants from `idleTimeout` and
`closeGraceTimeout`, with a 1 ms after-deadline offset. The assertions are unchanged: the Spot must
still reach logical `Closed`, retain the member Actor without calling public close, and return
`agent-1`.

### Verification

- `TMPDIR=/dev/shm/zlink-tmp-node node --test test/contract/sample-spot-lifecycle.test.js`
  — passed 3 consecutive runs, each `3/3` tests.

## Item 2 — aggregate sample PASS markers

### Cause

`framework/languages/node/samples/TicTacToe.Ts/Runner/sample-runner.mjs:87-97` completed the browser
lifecycle and checked all client/server evidence, then printed only
`tictactoe-placement=completed`. The browser was launched with `startBrowser()` at
`framework/languages/node/samples/run-sample.mjs:151-183`; `startNode()` redirects that child's
stdout and stderr to `browser-client.log` at `framework/languages/node/samples/run-sample.mjs:348-355`.
Thus the client's `PASS TicTacToe.Ts` was valid evidence in the child log but absent from aggregate
stdout. The aggregate continued; this was not an exit-code or ordering abort. GameQuest and
ShoppingMall also depended on execution styles that did not guarantee a PASS marker on shared
runner stdout.

### Fix

`framework/languages/node/samples/run-sample.mjs:51-55` now emits `PASS ${sampleName}` only after the
sample build and `runSample(context)` complete successfully. This makes the shared runner own the
documented success marker while preserving every sample-specific self-check and failure path.
Samples whose foreground browser/client already emits PASS can have additional identical markers;
the final shared-runner marker is the one guaranteed after all runner evidence has passed.

### Verification

- `TMPDIR=/dev/shm/zlink-tmp-node flock -w7200 /tmp/zlink-node-gate.lock ./samples/run_samples.sh`
  — exit 0; all seven samples completed.
- `TMPDIR=/dev/shm/zlink-tmp-node flock -w7200 /tmp/zlink-node-gate.lock node --test test/contract/sample-regression.test.js`
  — passed `50/50`; `node run_samples.sh executes every sample self-check` passed.

Direct aggregate marker observations:

| Sample | Required marker | Exact-line occurrences | Result |
|---|---|---:|---|
| TicTacToe.Ts | `PASS TicTacToe.Ts` | 1 | PASS |
| Bingo.Ts | `PASS Bingo.Ts` | 3 | PASS |
| DeliveryDispatch.Ts | `PASS DeliveryDispatch.Ts` | 3 | PASS |
| SupportChat.Ts | `PASS SupportChat.Ts` | 3 | PASS |
| GameQuest.Ts | `PASS GameQuest.Ts` | 1 | PASS |
| ShoppingMall.Ts | `PASS ShoppingMall.Ts` | 1 | PASS |
| ZoneWorld | `PASS ZoneWorld` | 3 | PASS |

## BLOCKERS

None.
