# m1672c 완료 보고 — #1672 C++ 메시지당 비용

worktree `/root/worktree/zlink-1639-msg-cpp`, 브랜치 `framework-cpp/1639-per-message-cost`. 계획은 `m1672c-plan.md`. 감독자가 승인한 항목 ③→②→④→⑤→⑥을 순서대로 구현했다. ①(§4.4 hand-off), ⑦·⑧·⑨는 보류 지시대로 손대지 않았다.

## 1. 커밋

| sha | 내용 |
|---|---|
| `959463f5fd` | WIP `d9d6d6b86d` revert. 배포 Core에 EVENTS POLLIN 수정(`1ad2b0439c`)이 없으므로 Core 1.20 배포 뒤 다시 넣는다 |
| `cf22f0051f` | ③ 수신 회전마다 ROUTER poll 1회, admitted peer를 snapshot 참조로 읽기 |
| `e7b0f9163f` | ② node request 제출 경로의 pass-through 층 제거 |
| `f383f21609` | ④ `c7152b790a`(#1645) 재적용: binding reply message를 completion까지 유지 |
| `01d214c869` | ⑤ 해석한 application part들을 retained message 하나로 소유 |
| `45a8e62c8b` | ⑥ Node direct target을 local topology로 판정(Location Store 조회 제거) |
| `655699c867` | origin/main `d2489ac50f` merge(#1814, #1807). 충돌 해소는 §6 |

모든 커밋 본문에 `Refs #1672`, 해당 항목에 `Refs #1645`·`#1643`·`#1641`, 끝 줄 `Co-Authored-By`.

## 2. 계측 방법

시간 측정은 하지 않았다. 횟수만 셌다. 계측 코드는 커밋하지 않았다(`/root/jobs/fx/cx/m1672c/harness/`).

- driver `count_driver.cpp`: RouteMesh `bench` 경로(bench client의 `request_to_node`/`send_to_node`와 같은 public API)를 server·client 두 process로 실행한다. 1024B typed JSON payload, warmup 2000건 뒤 10000건 구간.
- `libcount_preload.so`(LD_PRELOAD): `malloc/calloc/realloc/aligned`, `pthread_mutex_lock`(+성공한 trylock), `pthread_cond_signal/broadcast`, futex syscall, `getrusage` 자발·비자발 context switch를 구간 전후로 센다. 메시지당 값 = 구간 차 / N.
- 항목별 수: 구간 안 이벤트를 1/31로 표본 추출해 stack을 남기고(`symbolize.py`, `items.py`), 해당 함수가 stack에 있는 표본 비율 × 메시지당 합계로 낸다. 표본 비율 기반이라 1건 미만 차이는 잡음이다.
- thread 전환은 프로세스 자발 context switch(vcsw)로 본다. 단계별(E1–E5, I0–I4, W1–W4) thread id 계측은 이번에 만들지 않았다. 바뀐 항목 중 전환 수를 바꾸는 것이 없어서다(①이 보류됨).

## 3. 전/후 횟수

### 3.1 전체(메시지당)

| 셀 | 측 | alloc 전→후 | mutex 전→후 | cond signal 전→후 | vcsw 전→후 |
|---|---|---|---|---|---|
| request-serial | client | 422.5 → 372.3 | 299.7 → 278.5 | 46.0 → 40.0 | 8.0 → 7.9 |
| request-serial | server | 355.8 → 336.7 | 302.8 → 283.7 | 34.0 → 31.0 | 4.5 → 4.5 |
| send(직렬) | client | 100.3 → 66.5 | 46.0 → 42.4 | 10.0 → 8.0 | 0.5 → 0.5 |
| send(직렬) | server | 102.0 → 116.6 | 38.8 → 59.1 | 4.4 → 7.0 | 0.3 → 0.6 |

"전"은 merge 커밋 `49a3f458ab` 기준 binary, "후"는 main merge 뒤 `655699c867`. send server 값은 수신 회전 하나가 몇 건을 묶는지에 따라 run마다 크게 흔들린다(같은 binary에서 102–148). 그래서 send server는 판정에 쓰지 않는다. 같은 원인으로 server send의 회전당 관리 작업 비용이 건수로 나뉘는 정도가 달라진다(§5 ⑪).

### 3.2 항목별

| 항목 | 지표(메시지당) | 전 → 후 |
|---|---|---|
| ③ 수신 회전 | server 수신 경로(`pump_one`/`receive_next`/`begin_receive_turn`) alloc | 27.9 → 9.7 |
| | 같은 경로 mutex | 40.6 → 29.0 |
| | peer·local descriptor 복사 alloc | 11.8 → 0.0 |
| | 판정: readiness | record마다 1 → 회전마다 1 |
| ② 제출 층 | client 제출 thread alloc | 201.5 → 195.4 |
| | 같은 thread mutex | 82.5 → 73.6 |
| | MeshNode lambda~raw owner turn 사이 coroutine frame | 8 → 5 |
| ④ reply 복사 | reply payload materialization(정적) | 3 → 1 |
| | reply 유효성 decode alloc | 5.3 → 0 |
| | request observer alloc | 14.0 → 9.3 |
| ⑤ part 소유 | `decode_application_parts` alloc(정적, part N개) | 2+2N → 2+N |
| | server 측 측정 | 4.1 → 2.6 |
| ⑥ target 분류 | client 제출 thread 분류 alloc | 32.9 → 0 |
| | 같은 분류 mutex | 6.7 → 1.0 |

## 4. 항목별 소유·스펙·대조·분류

| 항목 | 소유 계층 | 스펙 조항 | 교차언어 대조 | 분류 | 판정 위치 전/후 |
|---|---|---|---|---|---|
| ③ | Framework ingress owner(raw MeshNode owner, host 수신 회전) | 08-messaging-hot-path `:183–184`(I2·I3) | Java `ZLinkJavaRawServicePort.java:348–352`은 아직 record마다 poll(감독자가 Java job에 전달). Java `ZLinkJavaRawMeshNode.java:4762`, .NET `ZLinkManagedMeshNode.cs:6499`은 peer를 복사 없이 읽음 | B | readiness 판정 record당 1 → 회전당 1 |
| ② | Framework 요청 제출(MeshNode transport, host, raw owner) | 08 `:94–97`, `:120–127` | .NET `ZLinkRouteClient.cs:447`, Node `channel-transports.ts:497`은 요청당 operation 하나, Java `ZLinkChannelCallRuntime.java:190–205`은 CompletionStage 하나 | B | 제출 층 frame 8 → 5, 제출 payload materialization 2 → 1 |
| ④ | Framework payload codec·operation owner | 05-payload-ownership §2, 08 `:94`·`:98`, 01-submit-and-completion §10·§11 | Java `:1716`, .NET `:5953`, Node `raw-service-mesh-runtime.ts:1337`가 받은 frame에서 reply header를 읽음 | B | reply materialization 3 → 1, terminal owner 1 → 1 |
| ⑤ | Framework payload codec | 05 §2, 08 `:184`·`:218` | Java `:4901–4903`, .NET `:6444–6503`, Node `:844`. 관리형 메모리라 part별 owner 할당에 대응하는 것이 없음 | B | 해석한 payload당 owner 기록 N+1 → 1 |
| ⑥ | raw MeshNode owner topology | 02-channel-messaging §8(Object Client Node direct target은 NotFound), 08 `:95`, `:120–127` | Java `ZLinkJavaRawMeshNode.java:7560–7574` `isObjectClientNodeDirectTarget`와 같은 판정(peer object role, not-required peer). .NET `ZLinkMeshChannelSelection.cs:30` | B | 메시지마다 Location Store page 순회 → topology 읽기 1회 |

⑥의 동작 변화: admitted도 아니고 not-required로도 알려지지 않은 target은 더 이상 Location Store에서 찾지 않는다. send는 기존처럼 `not_connected`, request는 `not_found`로 끝난다. Java도 같다. 연결되지 않은 Object Client target의 send 결과를 스펙 §8에 맞추려면 Location Store 없이는 role을 알 수 없으므로, 네 언어 공통으로 판단할 일이다(스펙 개정 후보, 아래 §7).

⑤에서 제외한 부분: service-wire header가 ingress 분류(`raw_mesh_node_owner.cpp` `receive_next`)와 worker(`public_host_runtime.cpp:6366` 부근 `dispatch_application_claim`)에서 두 번 해석된다(#1643). node record의 두 번째 해석은 할당이 없다. 분류 결과를 mailbox record에 싣는 수정을 만들었으나 mailbox record를 직접 만드는 생산자·시험 fixture(`test_cpp_framework_m6b_runtime.cpp:1510`, `:4544`, `:4711` 등)의 계약이 함께 바뀌어 되돌렸다. envelope JSON header는 측마다 한 번만 해석한다(`envelope_codec.cpp:804–815`, server 약 10.7 alloc/메시지).

## 5. 남은 예산 초과(file:line, 수정 안 함)

예산: alloc ≤4, mutex ≤3, thread 전환 ≤0.1. 아래 수치는 최종 binary의 표본 기반 메시지당 값이다.

| # | 위치 | request-serial 메시지당 | 비고 |
|---|---|---|---|
| ⑪ 새 발견 | 수신 회전마다 관리 작업 전체 실행: `public_host_runtime.cpp:6101–6116`(`dispatch_ready`의 `retry_terminal_relays`, `reap_terminal_tombstones`, `tick_liveness`, `expire_requests`, `dispatch_user_spot_operations`), `mesh_node_host_service.cpp` `next_receive_activity_async`, `advance_spot_management_async` | server alloc 약 216, mutex 약 159 / client alloc 약 114, mutex 약 105 | 직렬 부하에서는 회전 1개 = 메시지 1개라 회전당 관리 비용이 그대로 메시지당 비용이 된다. 08 §4.2는 회전당 한 번을 허용하지만 기한이 안 된 작업의 확인이 할당·lane turn을 쓴다. **가장 큰 남은 항목. 승인 필요** |
| ⑫ 새 발견 | ambient flow 전파: `flow_context.cpp:61–82` `capture_ambient_flow`/`enter_ambient_flow`. 기본 message flow 모드가 `errors`(`execution.hpp:59`)라 요청마다 flow가 생기고, task 등록·재개마다 snapshot `make_shared`와 flow id 문자열 복사가 일어난다 | client alloc 약 50–65 | trace off에서는 비용이 없다. 기본값(errors)의 비용이다. 해결하려면 ambient hook을 할당 없는 형태로 바꿔야 한다(`task.hpp:186–257` detail hook 변경). **승인 필요** |
| ② 잔여 | `request_task.hpp:92–125`: `request_task_t` coroutine마다 promise·public shared state·admitted source·observer 2개. route client 경로에 3층 | client 약 20–30 | public header의 request_task 구조 변경이 필요 |
| ② 잔여 | `channel_runtime.cpp:70–83`, `:1767` transport 조회 lane turn | alloc 1, mutex 3.6 | bind 시점 snapshot으로 바꾸면 사라짐 |
| ① 보류 | I4·E5 thread 전환(`mesh_node_host_service.cpp:2558, 2567`, `operation_registry.cpp:102–232`) | 왕복당 2 | 감독자 설계 지시 대기 |
| ⑦⑧⑨ 보류 | 계획서 §4 그대로 | | |

## 6. main merge(`655699c867`) 충돌 해소

origin/main `d2489ac50f`(#1814 m6a ready·replay, #1807 owner generation·relocation)를 merge했다. 모든 충돌은 main 의도로 풀고 브랜치 변경을 그 위에 다시 맞췄다.

| 파일 | 해소 |
|---|---|
| `framework/src/runtime/mesh/raw_mesh_node_owner.cpp` | `infrastructure_request_retry_state_t`(브랜치 쪽 75ms timer 재시도)를 main의 `observe_durable_infrastructure_attempt`/`run_durable_infrastructure_request`로 대체했다. 그 안의 reply 처리만 ④의 타입에 맞췄다: `result.parts`는 `zlink::message_t`이므로 header는 `protocol::message_bytes`로 읽고, `decode_reply`에는 `copy_binding_parts (result.parts)`, `operations->complete`에는 `materialize_binding_part (...)`를 넘긴다. infrastructure reply의 복사 횟수는 merge 전 브랜치와 같다 |
| `framework/src/runtime/mesh/service_topology_registry.cpp` | main의 snapshot holder(`_topology.get ()`)를 쓰고, ③의 aliasing pointer 반환은 유지했다 |
| `tests/.../test_cpp_framework_m6b_runtime.cpp` | main의 `ready_peer (...)` 대기를 채택했다 |
| `tests/.../test_cpp_framework_m6c_runtime.cpp`, `test_cpp_framework_mesh_metrics.cpp`(2곳), `test_cpp_framework_route_mesh_weight_update.cpp`(2곳) | main의 `peer && peer->ready` 대기를 채택했다(shared_ptr에서도 같은 식) |
| `tests/.../test_cpp_framework_m6a_runtime.cpp`(충돌 없음, 의미 충돌) | main이 넣은 `verify_durable_attempt_waits_for_admit`의 callback 인자가 `std::vector<std::uint8_t>`였다. ④로 바뀐 registry callback 타입(`zlink::message_t`)에 맞추고, 비교에 쓰는 값은 `payload.to_bytes ()`로 넘겼다. 단언은 바꾸지 않았다 |

## 7. 시험

| 시험 | 결과 |
|---|---|
| C++ unit·contract 전체(`ctest -L '^(unit|contract)$'`), 항목마다 | ③ 155/155, ② 155/155, ④ 155/155, ⑤ 155/155(첫 시도 판정 변경본은 3건 실패해 되돌림, 아래), ⑥ 155/155(첫 판정본은 `route_payload_ownership`의 Object Client 단언에서 실패 → Java `isObjectClientNodeDirectTarget`과 같은 role 판정을 넣어 통과) |
| merge 뒤 전체 | 155/155(`logs/final-ctest.log`) |
| main 대비 새 실패 | 0. 앞선 main 기준(g1672m, 같은 C++ 코드) 155/155이고 브랜치도 155/155 |
| format | clang-format 18.1.8로 브랜치 변경 C++ 파일 `--dry-run -Werror` 통과, `git diff --check` 통과 |
| TSan(전후) | §7.1 |

간헐 실패: ⑤의 되돌린 변경본 실행에서 `test_cpp_framework_route_client_channel_routing`(`OneWayPublishDoesNotCreateCorrelation`, pubsub)이 1회 실패하고 재실행에서 통과했다. 나머지 실행에서는 모두 통과했다. 바뀐 코드 경로(RouteMesh)와 무관한 SUB 수신이지만 원인은 조사하지 않았다.

### 7.1 TSan

- 빌드: `-fsanitize=thread -O1 -g`, 바뀐 코드를 쓰는 15개 시험(m6a·m6b·m6c, operation_registry, lane_roundtrip, raw_route_port_contract, channel_messaging, mesh_metrics, route_mesh_weight_update, route_payload_ownership 등). 기준은 `/root/worktree/zlink-main-base`(C++ 코드는 main과 같음). 빌드 디렉터리 `/root/jobs/fx/cx/m1672c/tsan/`.
- 이 머신의 kernel 6.8 ASLR(mmap_rnd_bits)에서 gcc 13 TSan은 시작하지 못한다. `setarch -R`로 실행했다(시스템 설정은 바꾸지 않았다).
- 결과: 양쪽 모두 같은 13개 시험이 TSan 보고로 실패한다(head·base 실패 집합 동일). 보고 수 head 354, base 305. 이 중 head 353건, base 304건은 계측되지 않은 `libzlink.so`(배포 Core) frame을 포함한다. Core message refcount(`shared_message_memory_allocator::call_dec_ref`)가 TSan에 보이지 않아 생기는 종류다. head에서 늘어난 첫 위치 `raw_route_port.hpp`(6)·`operation_registry.cpp`(4)는 모두 이 종류이며, ④가 binding message를 completion까지 유지해 Core 해제 frame이 Framework 객체 소멸 쪽으로 옮겨진 결과다. Core frame이 없는 보고는 양쪽 각 1건이다.
- 판단: 이 구성으로는 ④·⑤의 message 수명 변경을 TSan으로 판정할 수 없다. 계측된 Core(`ZLINK_CORE_SOURCE=local`)로 TSan을 다시 돌려야 한다. 이번에는 하지 않았다.

## 8. 스펙 개정 필요

- 연결되지 않은 Object Client RID로 보낸 Node direct send의 결과(02-channel-messaging §8은 NotFound, 현재 C++·Java는 `not_connected`/`ROUTE_NOT_CONNECTED` 경로). Location Store 없이 판정할 수 없으므로 판정 근거(local topology만 볼지)를 네 언어 공통으로 정해야 한다. 제안 문안: "Node direct target의 Object role은 local topology(admitted peer descriptor, not-required peer)로만 판정한다. 둘 다 아니면 role을 모르는 target으로 보고 연결 오류로 끝낸다."

## 9. 리팩토링 점검

리팩토링 점검: 성능 5건·POSDDD 1건(Node direct 분류를 Location Store에서 topology owner로 옮김)·불필요 코드 2건(`request_to_target`, `classify_node_direct_target` 삭제) 발견·수정, 넘긴 항목 ⑪ 회전당 관리 작업(`public_host_runtime.cpp:6101–6116`), ⑫ ambient flow 전파(`flow_context.cpp:61–82`), ② 잔여 request_task 층(`request_task.hpp:92–125`)·transport lane(`channel_runtime.cpp:70–83`), ⑤ 잔여 worker의 wire header 재해석(`public_host_runtime.cpp:6366`).

## 10. 산출물

- 로그: `/root/jobs/fx/cx/m1672c/logs/`(빌드·ctest·계측 로그)
- 계측: `/root/jobs/fx/cx/m1672c/harness/`(`count_driver.cpp`, `count_preload.c`, `run.sh`, `symbolize.py`, `items.py`, `out/`)
- TSan: `/root/jobs/fx/cx/m1672c/tsan/`(`tsan-head.log`, `tsan-base.log`, `loc.py`)
- 계획서 `m1672c-plan.md`의 "worktree Core로 검증" 표현은 감독자 판정대로 정정했다(§2.3, §6).

## 11. 감독자 리뷰(review-1672.md) 뒤 추가 작업

| sha | 항목 | 내용 |
|---|---|---|
| `a6b373d8d9` | C-F1 | topology가 아는 target(admitted peer의 object role, not-required peer)은 지금처럼 topology로 판정하고, 어디에도 없는 target만 기존 Location Store 판정(`classify_unknown_node_target`)으로 넘긴다. 정상 경로에 조회 없음. 시험 `RoutePayloadOwnership.UnconnectedObjectClientNodeDirectTargetIsNotFound`: 연결 안 된 Object Client RID의 send·request 모두 NotFound, Store 조회 2회 |
| `6d03dd1e00` | C-F2 | completion dispatcher `state_t` 소멸자가 pending에 남은 chain을 삭제(각 item 소멸자가 drain owner를 놓음) |
| `46660e1c94` | ⑪ | 관리 소유자 다섯(liveness registry, relocation replay coordinator, host relocation terminal 상태, Actor transfer coordinator, node Actor 관리 상태)이 자기 lane에서 다음 기한의 이른 경계를 publish하고 atomic load 하나로 읽는다. 기한을 앞당길 수 있는 lane turn은 경계를 due-now로 되돌리고, 이벤트로 진행하는 작업(미정산 seal, 남은 remote leave·source cleanup)이 있으면 due-now를 유지한다. 구성 연산 사본은 infrastructure record가 있을 때만 읽는다. 새 timer·lock·cache 없음 |
| `5b1a4ac738` | ② 잔여 | Node direct transport 조회를 bind·route channel 초기화 시점 불변 index(atomic shared_ptr load)로. route channel 이름은 우선권을 유지하고 lane에서 해석. mesh transport map은 index로 대체(소유자 하나) |
| `f4f2255174` | ⑫ | 스펙 S2(04-flow-correlation §4 :91–94): flow 생성·inbound flow 읽기·capture·전파는 Normal·Detailed에서만. 판정은 `flow_context_t::tracing` 하나, flow 관련 지점은 tracer `flow_tracing ()`으로. Errors 기록 내용 gate(`capture_enabled ()`)는 그대로. 기존 Errors 동작을 단언하던 session seal relay 시험 2건은 Normal을 설정하도록 바꿨다(스펙 §4 :91–94 근거; `set_message_flow_mode`는 apply 전에 설정값으로 덮인다) |
| `814ee0a710`(앞서), `a350159aa8` | merge | origin/main `adfdff424a`(#1823 포함) merge. 충돌 없음 |

C-F1 교차언어: Java `ZLinkJavaRawMeshNode.java:1411–1413`은 topology에 없는 target을 `TARGET_NOT_FOUND`로 끝낸다(peer intent가 없는 경우). 연결 안 된 Object Client에 대한 gap은 Java의 이 경로에는 없다. intent가 있으나 admitted가 아닌 Object Client는 `:1392–1405`에서 empty(진행)로 남는다.

### 11.1 전/후 횟수(메시지당, request-serial, 기본 Errors)

| 단계 | client alloc | client mutex | server alloc | server mutex |
|---|---|---|---|---|
| ⑪ 전(`6d03dd1e00`) | 372.3 | 278.5 | 336.8 | 282.9 |
| ⑪ 후 | 285.5 | 210.3 | 180.7 | 166.4 |
| ② 잔여 후 | 284.7 | 207.7 | 180.9 | 166.7 |
| ⑫ 후 | 203.5 | 207.3 | 175.7 | 166.4 |

- ⑪ 항목별(server): relay retry 17.5→4.6, tick_liveness 30.8→2.5, `dispatch_user_spot_operations` 84.6→5.4, `next_receive_activity_async` 76.6→14.5 alloc.
- ② 잔여: 제출 thread transport 조회 alloc 1.0→0, mutex 4.1→0.6.
- send(직렬): client 66.5/42.4 → 63.3/39.5. server send는 회전 묶음에 따라 흔들린다(91–117).
- 처음 기준(`49a3f458ab`)에서 최종까지: client alloc 422.5→203.5, mutex 299.7→207.3; server alloc 355.8→175.7, mutex 302.8→166.4. thread 전환(vcsw)은 그대로(① 보류).

### 11.2 시험

- 항목마다 C++ unit·contract 전체: C-F1/C-F2 155/155, ⑪ 155/155(중간 두 번 실패를 고침: m6b `verify_actor_commit_is_replayable_until_deadline`이 coordinator `next_activity`의 정확한 값을 단언 → never-due일 때만 lane 생략, 아니면 정확히 계산; `verify_pending_relocation_management_bounds_dispatch_wait`가 `time_point::min ()`을 기대 → due-now 표식을 now로 바꾸던 처리 제거. 단언은 바꾸지 않음), ② 잔여 155/155, ⑫ 155/155, merge 뒤 155/155.
- #1813 `ActorMaintenanceJoin.*` 50회 반복: 50/50 통과(`logs/maint-join-repeat.log`).
- format: clang-format 18.1.8 `--dry-run -Werror` 통과, `git diff --check` 통과.

### 11.3 TSan(계측 Core·binding·Framework)

- Core(`libzlink`, `libzlink-static`)와 C++ binding을 `-fsanitize=thread`로 빌드해 `tsan2/pkg`에 두고, Framework 시험 10종(15 ctest)을 head(`5b1a4ac738` 소스)와 base(main `d2489ac50f` 소스, `git archive`)로 각각 빌드했다. `setarch -R`로 실행.
- 결과: head 보고 80건(8 위치), base 81건(8 위치). **head 전용 보고 위치 0**(첫 repository frame의 함수·파일 기준, `tsan2/loc.py`). 실패 시험은 양쪽 공통 3건(m6b, m6c, channel_messaging). head의 owner_rejection_observability abort는 동시 빌드 부하에서 4,200건 요청의 30초 timeout이었고, 단독 재실행 head 2/2·base 2/2 통과.
- ⑫는 TSan 대상 빌드 이후 커밋이다. flow 상태는 thread_local이라 공유 상태가 늘지 않는다.

### 11.4 사고 보고: 공유 Core cache를 덮어씀(복구함)

- 19:5x–20:26 사이 TSan Core 준비 스크립트가 `.artifacts/wsl/install/zlink-core/1.19.0`을 `cp -a`로 복사했는데, 이 경로가 `~/.cache/zlink/core/1.19.0/linux-x64`를 가리키는 **symlink**여서, 이어진 include·`libzlink.so`·`libzlink.a` 교체가 **머신 공용 Core cache**에 쓰였다. 그 사이 이 cache를 쓰는 모든 job의 C++ 시험·빌드는 TSan Core를 링크했다(실행 시 `ThreadSanitizer: unexpected memory mapping` FATAL).
- 20:26에 `scripts/local-package/core/fetch-release.sh --version 1.19.0 --force`로 배포본을 다시 받아 복구했다. `libzlink.so.1.19.0` sha256 `b3e67363…`가 provenance와 일치한다. 이후 TSan prefix는 symlink를 따르지 않는 실제 디렉터리로 다시 만들었다.
- 이 시간대에 다른 job이 돌린 C++ 시험 결과는 다시 확인해야 한다.

## 12. 2차 리뷰 뒤 작업

| sha | 항목 | 내용 |
|---|---|---|
| `375292c208` | C-F3 | host relocation terminal 상태에서 deadline·작업을 쓰는 모든 lane turn이 같은 turn에서 `lower_relocation_next_due_on_lane`(규칙 하나)으로 bound를 낮춘다. `next_finalize_at`의 쓰기는 `set_relocation_finalize_at_on_lane` 하나로 모았다. 시험 `verify_relocation_finalize_write_lowers_published_bound`(m6b): 미예약 attempt 삽입 → bound 게시 → finalize 쓰기 → 그 시각에 due. 낮추는 줄을 빼면 `:3957`에서 실패함을 확인(`logs/cf3-red-run.log`) |
| `546bad8135` | C-Q1 | `channel_runtime_manager.cpp:186`·`:202`(route channel 초기화)는 state lane 밖, `channel_runtime.cpp:758`(transport bind)은 lane 위다. 같은 lane이 아니므로 publication을 CAS 루프로 바꿨고, bind 쪽 변경은 재시도해도 같도록 입력을 복사한다 |
| merge | main `aadc8dead1`(#1825) | 충돌 없음, C++·Core·binding 변경 없음 |

### 12.1 bound에 들어가는 deadline을 더 이르게 만들 수 있는 쓰기 전수(rg)

| 소유자 | 쓰기 file:line | 성격 | bound 처리 |
|---|---|---|---|
| liveness registry | `service_liveness_registry.cpp:44` peer 삽입(deadline=now+timeout, next_probe=now) | 새 기한 | `:46` unknown(due-now) |
| | `service_liveness_registry.hpp:60` deadline CAS | 늦추기만(max) | 불필요 |
| | `service_liveness_registry.cpp:164` next_probe += interval | 늦추기만 | 같은 turn `:138` 재게시 |
| relocation replay coordinator | `raw_stateful_dispatch.cpp:1303–1304` target 삽입(next_retry=min) | 새 기한 | `:1305` due-now |
| | `:1345` next_retry = now+interval | 늦추기만 | 같은 turn `:1347` 재게시 |
| | `:1465–1467` source completed·completed_at | 새 기한(tombstone) | `:1468` due-now |
| | `:1553`, `:1610` acknowledging=false | 대상 복귀 | `:1554`, `:1611` due-now |
| | `:1532`, `:1588` acknowledging=true | 제외만 | 불필요 |
| host relocation terminal | `public_host_runtime.cpp:1895` seal 삽입 | 새 기한·이벤트 | `:1896` lower(due-now) |
| | `:2969–2972` shutdown seal | discard 필요 | `:2972` lower(due-now) |
| | `:3116`, `:3564`, `:3868`, `:5324` next_finalize_at | 기한 | `set_relocation_finalize_at_on_lane`(hpp `:1196–1200`) |
| | `:3845` attempt 삽입 | 새 항목 | `:3844` lower(due-now) |
| | `:5151` assembly expires_at 연장 | 늦추기(그래도 lower) | `:5153` lower |
| | `:5163` assembly 삽입 | 새 기한 | `:5162` lower(due-now) |
| | `:5297` cutover_received | 상태 변화 | `:5298` lower(due-now) |
| | `:3199` seal ready=true | 이벤트 항목 제외 | 불필요 |
| actor transfer coordinator | `actor_transfer_coordinator.cpp:159–161`, `:165–166` reconcile 기한 | 새 기한 | `:162`, `:167` due-now |
| | `:398` follow route 추가 | 새 기한 | `:399` due-now |
| | `:421` remove_at = max | 늦추기만 | 불필요 |
| | `:886–887` target_pending admission | 새 기한 | `:888` due-now |
| | `:1039`, `:1175` completed admission | 새 기한 | `:1040`, `:1176` due-now |
| | `:1145–1146` phase=reconcile | 새 기한 | `:1147` due-now |
| | `:183` next_reconcile_at(재시도) | 늦추기만 | 같은 turn `:187` 재게시 |
| | `:101`, `:104`, `:118`, `:951` 다른 phase | bound 대상 아님 | 불필요 |
| spot management state | `spot_runtime.cpp:5747`, `:15209` handoff 요청 삽입 | 새 기한 | `:5749`, `:15211` due-now |
| | `:9719` remote leave 삽입 | 이벤트 | `:9717` due-now |
| | `:10018` source cleanup 삽입 | 이벤트 | `:10016` due-now |
| | `:8259`, `:8298`, `:9817`, `:9881` leave_completed | cleanup 기한 앞당김 | cleanup이 남아 있는 동안 bound가 due-now로 고정(`publish_management_next_due_on_lane`) |

모든 쓰기는 그 상태를 소유한 lane의 turn 안이다(spot `:5673`, `:9693`, `:10004`, `:15206`; host `:1880`, `:2969`, `:5146`, `:5262`). 판정 위치는 소유자마다 `due ()` 하나로 그대로다.

### 12.2 시험

- C++ unit·contract 전체: C-F3·C-Q1 후 155/155, main merge 뒤 155/155(`logs/final2-ctest.log`). format `--dry-run -Werror` 통과.
