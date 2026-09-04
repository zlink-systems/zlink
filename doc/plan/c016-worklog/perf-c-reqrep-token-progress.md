# C perf REQREP REQUEST token 진행 로그

- 2026-09-05 시작: detached worktree 확인. 기존 변경은 `core/build`, `core/build-dev` symlink만 존재하며 보존한다.
- 범위: `bindings/c/perf/**`, `bindings/c/tests/**`만 수정. Core 아래 빌드 금지, 병렬도 3 이하.
- 계약 확인: D-B85 REQUEST backpressure는 nonzero wait token을 반환하며, 동일 queue의 matching WRITABLE(`ADMITTED`) 후 동일 request를 재제출해야 한다. REQUEST completion과 WRITABLE은 종류별 dispatch가 필요하다.
- 정책 확인: admission backpressure까지 무상한 연속 제출, reply completion 동시 진행, active 구간 안에서 완료된 왕복만 집계한다.
- 구현: multi requester별 retained payload/token/RID 상태와 completion kind dispatch를 추가했다. single requester도 DONTWAIT REQUEST와 matching WRITABLE 재제출로 전환하고 latency phase의 inflight=1 상한을 제거했다.
- C public contract test: HWM REQUEST 거절 → nonzero token → matching WRITABLE → 동일 payload 재제출 → nonzero REQUEST completion 흐름을 추가했다.
- 검증: `ZLINK_CORE_SOURCE=local ZLINK_BUILD_JOBS=3 bash bindings/c/tests/run_tests.sh` 통과(contract 9/9, sample smoke 6/6).
- 반복 검증: `test_c_dontwait_backpressure_contract` 5/5, `perf_multi_metrics_test` 5/5 통과.
- 빌드: 변경된 single/multi REQREP 및 smoke 대상은 `-j3`로 컴파일 통과. linked Core runtime은 외부 symlink를 사용하며 Core build는 수행하지 않았다.
- 최종 multi smoke: `CCU=8 DUR=2 SIZES=1024,65536 PATTERNS=DEALER_ROUTER_REQREP,ROUTER_ROUTER_REQREP,DEALER_DEALER TIMEOUT=600` 통과(6/6 throughput cells, fail 0).
- 최종 single smoke: `DEALER_ROUTER_REQREP,ROUTER_ROUTER_REQREP`, `tcp,inproc`, 1024B, duration 2 통과(4/4 cases, fail 0).
- 최종 반복: 새 public REQUEST token contract와 `perf_multi_metrics_test`를 함께 5회 실행해 모두 green 확인.
- 최종 범위 검사: 변경 5개 파일 모두 `bindings/c/perf/**` 또는 `bindings/c/tests/**`; `git diff --check` 통과. 기존 untracked `core/build`, `core/build-dev` symlink 보존.

EXIT:0
