# Node framework gate re-verification — 2026-09-04

Baseline: `main` at `4d263e66b9`; Core input was `core/build-dev/lib/libzlink.so.0.17.0` (requested `50d77800f2`).  No Core, binding source, spec, or framework source was changed; no commit was made.

## Result table

| Gate | Result | Count / evidence |
|---|---|---|
| Node binding rebuild | PASS | `@zlink-systems/zlink@0.17.0`, native addon rebuilt; prebuild verifier reported Core `0.17.0` linux-x64. |
| Node framework `npm test` | BLOCKED by known C baseline | Build and typecheck passed; ESLint stopped at 1 error, so the per-file runtime tests were not started. |
| DONTWAIT focused framework coverage | PASS | `test/contract/backend-contract.test.js`: 56 pass, 0 fail, including the two DONTWAIT cases. |
| Common samples | NOT GREEN | TicTacToe, Bingo, DeliveryDispatch passed; SupportChat failed and stopped the seven-sample runner. GameQuest, ShoppingMall, ZoneWorld were not run by that command. |
| Node--.NET E2E smoke | terminal result unverified | The command reached the direct-channel/route/spot-route/stream cases and the child process exited, but the runner removed its temporary directory before a terminal summary could be captured by the 30-second terminal window. Do not treat this as PASS. |

## Buckets

### (A) DONTWAIT-cleared

PASS. `framework/languages/node/test/contract/backend-contract.test.js:141` (managed binding admission) and `:193` (terminal binding failure) passed in the focused 56/56 run.  The implementation path is `framework/languages/node/packages/framework/src/runtime/backend/node/node-socket-backend-adapter.ts` from `4d263e66b9`; no residual Node DONTWAIT failure was observed.

### (B) terminal/error-classification cluster

No Node member of the requested C++/.NET terminal cluster was exercised by the Node-only gate.  Consequently there is no Node status-to-terminal mapping fix to apply.  The known C++ `records.size()==1` / `deadline_exceeded` and .NET stale-authority `TimedOut(101)` vs stale-terminal `(107)` items require their owning language gate evidence; this run made no Core or binding edit.

### (C) pre-existing

`framework/languages/node/packages/framework/src/runtime/spots/spot-timer.ts:137`: ESLint reports `@typescript-eslint/strict-boolean-expressions` (unexpected nullable boolean).  This is the stated baseline, not a DONTWAIT regression.  `npm test` therefore exits before runtime-test enumeration.

## Additional sample failure (not framework-fixable under this task)

`SupportChat.Ts` fails in the browser scenario while deliberately submitting a chat after the idle close.  The domain correctly throws `Closed conversation must reject messages.` at `framework/languages/node/samples/SupportChat.Ts/Server/Support/Domain/SupportChat/conversation.ts:91-93`; the scenario deliberately expects that rejection at `Client/supportchat-client-scenario.ts:220`.  The connector decodes the remote application error as `RemoteError` at `framework/languages/node/packages/stream-connector/src/Runtime/ZlinkStreamReceiveDispatcher.ts:228-229`, and `ZlinkStreamAssertions.expectFailure` catches action rejection at `Runtime/ZlinkStreamAssertions.ts:27-44`.

The browser still marks the scenario failed with that rejection (`ZlinkStreamException: Closed conversation must reject messages.`). This is a sample/browser-harness rejection-observation issue, not a Core/binding errno or a Node framework status mapping.  The requested fix policy permits only a minimal framework mapping fix, so no change was made.

## Commands

```bash
export TMPDIR=/dev/shm/zlink-tmp-node
# temporary package prefix copied only Core headers/runtime from core/build-dev
ZLINK_CORE_SOURCE=release ZLINK_CORE_INSTALL_PREFIX="$PWD/zlink-work/c016/core-prefix" \
ZLINK_CORE_PACKAGE_PREFIX="$PWD/zlink-work/c016/core-prefix" \
ZLINK_CPP_CORE_BUILD_DIR="$PWD/core/build-dev" ZLINK_CORE_BUILD_DIR="$PWD/core/build-dev" \
scripts/local-package/node/build-wsl.sh --core-prefix "$PWD/zlink-work/c016/core-prefix"

cd framework/languages/node
npm install --package-lock=false --ignore-scripts   # npm ci was blocked by stale HTTP-client tarball integrity
npm test
node --test --test-force-exit --test-timeout=600000 test/contract/backend-contract.test.js
cd ../../..
framework/languages/node/samples/run_samples.sh
framework/languages/node/samples/SupportChat.Ts/run_sample.sh
framework/languages/node/cross-language/run_cross_language_smoke.sh
```

`npm ci` could not be used because the checked-in lock expected SHA-512 `lKaI...`, while the shared `.artifacts/wsl/npm/zlink-systems-http-client-0.10.0.tgz` supplied `K7Fp...`; neither artifact nor lockfile was edited.
