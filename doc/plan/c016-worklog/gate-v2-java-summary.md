# Java framework gate v2 — 2026-09-05

The Java framework gate used the rebuilt 0.17.0 Java package containing the
D-B85 REQUEST port (`a06260f507`).  Full output is retained in
`zlink-work/c016/logs/gate-v2-java-core-test.log` and
`zlink-work/c016/logs/gate-v2-java-contract-test.log`.  No source or test
files were changed.

| Suite | Result | Pass/fail | Exact command |
| --- | --- | --- | --- |
| framework core unit test | failed | 1,206 / 3 | `cd framework/languages/java && TMPDIR=/dev/shm/zlink-tmp-java flock -w7200 /tmp/zlink-jvm-gate.lock ./gradlew --no-daemon --refresh-dependencies :zlink-framework-core:test` |
| root contract test | failed | 26 / 1 | `cd framework/languages/java && TMPDIR=/dev/shm/zlink-tmp-java flock -w7200 /tmp/zlink-jvm-gate.lock ./gradlew --no-daemon --refresh-dependencies contractTest` |

| Bucket | Failure and exact assertion/error | Location | Classification evidence |
| --- | --- | --- | --- |
| C | `ZLinkJavaRawMeshNodeM6ATest.observedInprocCloseDoesNotFenceDescriptorReplacement`: `java.lang.AssertionError: peer state was not observed: CLOSED` | invocation `ZLinkJavaRawMeshNodeM6ATest.java:634`; assertion `ZLinkJavaRawMeshNodeM6ATest.java:1400` | Exact test and assertion match the prior evidence at `doc/plan/c016-worklog/gate-node-java-summary.md:22`; the baseline `e65abaf7ac` has the same `awaitState` assertion at line 1400. |
| C | `ZLinkJavaRawMeshNodeM6ATest.descriptorFenceReplacesEndpointOnlyIntent`: `expected: <true> but was: <false>` from `assertTrue(live)` | invocation `ZLinkJavaRawMeshNodeM6ATest.java:573`; assertion `ZLinkJavaRawMeshNodeM6ATest.java:1439` | Exact test and assertion match the prior evidence at `doc/plan/c016-worklog/gate-node-java-summary.md:23`; the baseline `e65abaf7ac` has the same `assertTrue(live)` assertion at line 1439. |
| F | `EntrySpotActorDispatchTests.postCutActorArrivalWithoutForwardKeepsTheStaleTerminal`: `java.util.NoSuchElementException: No value present` from `actors.trySealActorRelocation("actor-a").orElseThrow()` | `EntrySpotActorDispatchTests.java:528` | Not in the earlier Java result (`doc/plan/c016-worklog/gate-node-java-summary.md:17-25`, which reports exactly the two M6A core failures).  The assertion is an absent relocation seal, not a DONTWAIT/backpressure result or a terminal/error-classification assertion.  Therefore it is a new observed failure relative to the pre-D-B85 gate result. |
| C | `JavaDocumentationRegressionTest.canonicalCommonSpecOwnsLiveJavaContracts`: `expected: <true> but was: <false>` for `Files.isRegularFile(commonSpec.resolve("server/13-mesh-node.ko.md"))` | `JavaDocumentationRegressionTest.java:37` | Exact test, assertion line, and target path match the prior evidence at `doc/plan/c016-worklog/gate-node-java-summary.md:24` and baseline `e65abaf7ac`. |

No (A) DONTWAIT/backpressure failure, (B) terminal/error-classification failure,
or (D) environment failure was observed.  The gate has three known pre-existing
(C) failures and one new observed (F) failure.
