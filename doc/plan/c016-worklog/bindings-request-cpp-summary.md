# C++ REQUEST 계약 통일 결과

## 결과

D-B85에 맞춰 C++ 비동기 REQUEST 제출을 SEND와 같은 `BACKPRESSURED + WRITABLE token` 상태 기계로 변경했다. 최초 즉시 admission 경로는 기존 borrowed-message 제출을 유지하며 새 payload 할당·복사·락을 추가하지 않는다. 거절된 경우에만 바인딩이 payload를 소유하고, 해당 요청의 WRITABLE completion에서 동일 요청을 재제출한다. admission 뒤에는 기존 REQUEST reply/timeout completion으로 완료한다.

## 변경 파일

- `bindings/cpp/src/Runtime/Messaging/operation_submit.hpp`
- `bindings/cpp/src/Runtime/Messaging/completion_owner.hpp`
- `bindings/cpp/src/Runtime/Messaging/completion_owner.cpp`
- `bindings/cpp/src/Runtime/Messaging/request_reply.cpp`
- `bindings/cpp/src/Runtime/Messaging/operation_state.hpp`
- `bindings/cpp/include/zlink/Contracts/Messaging/operation_contracts.hpp`
- `bindings/cpp/README.doxygen.md`
- `bindings/cpp/tests/contract/test_cpp_contract_request_writable_retry.cpp` (신규)
- `bindings/cpp/tests/contract/test_cpp_contract_request_reply.cpp`
- `bindings/cpp/tests/contract/test_cpp_contract_optimization_guard.cpp`
- `bindings/cpp/CMakeLists.txt`
- `bindings/cpp/perf/multi/common/perf_multi_reqrep.hpp`
- `bindings/cpp/perf/run_binding_common.sh`

`bindings/cpp/include/zlink_enum.h`의 PENDING 옵션 주석은 이미 "ABI-retained, stored and returned, otherwise ignored"로 동기화되어 있어 수정하지 않았다.

## API 동작 전/후

| 구분 | 이전 | 이후 |
|---|---|---|
| 비동기 REQUEST 최초 제출 | nonzero ID를 admission된 REQUEST completion ID로 간주 | `DONTWAIT` 결과를 분기: `OK + request ID`는 admission, `BACKPRESSURED/EAGAIN + token`은 대기 |
| backpressure payload | Core의 admission 전 pending 수락을 가정 | 거절 시점에만 바인딩 소유 snapshot을 만들고 호출자 참조를 분리 |
| 재시도 | 별도 REQUEST WRITABLE 단계 없음 | 자기 token/RID의 WRITABLE에서만 동일 operation을 재제출; 재거절이면 새 token으로 계속 대기 |
| 완료 | REQUEST reply/timeout completion | admission 뒤 기존 reply/timeout completion 유지 |
| terminal WRITABLE | REQUEST completion으로 잘못 해석 가능 | `ENOENT`, `ESHUTDOWN`, `ETERM`을 typed `submit_error_t`로 완료 |
| blocking REQUEST | 별도 raw 제출 구현 | 공용 REQUEST submit helper 사용; blocking 의미는 유지 |
| PENDING_MAX 옵션 | pre-admission pending pool 의미가 남아 있었음 | 숫자 ABI 저장/조회만 유지하며 동작에는 무시됨을 명시 |
| multi REQREP perf | async 반환/nonzero ID를 admission으로 간주 | 한 client slot이 WRITABLE 대기부터 reply/terminal까지 하나의 closed REQUEST lifecycle을 소유 |

completion owner는 WRITABLE 처리 후 entry를 제거하지 않고 재제출 또는 REQUEST phase 전환을 수행하며, 최종 reply/timeout/terminal에서만 제거한다. wake는 public poller 또는 기존 runtime owner만 사용하며 spin/sleep/timer 재시도를 추가하지 않았다.

## 테스트

- 신규 public API 회귀 테스트: 5/5 통과
  - 작은 HWM에서 일부만 최초 admission됨을 확인
  - peer drain/reply 후 WRITABLE을 통해 32개 동일 요청이 각각 한 번만 도착하고 reply 완료
  - TCP connect-before-bind REQUEST
  - 동일 socket의 REQUEST/SEND token 혼재
  - close 시 pending REQUEST token이 `submit_error_t(terminated, ESHUTDOWN)`으로 정리
  - 테스트 코드에 sleep 없음
- 기존 REQUEST reply contract 및 optimization guard: 통과
- 전체 `bindings/cpp/tests/run_tests.sh`: contract 16/16, sample smoke 7/7 통과
- `git diff --check -- bindings/cpp`: 통과
- `bash -n bindings/cpp/perf/run_binding_common.sh`: 통과

## 스모크 수치

### Single, tcp, 1024 B, duration 2, runs 1

최종 상태 `complete`, expected 15 / actual 15.

| 패턴 | 처리량 | 대역폭 | latency mean / p95 / p99 |
|---|---:|---:|---:|
| DEALER_ROUTER | 463,846.5 msg/s | 474.980 MB/s | 3.851 / 7.421 / 10.418 ms |
| DEALER_ROUTER_REQREP | 2,288 ops/s | 4.686 MB/s | 0.436 / 0.620 / 0.820 ms |
| ROUTER_ROUTER_REQREP | 2,053 ops/s | 4.205 MB/s | 0.485 / 0.617 / 0.703 ms |

보고서: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260905_004939.txt`

### Multi, clients 8, duration 2, runs 1

기본 transport `tcp,tls,ws,wss`, message size `1024,65536`. 최종 상태 `complete`, success 24 / failure 0, expected 120 / actual 120.

| 패턴 | transport | 1024 B | 65536 B |
|---|---|---:|---:|
| DEALER_ROUTER_REQREP | tcp | 13,838 ops/s | 9,996 ops/s |
| DEALER_ROUTER_REQREP | tls | 12,369 ops/s | 4,749.5 ops/s |
| DEALER_ROUTER_REQREP | ws | 14,744.5 ops/s | 7,546.5 ops/s |
| DEALER_ROUTER_REQREP | wss | 10,508.5 ops/s | 3,796.5 ops/s |
| ROUTER_ROUTER_REQREP | tcp | 15,176.5 ops/s | 10,821 ops/s |
| ROUTER_ROUTER_REQREP | tls | 10,107.5 ops/s | 3,293.5 ops/s |
| ROUTER_ROUTER_REQREP | ws | 11,139 ops/s | 5,510.5 ops/s |
| ROUTER_ROUTER_REQREP | wss | 6,553 ops/s | 1,872 ops/s |
| DEALER_DEALER | tcp | 267,603.5 msg/s | 33,607 msg/s |
| DEALER_DEALER | tls | 223,358.5 msg/s | 11,521.5 msg/s |
| DEALER_DEALER | ws | 250,557 msg/s | 27,375.5 msg/s |
| DEALER_DEALER | wss | 160,536.5 msg/s | 9,814 msg/s |

보고서: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260905_005546.txt`

외부 symlink Core build를 정상 재사용하도록 perf stale-source 검사를 실제 Core build의 source tree 기준으로 보정했다. `core/**`에서 configure/build/clean은 수행하지 않았다.

## BLOCKERS

없음.
