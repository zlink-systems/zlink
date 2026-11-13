# Request별 lock·heap 측정 — C++ Framework / binding

## 결과와 범위

Worktree는 `perf/3867-cpp-receive-handoff`, HEAD `5ecae5de8b`였고 변경하지 않았다. 작업 산출물은 이 디렉터리에만 썼다. Framework 카운트는 복사한 `chand` 계수기와 동일한 인터셉터로 새 측정했다. 30초 warmup, 5초 active, 1 KiB `request-backpressure`, source CPU 0–7, target CPU 8–15 조건에서 요청 248,171개가 완료됐고 source/target 개수 일치, 오류 0, 미완료 0이었다. 메모리 가용량은 측정 전 12 GiB를 넘었다.

### Framework: source 요청당 정확한 인터셉터 카운트

Heap은 `malloc/calloc/realloc/aligned_alloc` 호출 수다. Explicit mutex는 `pthread_mutex_lock` 및 성공한 `pthread_mutex_trylock` 수다. implicit condition-wait 재획득은 세지 않는다. 이 값은 lock 대기 시간이나 경합 횟수가 아니다.

| 실행 역할 | Heap 호출/request | Explicit mutex/request | 기준 요청 수 |
|---|---:|---:|---:|
| Caller | 17.905 | 36.429 | 248,171 |
| Receive role | 9.125 | 44.018 | 248,171 |
| Completion workers 합계 | 7.001 | 6.002 | 248,171 |
| 기타 source threads | 2.240 | 9.329 | 248,171 |
| Source 전체 | 36.271 | 95.778 | 248,171 |

이 측정은 제공된 chand 측정과 비슷한 크기의 값이다. chand의 기존 측정은 caller 17.9 / 36.6, receive 9.2 / 44.3, workers 7.0 / 6.0, other 2.2 / 9.4였다. 이번 Framework 측정은 같은 계수기 경로로 다시 얻은 값이다.

### Framework target

| Process scope | Heap 호출/request | Explicit mutex/request | 기준 요청 수 |
|---|---:|---:|---:|
| Target 전체 (IO 및 server threads) | 42.747 | 58.072 | 248,171 |

target 계수기는 서버의 세부 thread role을 따로 표시하지 않았다. 따라서 이 행을 IO, handler, worker별 값으로 나누지 않았다. 기존 `chand` 하네스의 role 표시는 source Framework receive와 completion worker를 구분한다.

### Binding 비교 상태

요청한 `zlink-cpp` run은 기존 target이 phase를 거부해 readiness 대기 조건을 보완했으나, 공유 measurement lock의 선행 writer가 유지되어 이번 동일 계수기 run은 시작되지 않았다. 이 보고서에는 binding의 이번 run 결과를 만들거나 추정하지 않았다.

참고 가능한 이전 측정은 `/root/jobs/fx/cx/v21/s033/cprof/report.md` 및 `/root/jobs/fx/cx/v21/s033/calloc/report.md`다. 서로 다른 실행/계수 설정이므로 이번 카운트와 직접 비교하는 결과가 아니다. 이전 profile의 binding 측정은 `zlink-cpp`, 1 KiB request-backpressure에서 caller heap 12.395, whole source heap 12.967, whole target heap 7.728 calls/request였다. 같은 run의 explicit mutex calls/request는 없다. 따라서 binding per-role lock/heap 비교는 미완료다.

## Call-site attribution

Heap call stacks는 기존 allocation-site capture의 1/256 per-thread sample에서 가져왔다. 해당 capture는 2026-11-05, HEAD `ee11390257`, 10초 warmup / 20초 active run이어서 새 Framework 계수 run과 commit·기간이 다르다. 표본 stack을 확장한 calls/request이며, 새 run의 관측값으로 취급하지 않는다. Framework caller의 named-site coverage는 100%, top rows는 95.69%를 차지했다. Framework whole source top rows는 81%를 조금 넘게 덮고, target top rows는 81%를 조금 넘게 덮었다. Binding caller 표본의 named-site coverage는 100%였고, top rows는 99.88%를 덮었다.

