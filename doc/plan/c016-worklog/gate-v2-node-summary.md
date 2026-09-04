# Gate v2 — Node framework on rebuilt 0.17.0 binding

## Result

The rebuilt `@zlink-systems/zlink` 0.17.0 tarball was installed without changing
the lockfile, and the Node build passed. The direct runtime suite completed with
**1,552 pass / 5 fail / 0 skipped** (1,557 total). There is no observed
DONTWAIT/backpressure failure. `npm test` remains blocked at its known ESLint
baseline. Chromium was available, so the cross-language smoke ran past the
browser stage and stopped at its previously-recorded .NET flow-listener assertion.

## Commands and suite counts

All commands were run from `framework/languages/node`, except the smoke command,
which was run from the repository root.

| Suite | Exact command | Result |
| --- | --- | --- |
| Package install | `TMPDIR=/dev/shm/zlink-tmp-node npm install --package-lock=false` | pass; 1 package changed, lockfile unchanged |
| Build | `npm run build` | pass |
| Direct tests | `node --test --test-force-exit --test-timeout=600000 test/**/*.test.js` | fail; 1,552 pass / 5 fail / 0 skipped |
| Standard gate | `TMPDIR=/dev/shm/zlink-tmp-node npm test` | fail; build and typecheck pass, lint 0 pass / 1 error |
| Node cross-language smoke | `TMPDIR=/dev/shm/zlink-tmp-node framework/languages/node/cross-language/run_cross_language_smoke.sh` | fail; `dotnet connector -> Node stream server` flow assertion |

Logs: `zlink-work/c016/logs/gate-v2-node-install.log`,
`gate-v2-node-build.log`, `gate-v2-node-direct-tests.log`,
`gate-v2-node-npm-test.log`, and `gate-v2-node-cross-language-smoke.log`.

## Failure classification

| Bucket | Exact failed assertion and location | Verdict |
| --- | --- | --- |
| A — DONTWAIT/backpressure | None. The direct suite completed all DONTWAIT cases without `BACKPRESSURED`, `EAGAIN`, or a native errno. | No A failure. |
| B — terminal/error | None. | No B failure. |
| C — known pre-existing | `test/contract/sample-zoneworld-domain.test.js:4` cannot require `samples/ZoneWorld/dist/Shared/spec`; the file-level test fails at `:1:1`. Exact prior evidence: `doc/plan/c016-worklog/gate-node-bootstrap-summary.md:54`. | C: unchanged missing prebuilt ZoneWorld output. |
| C — known pre-existing | `ZoneWorld maintenance rejects every arrival while allowing same-zone movement`, `test/contract/sample-zoneworld-gate.test.js:185`, cannot require `dist/Server/ZoneNode/Domain/node-runtime-state.js` at `:186`. Exact prior evidence: `gate-node-bootstrap-summary.md:55`. | C: unchanged missing prebuilt output. |
| C — known pre-existing | `ZoneWorld node status combines logical identity with live routing status`, `test/contract/sample-zoneworld-gate.test.js:222`, cannot require `dist/Server/Ops/node-registry.js` at `:223`. Exact prior evidence: `gate-node-bootstrap-summary.md:56`. | C: unchanged missing prebuilt output. |
| C — known pre-existing | Standard gate ESLint assertion: `@typescript-eslint/strict-boolean-expressions` at `packages/framework/src/runtime/spots/spot-timer.ts:137:12`, `!this.isTimerExecuting?.(name)`. Exact prior evidence: `gate-node-bootstrap-summary.md:51`. | C: unchanged gate blocker. |
| D — environment/harness | Smoke `assertFlowLog`, `framework/languages/node/cross-language/node_dotnet_smoke.js:900-907`, expected `packet=...`; the .NET `.flow` contains four `packet=<null> flow=<null> origin=<null>` lines, despite `Node stream events: dispatch:dotnet-to-node, reply:sent`. | D: established TestHost listener/tag-contract mismatch, not a request failure. Exact prior evidence and same assertion: `doc/plan/c016-worklog/c-cross-language-e2e-2-summary.md:76-81`; preserved log `zlink-work/c016/logs/c-e2e-2-07-node-cross.log:26-42`. |
| F — new since the named D-B85 comparison | `node run_samples.sh executes every sample self-check`, `test/contract/sample-regression.test.js:2271`, fails at `:2282`: expected `/PASS TicTacToe\\.Ts/`, but runner output has `tictactoe-placement=completed` and no `PASS TicTacToe.Ts`. The earlier gate instead failed this file before assertions because Docker Redis creation failed (`gate-node-bootstrap-summary.md:53`). | F: newly exposed/changed assertion failure; no DONTWAIT evidence. |
| F — new since the named D-B85 comparison | `SupportChat closes a conversation Spot after the close grace deadline`, `test/contract/sample-spot-lifecycle.test.js:10`, exact assertion at `:53`: actual `undefined`, expected `'agent-1'`. | F: absent from the named earlier direct-run failure list (`gate-node-bootstrap-summary.md:49-57`); no backpressure evidence. |

The older direct-run terminal `RequestError` at `user-spot-native-two-process.test.js` is absent in this run. Chromium is also now available: its direct browser test passed, unlike the earlier missing-browser D result. No source, test, binding, Core, protected documentation, package manifest, or lockfile was modified.
