# zlink C++ binding hot-path 개선 pass 1

## 결과

- 범위: `bindings/cpp/**`만 수정했다. 공개 헤더, Core, 다른 binding, 문서는 수정하지 않았다.
- 측정 조건: paired before와 동일하게 multi/tcp, 100 clients, 5초, 1 run, Core 0.17.0을 사용했다.
- 작은 메시지에서 DEALER_DEALER는 64B +22.3%, 1024B +21.8%, DEALER_ROUTER_REQREP는 64B +36.1%, ROUTER_ROUTER_REQREP는 64B +42.1% 개선됐다.
- 전체 C++ 계약 테스트와 sample smoke, 관련 반복 테스트, send/close stress가 모두 통과했다.

## 원인 및 비용 위치

| 경로 | before callgrind 근거 | 확인한 고정 비용 | after callgrind |
|---|---:|---|---:|
| DEALER_DEALER, 1024B | C++ 13.47k Ir/msg, 3.42 `new`/msg (C 9.06k, 0.26) | 즉시 성공 SEND도 매번 async result와 completion entry를 따로 할당하고 owner map 등록/해제 및 mutex를 거침 | 12.46k Ir/msg, 1.39 `new`/msg |
| ROUTER_ROUTER_REQREP, 1024B | C++ 50.71k Ir/op, 12.57 `new`/op (C 23.98k, 1.13) | 요청마다 result/entry 분리 할당과 completion map node 할당이 반복됨 | 49.16k Ir/op, 10.44 `new`/op |

callgrind는 10 clients, 1024B, 1초의 좁은 위치 확인용 측정이다. 처리량 판정에는 아래 공식 100-client 측정을 사용했다.

| callgrind 상위 binding 항목 | DD before (11,540 msg) | DD after (12,414 msg) | REQREP before (1,336 op) | REQREP after (1,405 op) | 판정 |
|---|---:|---:|---:|---:|---|
| completion entry 생성 | 11,540 (1.00/msg) | 12,414 (1.00/msg) | 1,336 (1.00/op) | 1,405 (1.00/op) | completion identity 자체는 필요하며 result와 묶어 한 번에 할당 |
| owner register / unregister / map emplace | 각 11,540 (1.00/msg) | 각 17 (0.0014/msg) | 각 1,336 (1.00/op) | 각 1,405 (1.00/op) | DD 즉시 admission의 map/lock 제거; REQREP completion 대기는 유지하되 map node 재사용 |
| async ready / take | 각 11,540 (1.00/msg) | 각 12,414 (1.00/msg) | 각 1,336 (1.00/op) | 각 1,405 (1.00/op) | terminal publish 뒤 mutex 제거 |
| coroutine suspend + scheduler `std::function` | 각 15 (0.0013/msg) | 각 17 (0.0014/msg) | 각 1,336 (1.00/op) | 각 1,405 (1.00/op) | DD에서는 backpressure만 발생; REQREP의 남은 op당 고정 비용 |
| `message_t` move constructor | 23,080 (2.00/msg) | 24,828 (2.00/msg) | 6,681 (5.00/op) | 7,026 (5.00/op) | payload 복사는 확인되지 않았고 wrapper move가 남음; 공개 ownership 의미 때문에 pass 1에서 유지 |
| 전체 `operator new` | 39,422 (3.42/msg) | 17,318 (1.39/msg) | 16,797 (12.57/op) | 14,662 (10.44/op) | bundle 및 map-node pool 효과. 대응 C는 DD 4,696/18,307=0.26/msg, REQREP 5,103/4,510=1.13/op |

따라서 DD의 주된 추가 비용은 모든 즉시 성공 메시지에 붙던 completion map/lock과 분리 allocation이었고, REQREP는 필수 completion 대기 위에 coroutine scheduler/`std::function`, wrapper move, 남은 allocator 비용이 op마다 겹치는 구조였다. 거절 경로 탐색, drain/wake의 전역 scan, payload copy는 상위 추가 비용으로 관측되지 않았다.

## 변경

- async result와 completion entry를 한 control block에 함께 할당하고 aliasing `shared_ptr`로 기존 수명/ownership을 유지했다.
- completion entry 자체나 Core callback identity는 재사용하지 않고, owner mutex 아래 unordered-map node만 socket 수명 PMR pool에서 재사용한다.
- SEND 첫 `DONTWAIT` admission을 owner 등록보다 먼저 수행한다. 즉시 성공 경로는 owner mutex/map을 건너뛰고, 실제 backpressure만 등록한다.
- 선행 `WRITABLE` completion을 보존·재생해 submit과 등록 사이의 concurrent drain을 처리하고, 등록 실패 전에는 source ownership detach를 늦춰 기존 실패 ownership을 유지한다.
- async terminal은 release/acquire publish로 전환해 `ready()`와 완료 후 단일-consumer `take()`의 mutex를 제거했다.
- optimization guard에 선행 completion 보존과 submit-before-register 순서를 고정했다.

