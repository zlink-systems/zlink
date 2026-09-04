# Node gate bootstrap summary

## Result

The local HTTP-client artifact was rebuilt successfully.  The Node runtime gate is
blocked by the pre-existing lint error.  Running its underlying test runner then
found 1,532 passing and six failing tests; the failures are classified below.
The Java cross-language peer stage passed.

## Bootstrap

No documented producer command was found in the requested Node scripts, README
files, package scripts, or getting-started guides.  `scripts/verify_packaged_contract.sh`
only reads the file pin and consumes the artifact.  The successful minimal
bootstrap was run from `framework/languages/node`:

```bash
npm pack ./packages/http-client --pack-destination ../../../.artifacts/wsl/npm
TMPDIR=/dev/shm/zlink-tmp-node npm install --package-lock=false
npm run build
npm pack ./packages/http-client --pack-destination ../../../.artifacts/wsl/npm
TMPDIR=/dev/shm/zlink-tmp-node npm install --package-lock=false
test -f node_modules/@zlink-systems/http-client/dist/index.js
```

The first pack contained `LICENSE` and `package.json` only; the second replaced
it with the 36-file, 44.2-kB `zlink-systems-http-client-0.10.0.tgz`, including
`dist/`.  Neither `package.json` nor `package-lock.json` was edited.
Full log: `zlink-work/c016/logs/gate-node-bootstrap.log`.

## Gate and smoke results

| Suite | Command | Result | Counts / verdict |
| --- | --- | --- | --- |
| Node gate | `TMPDIR=/dev/shm/zlink-tmp-node flock -w7200 /tmp/zlink-node-gate.lock npm test` | fail | build and typecheck passed; ESLint: 1 error, 0 warnings. Blocked at known pre-existing C. |
| Direct Node test runner | `node --test --test-force-exit --test-timeout=600000` over the 144 `test/**/*.test.js` files | fail | 1,532 pass / 6 fail / 0 skipped. See buckets. |
| Node cross-language smoke | `TMPDIR=/dev/shm/zlink-tmp-node framework/languages/node/cross-language/run_cross_language_smoke.sh` | fail | Browser connector to .NET stream host could not launch Chromium; D. The runner was stopped after its failure and its TestHost received its stop file. |
| Java cross-language stage | `ZLINK_CPP_CROSS_LANGUAGE_STAGE=java-cross framework/languages/cpp/cross-language/run_cross_language_smoke.sh` | pass | 4/4 spot-route directions passed. |

Full logs: `zlink-work/c016/logs/gate-node-npm-test.log`,
`gate-node-direct-tests.log`, `gate-node-sample-regression.log`,
`gate-node-cross-language-smoke.log`, and
`gate-node-java-cross-language-smoke.log`.

## Failure classification

| Bucket | Failure and exact assertion / source location | Classification |
| --- | --- | --- |
| A: DONTWAIT/backpressure | None. The direct suite's DONTWAIT tests passed, including `backend DONTWAIT Spot send awaits managed binding admission`. | No A failure. |
| B: terminal/error | `public SpotManager completes User Spot lifecycle through two native MeshNode processes`, `test/contract/user-spot-native-two-process.test.js:15`; child reports `RequestError: request failed`, rejected at `:170` (`operation.reject(new Error(message.message))`). | B — terminal `RequestError`; not a DONTWAIT/backpressure result. |
| C: known pre-existing | ESLint `@typescript-eslint/strict-boolean-expressions` at `packages/framework/src/runtime/spots/spot-timer.ts:137:12`: `!this.isTimerExecuting?.(name)`. | C — exact known lint location; it is the gate blocker. |
| D: environment | Chromium test `actual Chromium uses ws/wss, explicit flow, reconnect, drain, and browser trust`, `test/browser/stream-connector-chromium.test.js:16:1`, failed at `:26` (`chromium.launch`) because Playwright's Chromium executable is absent. | D — run `npx playwright install` to supply the executable. |
| D: environment | `test/contract/sample-regression.test.js:1:1` subprocess failed before sample assertions: `samples/run-sample.mjs:52` calls `startRedis`; `:269-271` executes `docker create ... redis:7.2-alpine`, which failed. | D — Docker/Redis container creation unavailable. The known SupportChat browser harness was not reached, so it cannot truthfully be counted or verified as C in this run; its launch point is `scripts/browser-e2e/connector-driver.mjs:34`. |
| D: environment | `test/contract/sample-zoneworld-domain.test.js:4` requires missing `samples/ZoneWorld/dist/Shared/spec`; whole file failed at `:1:1`. | D — required sample build output absent. |
| D: environment | `ZoneWorld maintenance rejects every arrival while allowing same-zone movement`, `test/contract/sample-zoneworld-gate.test.js:185`; require at `:186-189` could not find `dist/.../node-runtime-state.js`. | D — required sample build output absent. |
| D: environment | `ZoneWorld node status combines logical identity with live routing status`, `test/contract/sample-zoneworld-gate.test.js:222`; require at `:223-230` could not find `dist/.../node-registry.js`. | D — required sample build output absent. |
| D: environment | Node smoke `Browser TypeScript connector -> dotnet stream server`, `cross-language/node_dotnet_smoke.js:628`; `chromium.launch` at `scripts/browser-e2e/connector-driver.mjs:34` could not find Playwright Chromium. | D — same missing browser executable; the .NET host did start. |

No source, test, package manifest/lock, C++ framework, or protected framework
documentation files were changed.
