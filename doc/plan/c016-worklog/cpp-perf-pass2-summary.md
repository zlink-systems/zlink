# zlink C++ binding hot-path 개선 pass 2

## 결과

- read-only 리뷰에서 공개 API·ownership·error contract와 측정 의미를 유지하는 내부 후보 3개를 채택했다.
- REQREP callgrind 고정비는 pass 1 after의 49.16k Ir/op·10.44 new/op에서 48.57k Ir/op·9.42 new/op로 줄었다. instruction은 1.2%, allocation은 9.8% 감소했다.
- 100-client 공식 after는 15/15 complete였다. pass 1 대비 size별 throughput ratio 평균은 DEALER_DEALER 100.01%, DEALER_ROUTER_REQREP 104.84%, ROUTER_ROUTER_REQREP 99.46%다.
- 5% 효과 기준을 넘은 셀은 DEALER_ROUTER_REQREP 256B(+14.44%)와 1024B(+18.16%)다. 나머지에는 단일 run 편차가 섞였으며 값을 고르기 위한 반복 측정은 하지 않았다.
- 공개 header diff는 없고, 변경은 `bindings/cpp/src/Runtime/Messaging/**` 5개 파일뿐이다.

## Read-only 리뷰 후보

| 후보 | 계약 보존 근거 | 예상 효과 | 채택 여부 |
|---|---|---|---|
| reply가 이미 도착한 경우 coroutine suspend 생략 | `async_result_t::awaiter_t::await_ready()`와 `async_operation_state_t::suspend()`의 terminal 재검사가 submit/reply race 양쪽을 이미 처리한다. | 이미 적용되어 추가 효과 없음 | 미채택: 중복 |
| scheduler `std::function`을 함수 포인터/고정 task로 변경 | public header의 `async_continuation_scheduler_t`와 custom coroutine promise 연동 signature/ABI를 바꿔야 한다. | indirect call·manager 비용 감소 가능 | 미채택: 공개 signature 보존 |
| resume slot을 operation bundle 내부에 배치 | operation마다 고유한 bundle/slot identity를 유지한다. queued callback은 aliasing `shared_ptr`로 같은 control block을 잡고, abandon 경쟁은 기존 atomic slot이 처리한다. scheduler의 public `std::function` signature는 유지한다. | suspended operation당 별도 control-block allocation 1회 제거 | 채택 |
| 2-part request staging inline화 | native submit은 이미 8-part stack staging을 사용하고, builder vector capacity는 pooled operation state에서 재사용한다. callgrind에서 state allocation은 1,405 op 중 1회였다. | 정상 상태에서 줄일 allocation 없음 | 미채택: 중복·복잡도 증가 |
| reply `message_t`를 vector 원소에 직접 adopt | 반환 `std::vector<message_t>`, part 순서와 native ownership은 그대로이고 임시 wrapper→vector 이동만 없앤다. | 2-part reply에서 move 2회/op 제거 | 채택 |
| socket당 첫 completion entry를 inline 보관 | 첫 entry도 기존 owner mutex가 소유하며 callback identity를 재사용하지 않는다. 둘째 이후 동시 operation은 기존 PMR map을 그대로 사용한다. | 흔한 1-outstanding 경로의 hash/node register·lookup·erase 제거 | 채택 |
| completion map node pool 확대 또는 entry/Future pool | PMR node는 이미 socket lifetime 동안 재사용한다. entry/Future identity 재사용은 늦은 completion의 ABA 위험이 있어 가이드 §4 금지 대상이다. | 추가 allocation 감소 근거 없음 | 미채택: no-go |
| perf REQREP client의 part/copy 방식 변경 | C와 C++ 모두 client당 1 outstanding, 같은 payload 크기, 같은 1/2 part 설정, payload→native message 복사 1회를 사용한다. | parity를 지키며 없앨 차이 없음 | 미채택: 측정 의미 보존 |
| public wrapper/Future 또는 coroutine frame pool | public consumer identity와 늦은 completion 수명이 관찰되며 가이드 §4의 pool 금지 범위에 해당한다. | allocation 감소 가능 | 미채택: ABA·ownership 위험 |