변경 파일:

- `bindings/cpp/src/Runtime/Messaging/async_operation_state.hpp`
- `bindings/cpp/src/Runtime/Messaging/completion_owner.hpp`
- `bindings/cpp/src/Runtime/Messaging/completion_owner.cpp`
- `bindings/cpp/src/Runtime/Messaging/send_operations.cpp`
- `bindings/cpp/src/Runtime/Messaging/request_reply.cpp`
- `bindings/cpp/tests/contract/test_cpp_contract_optimization_guard.cpp`

## Before / after

처리량 단위는 DEALER_DEALER가 msg/s, 두 REQREP 패턴이 ops/s다. `after/C`는 동일 paired C 기준이다.

| 패턴 | 크기 | C | C++ before | C++ after | 변화 | after/C |
|---|---:|---:|---:|---:|---:|---:|
| DEALER_DEALER | 64 | 1,079,498.4 | 605,522.0 | 740,751.0 | +22.3% | 68.6% |
| DEALER_DEALER | 256 | 974,458.6 | 586,876.2 | 711,017.8 | +21.2% | 73.0% |
| DEALER_DEALER | 1024 | 862,834.2 | 593,257.0 | 722,420.4 | +21.8% | 83.7% |
| DEALER_DEALER | 4096 | 358,081.8 | 292,841.6 | 335,123.0 | +14.4% | 93.6% |
| DEALER_DEALER | 65536 | 77,381.8 | 83,807.2 | 104,544.8 | +24.7% | 135.1% |
| DEALER_ROUTER_REQREP | 64 | 155,527.2 | 63,233.8 | 86,065.0 | +36.1% | 55.3% |
| DEALER_ROUTER_REQREP | 256 | 149,064.2 | 64,462.8 | 70,955.8 | +10.1% | 47.6% |
| DEALER_ROUTER_REQREP | 1024 | 160,067.4 | 60,897.8 | 62,216.8 | +2.2% | 38.9% |
| DEALER_ROUTER_REQREP | 4096 | 128,155.6 | 58,332.0 | 57,051.0 | -2.2% | 44.5% |
| DEALER_ROUTER_REQREP | 65536 | 22,993.0 | 21,033.2 | 21,354.2 | +1.5% | 92.9% |
| ROUTER_ROUTER_REQREP | 64 | 139,506.2 | 62,543.8 | 88,867.4 | +42.1% | 63.7% |
| ROUTER_ROUTER_REQREP | 256 | 128,620.4 | 58,917.2 | 70,545.8 | +19.7% | 54.8% |
| ROUTER_ROUTER_REQREP | 1024 | 117,738.4 | 58,578.6 | 64,666.8 | +10.4% | 54.9% |
| ROUTER_ROUTER_REQREP | 4096 | 103,833.8 | 55,796.4 | 59,820.2 | +7.2% | 57.6% |
| ROUTER_ROUTER_REQREP | 65536 | 19,822.2 | 21,690.4 | 21,953.8 | +1.2% | 110.8% |

공식 after 보고서: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260905_041338.txt` (15/15 complete).

## Gate

- 요청한 전체 gate 명령: `ZLINK_CORE_SOURCE=local ZLINK_CPP_CORE_BUILD_DIR=$PWD/core/build ZLINK_BUILD_JOBS=4 bash bindings/cpp/tests/run_tests.sh` — PASS.
- contract: 16/16 PASS.
- sample smoke: 7/7 PASS.
- 관련 테스트 `test_cpp_contract_request_reply`, `test_cpp_contract_request_writable_retry`, `test_cpp_perf_application_ready_queue`, `test_cpp_contract_optimization_guard`: 각각 5회 PASS.
- `test_cpp_send_close_stress`: PASS (`ownership_failures=0`, `bad_records=0`, `unexpected=0`).
- `git diff --check`: PASS.
- `bindings/cpp/include/zlink` diff: 0줄. 공개 API signature 변경 없음.
- tracked 변경은 위 6개 `bindings/cpp/**` 파일뿐이다. 기존 untracked `core/build`, `core/build-dev` symlink는 건드리지 않았다.

## BLOCKERS

- 작업·검증 blocker 없음.
- 남은 성능 gap: DEALER_ROUTER_REQREP 256~4096B는 C의 38.9~47.6%, ROUTER_ROUTER_REQREP 64~4096B는 C의 54.8~63.7%다. pass 1에서 확인한 allocation 감소만으로는 REQREP의 나머지 protocol/completion 고정 비용을 해소하지 못했다.
- 단일 after run에서 DEALER_ROUTER_REQREP 4096B가 -2.2%였다. 측정 의미를 유지하기 위해 재측정으로 값을 골라내지 않았으며, 다음 pass에서 반복 분산과 해당 경로 비용을 함께 확인해야 한다.
