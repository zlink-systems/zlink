# C perf REQREP REQUEST token 구현 요약

## 결과

D-B85 REQUEST 계약 B에 맞춰 C single/multi REQREP runner를 wait-token/WRITABLE 재제출 모델로 통일했다. REQUEST admission이 거절되면 원본 logical request를 application이 보존하고, 동일 completion queue에서 자신의 token/context/RID와 일치하는 WRITABLE을 받은 뒤 같은 payload를 재제출한다. `ZLINK_SUBMIT_OK`와 nonzero REQUEST completion ID를 받은 경우에만 in-flight로 집계한다.

## 변경

- `bindings/c/perf/multi/common/perf_multi_socket_reqrep.hpp`
  - requester socket별 retained request, wait token, retry-ready, routed RID 상태 추가
  - WRITABLE과 REQUEST completion을 kind별로 dispatch하고 token/context/RID/terminal 결과 검증
  - active 종료 후에도 거절된 logical request를 재제출해 token과 completion을 정상 drain
  - active deadline 안에서 완료된 reply만 throughput/latency에 반영하는 기존 집계 유지
- `bindings/c/perf/single/common/perf_single_reqrep.hpp`
  - 동일 retained request/token/WRITABLE 재제출 상태 추가
  - busy sleep fallback 없이 completion poller wake로 retry 진행
  - throughput/latency phase 모두 admission backpressure까지 연속 제출하도록 latency inflight=1 상한 제거
- `bindings/c/perf/single/src/perf_dealer_router_reqrep.cpp`
  - current completion API REQUEST를 DONTWAIT 제출로 전환
- `bindings/c/perf/single/src/perf_router_router_reqrep.cpp`
  - DONTWAIT REQUEST와 expected target RID 설정
- `bindings/c/tests/test_c_dontwait_backpressure_contract.c`
  - HWM admission 거절 → nonzero wait token → matching WRITABLE → 동일 payload 재제출 → REQUEST completion을 검증하는 public C 계약 시나리오 추가

## 검증

- `ZLINK_CORE_SOURCE=local ZLINK_BUILD_JOBS=3 bash bindings/c/tests/run_tests.sh`: contract 9/9, sample smoke 6/6 통과
- 새 REQUEST token contract: 5/5 통과
- `perf_multi_metrics_test`: 5/5 통과
- multi smoke: 6/6 throughput cells, fail 0
  - CCU 8, duration 2, TCP
  - sizes 1024/65536
  - DEALER_ROUTER_REQREP, ROUTER_ROUTER_REQREP, DEALER_DEALER
- single smoke: 4/4 cases, fail 0
  - DEALER_ROUTER_REQREP, ROUTER_ROUTER_REQREP
  - TCP/inproc, size 1024, duration 2
- 모든 빌드 병렬도 3 이하. Core external symlink runtime 재빌드 없음.
- `git diff --check` 통과. 남은 실패 없음.

EXIT:0