## 변경

- `async_operation_state.hpp`: public async operation의 resume slot을 고유 completion bundle 안에 두고, queued callback의 수명은 aliasing `shared_ptr`로 보존했다. standalone internal state는 기존 별도 slot allocation fallback을 유지한다.
- `request_reply.cpp`, `send_operations.cpp`: bundle 생성 직후 result에 bundle lifetime을 묶었다. 공개 terminal과 error mapping은 바꾸지 않았다.
- `completion_owner.hpp`, `completion_owner.cpp`: socket당 첫 completion entry는 inline `shared_ptr`에 두고 추가 동시 entry만 기존 PMR map에 둔다. shutdown·public/runtime owner 전환·send entry count 조건은 두 저장소를 함께 본다.
- `completion_owner.cpp`: reply vector를 먼저 resize하고 native reply part를 원소에 직접 adopt했다.

변경 파일:

- `bindings/cpp/src/Runtime/Messaging/async_operation_state.hpp`
- `bindings/cpp/src/Runtime/Messaging/completion_owner.hpp`
- `bindings/cpp/src/Runtime/Messaging/completion_owner.cpp`
- `bindings/cpp/src/Runtime/Messaging/request_reply.cpp`
- `bindings/cpp/src/Runtime/Messaging/send_operations.cpp`

## Callgrind before / after

조건은 10 clients, tcp, 1024B, 1초 DEALER_ROUTER_REQREP다. pass 2 run은 deadline 뒤 정상 drain된 10건을 포함해 submit/completion 1,415건으로 정규화했다.

| 항목 | pass 1 after | pass 2 after | 변화 |
|---|---:|---:|---:|
| 전체 Ir | 69,077,105 / 1,405 | 68,721,654 / 1,415 | -0.5% total(작업량 차이 포함) |
| Ir/op | 49.16k | 48.57k | -1.2% |
| 전체 `operator new` | 14,662 / 1,405 | 13,336 / 1,415 | 작업량 차이 포함 |
| `new`/op | 10.44 | 9.42 | -9.8% |
| `message_t` move/op | 5.00 | 3.00 | -40.0% |
| 별도 `async_resume_slot_t` allocation | 1.00/op | 0/public operation | 제거 |

pass 2 profile: `/home/hep7hep7/project/zlink-work/c016/profiles/cpp-reqrep-pass2.callgrind`.

## 공식 before / after

처리량 단위는 DEALER_DEALER가 msg/s, REQREP가 ops/s다. `after/C`는 pass 1의 paired C 값을 사용했다.

| 패턴 | 크기 | pass 1 after | pass 2 after | 변화 | after/C |
|---|---:|---:|---:|---:|---:|
| DEALER_DEALER | 64 | 740,751.0 | 743,440.6 | +0.36% | 68.9% |
| DEALER_DEALER | 256 | 711,017.8 | 724,345.8 | +1.87% | 74.3% |
| DEALER_DEALER | 1024 | 722,420.4 | 709,697.4 | -1.76% | 82.3% |
| DEALER_DEALER | 4096 | 335,123.0 | 331,874.4 | -0.97% | 92.7% |
| DEALER_DEALER | 65536 | 104,544.8 | 105,103.2 | +0.53% | 135.8% |
| DEALER_ROUTER_REQREP | 64 | 86,065.0 | 80,275.0 | -6.73% | 51.6% |
| DEALER_ROUTER_REQREP | 256 | 70,955.8 | 81,202.8 | +14.44% | 54.5% |
| DEALER_ROUTER_REQREP | 1024 | 62,216.8 | 73,514.8 | +18.16% | 45.9% |
| DEALER_ROUTER_REQREP | 4096 | 57,051.0 | 58,162.8 | +1.95% | 45.4% |
| DEALER_ROUTER_REQREP | 65536 | 21,354.2 | 20,585.0 | -3.60% | 89.5% |
| ROUTER_ROUTER_REQREP | 64 | 88,867.4 | 89,505.0 | +0.72% | 64.2% |
| ROUTER_ROUTER_REQREP | 256 | 70,545.8 | 61,315.4 | -13.08% | 47.7% |
| ROUTER_ROUTER_REQREP | 1024 | 64,666.8 | 66,712.0 | +3.16% | 56.7% |
| ROUTER_ROUTER_REQREP | 4096 | 59,820.2 | 61,991.0 | +3.63% | 59.7% |
| ROUTER_ROUTER_REQREP | 65536 | 21,953.8 | 22,588.6 | +2.89% | 114.0% |