아래 site의 `locks/request`는 **측정되지 않음**이다. 기존 인터셉터는 mutex를 정확히 세었지만 stack을 기록하지 않았다. 별도 CPU profile의 sampled stacks는 lock call 자체를 표본화한 것이 아니며, 그 비율을 mutex 횟수로 환산할 수 없다. 따라서 80% coverage 조건을 lock count에 대해 충족했다고 주장하지 않는다.

| Call site (function, file:line) | Role | Locks/request | Heap/request | Binding run에 존재? | Framework 결정 / 역할 |
|---|---|---:|---:|---|---|
| `request_task_access_t::receive`, `framework/include/zlink/framework/contracts/dispatch/request_task.hpp:698` | Framework caller | 미측정 | 6.093 | 아니오 | 응답 요청 task와 receiver를 수신 경로에 연결 |
| `route_client_t::submit_request_reply_message_erased`, `framework/src/runtime/channels/channel_runtime.cpp:1732` | Framework caller | 미측정 | 5.084 | 아니오 | 채널 요청 및 reply completion 등록 |
| `materialize_binding_part`, `framework/src/runtime/backend/raw_binding_adapter.hpp:211` | Framework caller | 미측정 | 4.010 | 부분 | native binding part를 Framework 표현으로 materialize |
| `request_receiver_t` 복사/생성자, `framework/include/zlink/framework/contracts/dispatch/request_task.hpp:698` | Framework caller | 미측정 | 3.990 | 아니오 | receiver completion 소유권 전달 |
| `framework_driver_t::run_tasks`, `framework/bench/grpc/cpp/client/bench_cpp_client.cpp:889` | Framework caller / benchmark caller | 미측정 | 3.023 | 아니오 | benchmark 요청 입력·비동기 수집 |
| `application_payload_t::from_parts`, `framework/src/runtime/protocol/service_wire_codec.cpp:2941` | Framework caller | 미측정 | 2.997 | 아니오 | multipart application payload 구성 |
| `encode_node_request_header`, `framework/src/runtime/protocol/service_wire_codec.cpp:2941` | Framework caller | 미측정 | 2.993 | 아니오 | node request correlation header 직렬화 |
| `task_shared_state_t` completion observer, `framework/include/zlink/framework/contracts/dispatch/task.hpp:721` | Framework caller | 미측정 | 2.960 | 부분 | task terminal 관찰자 등록 및 전달 |
| `route_client_t::request_to_node`, `framework/include/zlink/framework/contracts/channels/channel.hpp:898` | Framework caller | 미측정 | 2.014 | 아니오 | 공개 채널 API의 node 요청 경로 선택 |
| `raw_mesh_node_owner_t::request_with_header`, `framework/src/runtime/mesh/raw_mesh_node_owner.cpp:1498` | Framework caller | 미측정 | 1.990 | 부분 | mesh node 요청 헤더 및 전송 요청 구성 |
| `raw_route_port_t::request`, `framework/src/runtime/backend/raw_route_port.cpp:311` | Framework caller | 미측정 | 1.962 | 부분 | binding raw route request 제출 |
| `operation_registry_state_t::register_operation`, `framework/src/runtime/foundation/operation_registry.cpp:363` | Framework caller | 미측정 | 1.942 | 아니오 | logical operation 등록과 terminal 상태 수명 소유 |
| `raw_route_port_t::reply`, `framework/src/runtime/backend/raw_route_port.cpp:496` | Framework target | 미측정 | 4.969 | 아니오 | handler reply를 raw route로 전송 |
| `route_handler_invoker_t::invoke`, `framework/src/runtime/channels/route_handler_invoker.cpp:10` | Framework target | 미측정 | 2.995 | 아니오 | 준비된 route handler invocation |
| `raw_route_port_t::receive_if_ready`, `framework/src/runtime/backend/raw_route_port.cpp:459` | Framework target | 미측정 | 2.045 | 부분 | receive readiness에 따라 binding part 수신 |
| `mesh_node_host_service_t::dispatch_application`, `framework/src/runtime/mesh/mesh_node_host_service.cpp:3664` | Framework target | 미측정 | 2.013 | 아니오 | 수신 application을 dispatch 경로로 전달 |
| `route_packet_dispatcher_t::dispatch_request`, `framework/src/runtime/channels/route_packet_dispatcher.cpp:347` | Framework target | 미측정 | 2.007 | 아니오 | route request packet 분류·dispatch |
| `service_mailbox_t::try_enqueue`, `framework/src/runtime/mesh/service_mailbox.cpp:79` | Framework target | 미측정 | 1.010 | 아니오 | ready/receive record를 service mailbox에 수락 |
| `async_result_t::awaiter_t::await_suspend`, `bindings/cpp/include/zlink/Contracts/Messaging/operation_contracts.hpp:85` | Binding caller | 미측정 | 2.645 | 예 | binding async operation await 등록 |
| `zlink_raw_driver_t::make_parts / message_t construction`, `framework/bench/grpc/cpp/client/bench_cpp_client.cpp:889` | Binding caller / benchmark caller | 미측정 | 2.028 | 예 | binding benchmark 메시지 part 준비 |
| `std::string::_M_mutate`, `framework/bench/grpc/cpp/client/bench_cpp_client.cpp:889` | Binding caller / benchmark caller | 미측정 | 2.018 | 예 | benchmark routing/header string 구성 |
| `request_submit_operation_t::async`, `bindings/cpp/src/Runtime/Messaging/request_reply.cpp:102` | Binding caller | 미측정 | 1.021 | 예 | physical request 제출 및 async operation 생성 |
| `completion_entry_t::capture`, `bindings/cpp/src/Runtime/Messaging/completion_owner.cpp:377` | Binding caller | 미측정 | 1.000 | 예 | binding completion callback state 보존 |

