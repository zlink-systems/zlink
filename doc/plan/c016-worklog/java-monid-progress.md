2026-09-05T10:23:37+09:00 START: branch/worktree and core artifact freshness inspection
2026-09-05T10:23:47+09:00 BLOCKER: core/build stale — 감독자 재빌드 후 재실행 필요; inspecting Java and C identity expectations
2026-09-05T10:24:35+09:00 EDIT: activated inproc lifecycle and TCP CLOSED cases; correlated CLOSED with optional CONNECT_DELAYED new attempt; updated inproc skip reason
2026-09-05T10:25:05+09:00 COMPILE: compileTestJava exit=1 (stale Core does not affect compilation)
2026-09-05T10:25:25+09:00 COMPILE-RETRY: compileTestJava exit=0 after sourcing local runtime from repository root
2026-09-05T10:25:48+09:00 TEST-REFERENCE: systems.zlink.contract.MonitorConnectionIdentityContractTest run=1/5 exit=1 (core/build stale)
2026-09-05T10:25:56+09:00 TEST-REFERENCE: systems.zlink.contract.MonitorConnectionIdentityContractTest run=2/5 exit=1 (core/build stale)
2026-09-05T10:26:03+09:00 TEST-REFERENCE: systems.zlink.contract.MonitorConnectionIdentityContractTest run=3/5 exit=1 (core/build stale)
2026-09-05T10:26:09+09:00 TEST-REFERENCE: systems.zlink.contract.MonitorConnectionIdentityContractTest run=4/5 exit=1 (core/build stale)
2026-09-05T10:26:15+09:00 TEST-REFERENCE: systems.zlink.contract.MonitorConnectionIdentityContractTest run=5/5 exit=1 (core/build stale)
2026-09-05T10:26:17+09:00 TEST-REFERENCE: systems.zlink.contract.MonitorPollingContractTest run=1/5 exit=130 (core/build stale)
2026-09-05T10:26:21+09:00 TEST-REFERENCE: systems.zlink.contract.MonitorPollingContractTest run=2/5 exit=1 (core/build stale)
2026-09-05T10:26:26+09:00 TEST-REFERENCE: systems.zlink.contract.MonitorPollingContractTest run=3/5 exit=1 (core/build stale)
2026-09-05T10:26:30+09:00 TEST-REFERENCE: systems.zlink.contract.MonitorPollingContractTest run=4/5 exit=1 (core/build stale)
2026-09-05T10:26:37+09:00 TEST-REFERENCE: systems.zlink.contract.MonitorPollingContractTest run=5/5 exit=1 (core/build stale)
2026-09-05T10:26:49+09:00 TEST-REFERENCE-CORRECTED: systems.zlink.contract.MonitorConnectionIdentityContractTest run=1/5 exit=0 task=:test (core/build stale)
2026-09-05T10:26:54+09:00 TEST-REFERENCE-CORRECTED: systems.zlink.contract.MonitorConnectionIdentityContractTest run=2/5 exit=0 task=:test (core/build stale)
2026-09-05T10:26:58+09:00 TEST-REFERENCE-CORRECTED: systems.zlink.contract.MonitorConnectionIdentityContractTest run=3/5 exit=0 task=:test (core/build stale)
2026-09-05T10:27:03+09:00 TEST-REFERENCE-CORRECTED: systems.zlink.contract.MonitorConnectionIdentityContractTest run=4/5 exit=0 task=:test (core/build stale)
2026-09-05T10:27:08+09:00 TEST-REFERENCE-CORRECTED: systems.zlink.contract.MonitorConnectionIdentityContractTest run=5/5 exit=0 task=:test (core/build stale)
2026-09-05T10:27:13+09:00 TEST-REFERENCE-CORRECTED: systems.zlink.contract.MonitorPollingContractTest run=1/5 exit=0 task=:test (core/build stale)
2026-09-05T10:27:17+09:00 TEST-REFERENCE-CORRECTED: systems.zlink.contract.MonitorPollingContractTest run=2/5 exit=0 task=:test (core/build stale)
2026-09-05T10:27:22+09:00 TEST-REFERENCE-CORRECTED: systems.zlink.contract.MonitorPollingContractTest run=3/5 exit=0 task=:test (core/build stale)
2026-09-05T10:27:26+09:00 TEST-REFERENCE-CORRECTED: systems.zlink.contract.MonitorPollingContractTest run=4/5 exit=0 task=:test (core/build stale)
2026-09-05T10:27:31+09:00 TEST-REFERENCE-CORRECTED: systems.zlink.contract.MonitorPollingContractTest run=5/5 exit=0 task=:test (core/build stale)
2026-09-05T10:27:35+09:00 TEST-REFERENCE-SUMMARY: identity 5/5 pass; polling 5/5 pass using :test (core/build stale)
2026-09-05T10:30:29+09:00 FULL-SUITE-REFERENCE: bindings/java/tests/run_tests.sh exit=0 (core/build stale)
2026-09-05T10:30:44+09:00 FINAL-CHECK: git diff --check exit=0; verifying scope and annotations
2026-09-05T10:31:16+09:00 STOP: ./gradlew --stop exit=0
EXIT:0