공식 after report: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260905_043517.txt` (15/15 complete).

size별 C ratio 산술평균은 DEALER_DEALER 90.79%, DEALER_ROUTER_REQREP 57.39%, ROUTER_ROUTER_REQREP 68.43%다. 64KiB가 평균을 높이므로 작은 메시지의 잔여 격차는 별도로 봐야 한다.

## Gate

- 요청한 전체 gate: `ZLINK_CORE_SOURCE=local ZLINK_CPP_CORE_BUILD_DIR=$PWD/core/build ZLINK_BUILD_JOBS=4 bash bindings/cpp/tests/run_tests.sh` — PASS.
- contract: 16/16 PASS.
- sample smoke: 7/7 PASS.
- 관련 4종 `test_cpp_contract_request_reply`, `test_cpp_contract_request_writable_retry`, `test_cpp_perf_application_ready_queue`, `test_cpp_contract_optimization_guard`: 각각 5회 PASS.
- `test_cpp_send_close_stress`: PASS.
- `git diff --check`: PASS.
- `bindings/cpp/include` diff: 0. 공개 header signature 변경 없음.
- 기존 untracked `core/build`, `core/build-dev` symlink는 건드리지 않았고 Core configure/build/clean을 수행하지 않았다.

## No-go 기록

- 이미 존재하는 await-ready/recheck fast path를 중복 구현하지 않았다.
- scheduler type을 바꾸지 않았다. 이를 없애려면 public header ABI와 custom promise 계약이 달라진다.
- 2-part native staging과 PMR map-node 재사용은 이미 적용돼 있어 별도 inline buffer/pool을 추가하지 않았다.
- entry/Future/public wrapper/coroutine frame을 재사용하지 않았다. 늦은 completion과 새 operation 사이의 ABA를 만들 수 있다.
- C와 part 수·copy 수가 같은 perf client는 바꾸지 않았다. in-flight 상한, poll 방식, 측정 deadline도 그대로다.
- 단일 공식 after의 하락 셀을 지우기 위한 재측정이나 결과 선택을 하지 않았다.

## BLOCKERS

- 기능·검증 blocker 없음.
- 성능 목표는 REQREP에서 여전히 미달이다. C 대비 size 평균은 DEALER_ROUTER_REQREP 57.39%, ROUTER_ROUTER_REQREP 68.43%로 목표 75%에 못 미친다.
- 작은 one-way 셀도 64B 68.9%, 256B 74.3%, 1024B 82.3%로 개별 85%에 못 미친다. 다만 size 평균은 64KiB 영향으로 90.79%다.
- 공식 단일 run에서 DEALER_ROUTER_REQREP 64B(-6.73%)와 ROUTER_ROUTER_REQREP 256B(-13.08%) 하락이 있었다. 이번 pass의 변경은 size별 분기를 추가하지 않았고 callgrind 고정비는 감소했지만, 정책에 따라 반복값을 골라내지 않았으므로 이 하락은 그대로 남긴다.