표의 line은 현재 worktree에서 해당 function definition 또는 핵심 구현을 확인한 위치다. allocation stack 자체는 기존 capture의 function-level attribution을 따른다.

### Framework-only와 shared site 구분

- Framework-only group: public request task/receiver, channel runtime, service-wire encoder/decoder, operation registry, Framework receive/dispatch/mailbox/reply 경로. Binding benchmark에는 이 Framework server pipeline이 없다.
- Shared binding/Core-facing group: raw route adapter/request boundary와 binding `request_submit_operation_t` / `completion_entry_t`. 이름과 소유 abstraction이 다르므로 같은 site로 합치지 않았다.
- `Core native (no Framework/binding frame)`는 Framework target allocation stack의 별도 row였으며 2.170 calls/request였다. Framework file/function에 귀속하지 않았다.
- Mutex site group은 call-event stacks가 수집되지 않아 분리할 수 없다. `chand`의 caller/receive/worker/other mutex 개수는 role별 합계이지 source-function별 계수가 아니다.

## 검증 및 제한

- Framework 새 측정: 248,171 완료 / target received 248,171 / 오류 0 / in-flight 0.
- Binding 새 측정: lock을 얻기 전에 대기 중이었다. 유효한 count가 없다.
- Allocation-site stacks: 별도 기존 capture; 1/256 표본이라 rare site에는 표본 오차가 있다. 이 capture는 이번 branch/head 측정이 아니다.
- Mutex call-site stacks와 per-site locks/request: **수집되지 않았으므로 요청한 80% lock attribution 및 결합 call-site 표는 미완료**다. 이를 완성하려면 다음 동일-condition run에서 mutex interposer가 role별 sampled stack을 남겨야 한다. 현재 보고서에는 대체 추정치를 넣지 않았다.
- 기존 first-attempt binding HTTP 409는 source readiness 확인 전에 start를 보낸 것이 원인이었다. 수정한 runner는 readiness 대기 후 실행하도록 바꿨으나 공유 lock 대기 때문에 아직 측정되지 않았다.
- 테스트는 요청되지 않아 실행하지 않았다. Worktree는 읽기 전용으로 유지했다.

리팩토링 점검: 성능 2건, POSDDD 0건, 불필요 코드 0건 발견. 고친 항목 없음, 넘긴 항목: binding 동일 계수 run과 role별 mutex call-site stack attribution.
