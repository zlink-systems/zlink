# Node and Java framework gates — 2026-09-04

All command output is retained in `zlink-work/c016/logs/gate-node-*.log` and
`zlink-work/c016/logs/gate-java-*.log`.  No source or test files were changed.

## Node

| Suite | Result | Pass/fail | Exact command |
| --- | --- | --- | --- |
| local dependency install | failed before test execution | n/a | `cd framework/languages/node && TMPDIR=/dev/shm/zlink-tmp-node npm install --package-lock=false` |
| runtime gate (`npm test`) | not run | n/a | blocked by the install failure above; requested command would have been `TMPDIR=/dev/shm/zlink-tmp-node npm test` |
| cross-language smoke | failed before smoke stages | 0/1 setup failure | `cd framework/languages/node && TMPDIR=/dev/shm/zlink-tmp-node ./cross-language/run_cross_language_smoke.sh` |

| Bucket | Failure and first assertion/error excerpt | Location | Classification evidence |
| --- | --- | --- | --- |
| D | `npm error enoent ... open '/home/hep7/project/zlink/.artifacts/wsl/npm/zlink-systems-http-client-0.10.0.tgz'` | `framework/languages/node/package.json:58` | The declared local `@zlink-systems/http-client` tarball is absent from `.artifacts/wsl/npm/`; only `zlink-systems-zlink-0.17.0.tgz` is present.  Therefore dependencies, including TypeScript, cannot be installed. |
| D | `Error: Cannot find module '.../framework/languages/node/node_modules/typescript/bin/tsc'` | `framework/languages/node/package.json:5` (`build`) | The cross-language script invokes `npm run build`; its required `node_modules` tree was not created because of the preceding missing tarball. |

No Node test ran, so ESLint (including the known `spot-timer.ts:137` item) was not reached and no direct unit-test fallback was applicable.  No (A), (B), or (C) test failure was observed.

Verdict: new failure — **(D)** missing `.artifacts/wsl/npm/zlink-systems-http-client-0.10.0.tgz` prevents Node installation, runtime tests, and the cross-language smoke.

## Java

| Suite | Result | Pass/fail | Exact command |
| --- | --- | --- | --- |
| core unit test | failed | 1,207/2 | `cd framework/languages/java && TMPDIR=/dev/shm/zlink-tmp-java flock -w7200 /tmp/zlink-jvm-gate.lock ./gradlew --no-daemon :zlink-framework-core:test contractTest` |
| root contract test | failed (run separately because Gradle stopped after `:zlink-framework-core:test`) | 26/1 | `cd framework/languages/java && TMPDIR=/dev/shm/zlink-tmp-java flock -w7200 /tmp/zlink-jvm-gate.lock ./gradlew --no-daemon contractTest` |
| Java Host install | passed | task passed | `cd framework/languages/java && TMPDIR=/dev/shm/zlink-tmp-java flock -w7200 /tmp/zlink-jvm-gate.lock ./gradlew --no-daemon -p cross-language :Host:installDist` |
| Java cross-language smoke (`java-cross` owner) | failed during first peer readiness | 0/1 stage setup failure | `TMPDIR=/dev/shm/zlink-tmp-java ZLINK_CPP_CROSS_LANGUAGE_STAGE=java-cross framework/languages/cpp/cross-language/run_cross_language_smoke.sh` |

| Bucket | Failure and assertion text | Location | Classification evidence |
| --- | --- | --- | --- |
| C | `ZLinkJavaRawMeshNodeM6ATest.observedInprocCloseDoesNotFenceDescriptorReplacement`: `java.lang.AssertionError: peer state was not observed: CLOSED` | test invocation `ZLinkJavaRawMeshNodeM6ATest.java:634`; failing assertion `ZLinkJavaRawMeshNodeM6ATest.java:1400` | Exact same test name and `peer state was not observed: ` assertion occur in baseline commit `e65abaf7ac`; this is one of the documented M6A pair. |
| C | `ZLinkJavaRawMeshNodeM6ATest.descriptorFenceReplacesEndpointOnlyIntent`: `expected: <true> but was: <false>` from `assertTrue(live)` | test invocation `ZLinkJavaRawMeshNodeM6ATest.java:573`; failing assertion `ZLinkJavaRawMeshNodeM6ATest.java:1439` | Exact same test name and `assertTrue(live)` assertion occur in `e65abaf7ac`; this is the other documented M6A failure. |
| C | `JavaDocumentationRegressionTest.canonicalCommonSpecOwnsLiveJavaContracts`: `expected: <true> but was: <false>` for `server/13-mesh-node.ko.md` | `JavaDocumentationRegressionTest.java:37` | The test name, assertion line, and target path are identical in `e65abaf7ac`, not merely the same count. |
| D | `timed out waiting for ready file .../node-spotroute-host-java.ready` | C++ runner Java-cross stage; Node peer cannot start | The Java Host installation succeeded.  The Node peer has no dependencies because the required local HTTP-client tarball is missing; the independent Node smoke confirms missing `node_modules/typescript/bin/tsc`.  This is an environment/setup dependency failure, not a terminal/error classification timeout. |

No (A) DONTWAIT/backpressure failure and no (B) terminal/error-classification failure occurred.

Verdict: new failure — **(D)** missing Node local dependency artifact blocks the Java cross-language peer; core and contract tests are otherwise green except the three verified pre-existing **(C)** failures.
