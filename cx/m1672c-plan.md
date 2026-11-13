# m1672c 계획 — #1672 C++ 메시지당 비용

worktree `/root/worktree/zlink-1639-msg-cpp`, 브랜치 `framework-cpp/1639-per-message-cost`. 이 문서는 진행 단계 1(계획)의 결과다. 코드 수정은 하지 않았다(main merge 커밋 하나만 만들었다).

## 0. 요약

- main merge: `49a3f458ab` (parents `52ee330842`, `90aafafaff`). main 쪽 변경은 Java 5파일뿐이라 충돌·의미 충돌이 없다. 본문 `Refs #1672`.
- WIP 시험(`d9d6d6b86d`)의 실패 원인은 Framework 코드가 아니다. C++ Framework 빌드가 링크하는 Core가 **배포된 Core 1.19.0(core/v1.19.0, source `136a6d2bff`)** 이고, 이 배포본에는 EVENTS POLLIN 수정 `1ad2b0439c`가 들어 있지 않다. worktree에서 빌드한 Core(`core/build-dev`, 수정 포함)를 `LD_LIBRARY_PATH`로 바꿔 끼우면 m6a_runtime과 `--owner-rejection-observability` 둘 다 통과한다(§2). 제안은 **이 브랜치에서 제거하고 Core 1.20 배포 뒤 별도 브랜치로 다시 올리는 것**이다.
- 비용 항목은 10개다(§4). 큰 순서: ① 왕복당 실행 자원 전환 2회(§4.4 인계 미구현) ② 제출 경로의 task·coroutine 계층과 lane turn ③ 수신 회전의 record별 poll·descriptor 복사 ④ reply 완료 경로의 payload 복사 3회·재해석 ⑤ worker claim의 header 재해석·part별 할당 ⑥ node 직접 대상 분류의 Location Store 조회 ⑦ route channel 요청의 worker 왕복 ⑧ Yield turn mutex ⑨ Actor 깊은 대기의 scheduler hop ⑩ #1645 브랜치 잔여 커밋.
- "스펙 개정 필요" 항목은 없다. ①은 이미 머지된 스펙 변경 ②(PR #1698, §4.4)를 구현하는 일이고, 나머지는 현재 스펙에 맞추는 수정이다. ⑨는 진단이 더 필요하다(§4 ⑨).
- 횟수는 코드 읽기로 센 **정적 계수**다. 이 머신에는 C++ 계측 harness가 없다(이전 C++ 계측 m1639c는 다른 머신). 단계 2의 첫 일로 §3의 계측을 만들고, 그 수로 순서를 다시 확인한 뒤 수정한다.

## 1. main merge

| 항목 | 값 |
|---|---|
| merge 전 HEAD | `52ee330842` |
| merge한 main | `90aafafaff` (main은 `52ee330842` 이후 2커밋: `09da8bbc52` Java lane completion 수정과 그 PR merge) |
| 변경 파일 | `framework/languages/java/...` 5개. C++·binding·Core 변경 없음 |
| 충돌 | 없음. C++ 쪽 의미 충돌도 없다(main이 C++을 건드리지 않았다) |
| merge 커밋 | `49a3f458ab`, 본문 `Refs #1672`, 끝 줄 `Co-Authored-By` |

merge 뒤 C++ 빌드 tree는 merge 전 내용과 같으므로(C++ 변경 0) 아래 시험은 그 binary로 했다.

## 2. WIP 커밋 `d9d6d6b86d` 시험 판정

### 2.1 시험이 검사하려는 것

`test_cpp_framework_m6a_runtime.cpp:2230–2249`(첫 구간)과 `:2365–2373`(paused liveness 구간)이 대상이다.

- host 공유 permit이 없을 때 ordinary ROUTER의 record를 Core에서 받지 않는다는 단언(`pump_one (now, false) == no_data`, `last_pump_bytes () == 0`, application mailbox 0)은, DATA가 **이미 Core에 도착한 상태**에서 해야 의미가 있다. 도착 전이면 `no_data`가 당연히 나와서 단언이 아무것도 검사하지 않는다.
- 그래서 WIP는 Core readiness를 receive 없이 관찰하는 `raw_mesh_node_owner_t::ingress_data_ready ()`(`raw_mesh_node_owner.cpp:3441–3454`, binding `common_socket_options_t::events ()` → `ZLINK_OPT_EVENTS`)를 넣고, 도착을 확인한 뒤 permit 없는 pump를 하고, pump 뒤에도 readiness가 남아 있는지(소비하지 않았는지) 검사한다.
- 두 번째 구간은 liveness probe가 application mailbox에 들어가지 않고 I3에서 내부 처리되는지(`last_pump_bytes () != 0`, application mailbox 0)도 검사한다. 이를 위해 앞선 두 application payload의 claim/release를 paused 구간 앞으로 옮겼다(payload 보존 검사는 그대로 있다).

### 2.2 스펙 근거

- 08-messaging-hot-path §4.1 I1(`08-messaging-hot-path.ko.md:182`): permit이 없으면 ordinary receive를 멈춘다.
- Application job queue 「3」 ordinary ingress permit 순서, I3(`:184`): control record는 I3에서 처리하고 application queue에 넣지 않는다.
- Core socket 공통 README의 `ZLINK_OPT_EVENTS`(읽기 전용 event bitmask, m1672b 보고서 인용 `README.ko.md:361`).

검사 의도는 스펙과 맞는다. fixture를 완화하거나 assertion을 구현에 맞춘 변경이 아니다.

### 2.3 실패 원인

실패 위치는 `test_cpp_framework_m6a_runtime.cpp:2241` `assert (second.ingress_data_ready ())`다. 원인을 다음 순서로 확인했다(계측 코드는 커밋하지 않았고 되돌렸다. 패치는 `/root/jobs/fx/cx/m1672c/probe/debug-instrumentation.patch`).

1. 대기 loop는 5초 동안 약 22만 번 돈다. `wait_for_activity`는 매번 즉시 true를 돌려준다. ingress poller가 ROUTER slot에 `POLLIN`을 보고하기 때문이다(`raw_route_port.cpp:346–393`, 계측 `readiness=1 wake=0`). 같은 순간 `events ()`는 0이다(`logs/dbg-run6.log`).
2. gdb로 보면 binding `events ()` → `zlink_get_option (0x3008)` → `socket_base_t::getsockopt (0x110b)`까지 가지만 `get_events_internal`을 부르지 않는다. poller 경로는 `get_events_internal`→`router_t::xhas_in`이 1을 돌려준다(`logs/gdb2.log`, `gdb3.log`).
3. 링크된 `libzlink.so.0`의 `getsockopt`를 disassemble하면 `process_commands`를 부르고 `get_events_internal` 호출이 없다. 이는 수정 전 코드(`has_out () ? ZLINK_POLLOUT : 0`)다. 수정 후 코드는 `core/src/runtime/sockets/common/socket_base_api.cpp:777–788`이 `get_events_internal`을 부른다.
4. 링크된 파일은 `.artifacts/wsl/install/zlink-core/1.19.0/lib/libzlink.so.1.19.0`(RUNPATH)이고, 그 provenance는 `release.tag = core/v1.19.0`, `source.revision = 136a6d2bff`다. `1ad2b0439c`는 `136a6d2bff`의 조상이 아니다. 이 prefix는 공유 cache `~/.cache/zlink/core/1.19.0/linux-x64`에서 복사된다(14:48에 다시 써졌다). main의 `VERSION`은 아직 1.19.0이라 배포 Core와 개발 Core가 같은 버전 디렉터리를 쓴다.
5. 같은 test binary를 `LD_LIBRARY_PATH=core/build-dev/lib`(worktree Core, `getsockopt`가 `get_events_internal` 호출)로 실행하면 m6a_runtime rc=0, `--owner-rejection-observability` rc=0이다(`logs/m6a-builddev-core.log`, `m6a-ownerrej-builddev-core.log`). 배포 Core로는 둘 다 `:2246`(계측판 줄 번호, 원본 `:2241`)에서 abort다.

결론: 시험의 전제(Core EVENTS가 POLLIN을 보고)는 main의 Core에서는 성립하지만, C++ Framework가 소비하는 **배포 Core 1.19.0**에서는 성립하지 않는다. 패키지 빌드는 설계상 배포된 Core 릴리스를 쓴다(`build-wsl.sh` → `core/fetch-release.sh`). 그래서 g1672m 보고서의 "worktree Core를 링크했다"는 표현은 "배포 Core 1.19.0을 링크했다"로 바로잡는다. RUNPATH가 가리키는 worktree 안의 prefix에 있는 파일은 배포 Core다. 로컬 Core로 검증하려면 `doc/building/local-core-bindings.ko.md`의 `ZLINK_CORE_SOURCE=local` 경로를 쓴다.

### 2.4 제안: 이 브랜치에서 제거

- `d9d6d6b86d`를 이 브랜치에서 revert 커밋 하나로 제거한다. 시험 의도와 구현(binding getter, owner 관찰 메서드, 시험 순서)은 그대로 두고 Core 1.20 배포 + binding 게시 뒤 별도 브랜치(#1672 하위)로 다시 올린다.
- 이유: 성능 브랜치는 main 대비 새 실패 0이어야 한다. 이 시험은 배포되지 않은 Core 동작에 의존하므로 CI(배포 Core 사용)에서도 실패한다. binding contract 시험 `test_socket_events_are_non_consuming`(같은 커밋)도 같은 Core에 의존한다.
- 수정(시험을 Core 버전에 따라 건너뛰기)은 하지 않는다. fixture 완화에 해당한다.
- 감독자 결정이 필요한 점: Core 1.20 태그·binding 게시 시점(사용자 확인 사항). (감독자 판정: revert 승인. 공유 cache는 도구 결함이 아니다.)

## 3. 계측 방법(단계 2 시작 때 만든다)

시간 측정은 하지 않는다. 같은 driver로 수정 전·후를 센다. 계측 코드는 커밋하지 않고 `/root/jobs/fx/cx/m1672c/` 아래 사본·패치로 둔다.

- driver: `framework/bench/grpc/cpp/client/bench_cpp_client.cpp`의 `framework_driver_t`와 bench Framework server의 RouteMesh 경로(`add_route_mesh` + `request_to_node`/`send_to_node`)를 gRPC 없이 옮긴 단일 실행 파일. 두 host를 한 process에 두고 1024B payload로 request-serial N=10000, send N=10000을 고정 횟수로 돌린다. (bench 원본 빌드는 vcpkg gRPC feature가 필요해 이 머신에서 빌드 이력이 없다.)
- 할당·lock: `LD_PRELOAD` interposer가 `malloc/calloc/realloc/posix_memalign`, `pthread_mutex_lock`(std::mutex와 libstdc++ `atomic_load(shared_ptr)` lock pool 포함), `pthread_cond_signal/broadcast`, `futex` wake 수를 thread별로 센다. driver가 측정 구간 시작·끝에서 interposer 함수를 불러 구간 값만 낸다. 메시지당 값 = 구간 합 / N.
- 단계별 수: 계측 패치가 E1–E5, I0–I4, W1–W4 지점에 thread id와 counter snapshot을 기록한다. 실행 자원 전환은 한 message의 연속 단계 사이에 thread id가 바뀐 횟수다(08 §3 `:100–110`의 정의).
- task·future 수: `task_shared_state_t`, `task_completion_source_t`, coroutine promise 생성자, `std::promise` 생성 수를 계측 패치의 counter로 센다.

## 4. 항목(비용이 큰 순)

표의 "정적 계수"는 bench 경로(RouteMesh `request_to_node`, 1024B, trace off) 1건 기준이다. "허용 표"는 #1639 10-13 댓글의 메시지당 허용 작업 표다.

| # | 항목 | 정적 계수(1건) | 허용 표 위반 | 소유 스펙 | 분류 | 스펙 개정 |
|---|---|---|---|---|---|---|
| ① | 왕복당 실행 자원 전환(§4.4 인계 미구현) | 전환 2(I4 1 + E5 1), wake 2 이상 | thread 전환 ≤0.1 위반 | 08 §4.4 `:275–313`, §3 `:100`, §4.1 `:187` | A | 없음(②는 머지됨) |
| ② | 제출 경로 task·coroutine 계층 | coroutine frame 약 12, task state·completion source 약 8, lane turn 2, `std::promise` 1, std::function 4 이상 | 메시지별 task/future·lane 합류 금지 위반 | 08 §3 E2–E4 `:95–97`, `:120–127` | B | 없음 |
| ③ | 수신 회전의 record별 poll·descriptor 복사 | record마다 poller wait 1(Core lock 여러 개·zero-timeout poll), coroutine frame 1, descriptor 깊은 복사 2(할당 약 12–15, lock 2) | 회전 단위 수신·header 해석 1회 위반 | 08 §4.1 I2·I3 `:183–184`, AJQ 「4」 | B | 없음 |
| ④ | reply 완료 경로 payload 복사·재해석 | payload 전체 복사 3, application payload 해석 2, JSON header 해석 1, task 완료 연쇄 약 6 | payload 소유권 이전·완료 전달 위반 | 08 §3 E1·E5 `:94,98`, Payload ownership 「2」 | B | 없음 |
| ⑤ | worker claim의 header 재해석·part별 할당 | wire header 2번째 해석 1, part마다 hint `make_unique` 1 + external message 1, owner `make_shared` 1, std::function 2 | header 해석 1회·payload 소유권 이전 위반 | 08 §4.1 I3 `:184`, §4.3 W2 `:218`, Payload ownership 「2」 | B | 없음 |
| ⑥ | node 직접 대상 분류의 Location Store 조회 | coroutine frame 1(Redis 없음), Redis 사용 시 메시지마다 `list_mesh_nodes` 왕복과 page 복사 | 다른 소유자 대기 금지 위반(#1641) | 08 §3 E2 `:95`, `:120–127` | B | 없음 |
| ⑦ | route channel 요청의 worker 왕복 | manager lane turn 1, offload worker 전환 2, worker의 동기 대기 | thread 전환·lane 합류 위반 | 08 §3 `:100`, Submit 「3」 worker offload | B(설계 변경 동반) | 없음 |
| ⑧ | Yield turn mutex 5회 | Yield 1회당 비경합 mutex 1 추가 | mutex ≤3 예산 압박 | Handler turn 「3」 `:101`, 「11」 `:432` | B | 없음 |
| ⑨ | Actor 깊은 대기의 scheduler hop | 대기 1회당 `executor_mutex` 2, hop 1 | thread 전환 위반 | Handler turn 「3」, 08 §3 `:100–110` | 진단 필요(B 또는 D) | 판단 보류 |
| ⑩ | #1645 브랜치 잔여 커밋 | ④에 포함 | — | — | ④와 같음 | 없음 |

아래 항목별로 위치, 다른 세 언어의 대응, 수정안을 적는다.

### ① 왕복당 실행 자원 전환 2회 — hand-off §4.4

- 현재 위치
  - 수신 thread: `mesh_node_host_service.cpp:2567`(정적 `_threads`). ready owner는 `:2556–2565`에서 `_application_dispatch->try_submit_internal`로 application executor에 게시한다(I4 → worker 전환 1).
  - 완료: `operation_registry.cpp:102–232` process 공유 completion dispatcher의 전용 `_worker` thread(E5 전환 1). `bb26c57e54`로 lock 없는 게시가 되었고 #1642의 동기 대기는 없다. 전환만 남았다.
- 비용: request 왕복마다 Framework 전환 2(서버 I4, 클라이언트 E5)와 OS wake 2. p1645 계측 구간 I4 20.2µs, E5 15.8µs(q1645r `:142`). 예산(전환 ≤0.1)을 맞추는 다른 방법은 없다.
- 다른 언어: q1645r `:55–56, 87, 123–129, 197, 280–282`. Java는 virtual-thread pump와 dispatcher 구조로 같은 인계 후보, .NET E5는 binding RCA pool 알림이라 직접 적용 불가, Node는 넘겨받을 자원이 없어 기존 경로.
- 수정안: q1645r `:237, 280–282`대로 수신 `:2567`, ready 게시 `:2558`, completion 게시(`operation_registry.cpp` `_worker`)의 세 실행 자원 배치를 기존 `offload_executor_t`의 자원 owner 하나로 **교체**한다(3→1). 수신 회전이 끝난 경계에서 idle worker를 다음 수신 역할에 독점 배정할 수 있으면 넘기고 현재 자원이 준비된 owner turn과 completion을 직접 실행한다. 배정할 수 없으면 지금처럼 queue에 남긴다. 기존 구조 옆에 인계 분기를 더하는 방식(3→4)은 쓰지 않는다.
- 분류 A(머지된 스펙 ② 적용). 범위가 가장 크고 Yield 재개 구조(#1745) 위에 올라간다. ②·③·④·⑤보다 뒤에 하는 것을 제안한다. 먼저 하는 항목들이 전환 수를 바꾸지 않으므로 순서가 결과를 흐리지 않는다.

### ② 제출 경로의 task·coroutine 계층과 lane turn

`route_client_t::request_to_node` 1건이 binding submit까지 지나는 층:

| 층 | 위치 | 비용 |
|---|---|---|
| route client coroutine | `channel_runtime.cpp:1744–1840` (`submit_request_reply_message_erased`) | frame 1, `request_task_t` promise의 `_admitted` `make_shared` 1(`request_task.hpp:92–93`) |
| transport 조회 | `channel_runtime.cpp:70–83, 1767`(send는 `:1436`) | channel state lane `run_task` 1(비경합이면 inline, `_mailbox_mutex` 2, std::function 1) |
| mesh request lambda | `app.cpp:2085–2106` | `request_task_t` coroutine frame 1 + `_admitted` 1, `observe_request_failure`(`mesh_node_runtime.cpp:4194–4204`)의 observer 1, `await_request_reply` frame 1(`:4207`) |
| `mesh_node_runtime_t::request_to_node` | `mesh_node_runtime.cpp:4307–4320` | frame 1, `classify_node_direct_target` frame 1(⑥) |
| `public_host_runtime_t::request_to_node` | `public_host_runtime.cpp:2852–2878` | frame 1, `encode_application (parts)`가 const 참조라 `from_parts` 복사(`service_wire_codec.cpp:2484–2496`: vector `make_shared` 1 + part마다 `copy ()`) |
| raw owner `request_to_node`→`request_to_target`→`request_with_header` | `raw_mesh_node_owner.cpp:929, 958, 1095–1172` | frame 3, header std::function 1, `_topology.peer` 깊은 복사 1(`:1109`, ③과 같은 비용), owner `_lane.run` 1(`:1124`: `std::promise` `make_shared` + std::function + future wait, mutex 3 이상), `running` `make_shared<task_t>` 1, `admitted` `make_shared` 1, `observe_request` frame 1(`:979`), `await_request_admission` frame 1 |
| raw port | `raw_route_port.cpp:296–340` | completion source `make_shared` 1, admission observer 1, `materialize_binding_parts` part마다 1 |
| route client 쪽 다시 감싸기 | `channel_runtime.cpp:1826, 1840`, `request_task.hpp:106–125` | `request_task_t (task, admitted)`마다 새 shared state 1 + observer 2 |

- 허용 표: send·request는 "encode 1회 + binding submit 1회". 위 층은 메시지별 task·future·lane 합류(금지 목록)에 해당한다. 08 §3 `:120–127`은 필수 소유 turn 둘(E2 selector, E3·E4 operation owner) 밖의 lane 왕복을 금지한다. node 직접 대상에는 selector가 없으므로 channel state lane turn은 필수 turn이 아니다.
- 다른 언어: .NET은 `ZLinkRouteClient.cs:403`에서 `GetMeshNodeRuntime`(map 조회, lane 없음) 뒤 `:447` `RequestToNodeAsync` Task 하나. Node는 `channel-transports.ts:460` `meshNode ()` 조회 뒤 `:497` `node.requestToNode` 하나. Java는 `ZLinkChannelCallRuntime.java:190–205` `requestNode`가 `node.requestToNode`의 CompletionStage 하나를 돌려준다. 세 언어 모두 transport 조회에 lane turn이 없고 제출 층이 1–2개다.
- 수정안(판정 위치를 늘리지 않고 층을 지운다)
  1. transport map은 bind 때 바뀌는 값이다. 08 §3 E2 "변경 시점에 미리 준비"대로 bind·unbind가 불변 snapshot을 publish하고 제출은 snapshot에서 읽는다. channel state lane turn 1 제거.
  2. `app.cpp:2088`의 `parts.items ()`(const 참조) 전달을 move로 바꿔 `from_parts` 복사를 없앤다(send 경로 `:2075–2080`는 이미 move).
  3. `mesh_node_runtime_t`·`public_host_runtime_t`·raw owner의 request 층을 "등록 + submit을 owner turn 하나에서 하고 결과 객체 하나를 돌려주는" 함수 하나로 합친다. raw owner의 `_lane.run (...).get ()`(promise·future)은 같은 turn을 쓰는 `run_task` inline 경로로 바꾼다.
  4. route client의 `request_task_t` 재포장(`channel_runtime.cpp:1826, 1840`)은 하위가 돌려준 결과 객체를 그대로 쓴다.
- 분류 B. 판정 위치: transport 조회 2→1(bind 쪽만), request 등록 경로 3층→1.

### ③ 수신 회전의 record별 poll·descriptor 복사

- 위치
  - `public_host_runtime.cpp:6133–6140` 회전 loop가 record마다 `pump_one`(coroutine frame 1)을 부르고, `raw_mesh_node_owner.cpp:2647`이 record마다 `port->poll (0)`을 부른다. 이 poll은 `_poller_mutex`, Core `zlink_poller_wait`(socket item의 `get_events_for_poller`: admission scope, primary signaler drain, `process_commands`, completion drain loop, `has_in`; wake timer fd의 zero-timeout poll)를 매번 지난다.
  - `raw_mesh_node_owner.cpp:2805` `_topology.peer (source)`와 `:3302` `_topology.local_descriptor ()`가 record마다 `admitted_peer_t`·`service_node_descriptor_t`를 값으로 복사한다(`service_topology_registry.cpp:257–260, 451–458`). descriptor 하나는 routing id·endpoint·channels·protocol capability 문자열들을 가져서 복사 1번에 할당 약 6–8, `std::atomic_load (shared_ptr)`가 libstdc++ lock pool mutex 1이다.
- 허용 표: receive는 "회전 단위 수신, header 해석 1회". 08 §4.1 I2(`:183`)는 한 번 깨어나면 예산 안에서 연속 claim, I3(`:184`)는 header만 해석한다.
- 다른 언어: Java `ZLinkJavaRawServicePort.java:348–352`도 record마다 `waitForReadable (ZERO)`와 state lane 2회를 쓴다(같은 위반, Java job에 넘길 항목). Java `ZLinkJavaRawMeshNode.java:4762` `topology.peer`는 참조를 돌려준다. .NET `ZLinkManagedMeshNode.cs:6499` `ResolvePeerGeneration`은 generation 값 하나만 읽는다. Node `raw-service-mesh-runtime.ts:844`는 header만 읽는다. descriptor 복사는 C++의 값 의미에서만 생긴다.
- 수정안
  1. 회전마다 poll 1회: readiness를 확인한 뒤 같은 회전 안에서 `receive_if_ready`를 `EAGAIN` 또는 예산(건수·byte·시간)까지 반복한다. `pump_one`은 회전 단위 함수로 바꾸고 record별 coroutine frame을 없앤다.
  2. topology snapshot(`shared_ptr<const topology_t>`)을 회전 시작에 한 번 읽고, peer·local descriptor는 그 snapshot의 참조로 쓴다. 새 cache를 두지 않는다(이미 있는 불변 snapshot을 읽는 횟수만 줄인다).
- 분류 B. 판정 위치: readiness 판정 record마다 1 → 회전마다 1.

### ④ reply 완료 경로의 payload 복사·재해석

- 위치(클라이언트, reply 1건)
  1. `raw_binding_adapter.hpp:143–151, 189` `copy_binding_parts`가 binding reply part를 `to_bytes ()`로 전부 복사(복사 1).
  2. `raw_mesh_node_owner.cpp:1049` 유효성 확인용 `decode_application_payload (parts[1], false)`가 `payload_bytes ().assign`(`service_wire_codec.cpp:2711–2716`)으로 다시 복사(복사 2, 해석 1).
  3. `operations->complete (operation, std::move (parts[1]))` → completion dispatcher → `public_host_runtime.cpp:7195–7197` `complete_operation`이 `decode_application_payload`(복사 3, 해석 2)와 `decode_application_parts`로 message를 다시 만든다.
  4. route client가 `channel_runtime.cpp:1843` `envelope.decode_header`(JSON SAX, `envelope_codec.cpp:804–815`)와 `decode_body`를 한다.
  5. 그 사이 task 완료가 `await_completion` → `await_request_reply` → mesh lambda → `request_task_t` 재포장 → route client coroutine으로 약 6단 이어진다(각 단 shared state lock 1과 callback).
- 허용 표: reply·completion은 "binding reply 1회, payload 소유권 이전". 08 §3 E1(`:94`)이 금지하는 `message → byte[] → message` 왕복과 같은 payload의 표현 두 벌이 그대로 있다.
- 다른 언어: Java `ZLinkJavaRawMeshNode.java:1716`(reply header 해석 1회), .NET `ZLinkManagedMeshNode.cs:5953` `TryDecodeReply`, Node `raw-service-mesh-runtime.ts:1337`. 세 언어의 reply payload 복사 횟수는 단계 2에서 대조표로 확인한다.
- 수정안: #1645 브랜치 `c7152b790a`("retain native reply storage through completion", application payload materialization 3→1, terminal owner 1→1)를 현재 HEAD에 맞춰 다시 적용한다. binding message를 operation registry까지 그대로 넘기고, 유효성 확인과 complete가 같은 해석 결과 하나를 쓴다. `c7152b790a`는 main보다 153커밋 뒤에서 만들어져 `public_host_runtime.cpp`(226줄)·`raw_mesh_node_owner.cpp`(74줄) 충돌이 예상된다. 0.30.1 정합성 수정 쪽을 유지하고 그 위에서 다시 쓴다.
- 분류 B. 판정 위치: payload materialization 3→1.

### ⑤ worker claim의 header 재해석·part별 할당

- 위치(서버, record 1건)
  - `public_host_runtime.cpp:6366–6367` `dispatch_application_claim`이 I3에서 이미 해석한 wire header를 다시 해석한다(#1643). node·channel·spot·actor header도 다시 decode한다(`:6398–6416`).
  - `public_host_runtime.cpp:6445` `decode_application_parts (std::move (...))` → `service_wire_codec.cpp:2720–2745`: owner `make_shared` 1, parts vector 1, part마다 hint `make_unique` 1과 external message 1.
  - `:6338–6350` claim마다 `claim_holder`·`release_state` `make_shared`와 record마다 retain·release std::function 2.
  - host 쪽 `decode_mesh_record_header`가 envelope JSON header를 SAX로 해석한다(`envelope_codec.cpp:804–815`). 이것은 header 해석 1회로 허용 범위다. 할당 수는 계측에서 따로 낸다.
- 허용 표: receive는 "header 해석 1회, payload 소유권 이전".
- 다른 언어: Java `ZLinkJavaRawMeshNode.java:4901–4903`, .NET `ZLinkManagedMeshNode.cs:6444–6503`, Node `raw-service-mesh-runtime.ts:844–845`가 수신 때 한 번 해석한 결과를 record와 함께 넘긴다(단계 2에서 worker 쪽 재해석 여부를 줄 단위로 대조한다).
- 수정안
  1. I3에서 해석한 kind·correlation·operation·target을 `service_mailbox_record_t`에 담아 넘기고 W 쪽 재해석을 지운다(header 해석 판정 2→1).
  2. part hint는 part마다 `shared_ptr` 사본을 heap에 두지 않고, 원본 message 하나를 가진 블록 하나의 참조 수로 해제한다(할당 1+N → 1).
- 분류 B.

### ⑥ node 직접 대상 분류의 Location Store 조회

- 위치: `mesh_node_runtime.cpp:4227–4265` `classify_node_direct_target`이 send·request마다 실행된다. Redis store가 있으면 메시지마다 `_user_spot_store->list_mesh_nodes`를 page 단위로 끝까지 읽는다. 없으면 coroutine frame 1만 남는다(bench는 이 경우).
- 다른 언어: Java `ZLinkJavaRawMeshNode.java:1384–1414`는 local peer intent·topology로 판정한다(Location Store 조회 없음). .NET은 admission의 object role(`ZLinkMeshChannelSelection.cs:30`)을 쓴다. C++ 주석(`public_host_runtime.cpp:2864–2876`)도 Java·Node가 topology로 판정한다고 적는다.
- 수정안: admitted peer descriptor의 `object_role`(`service_topology_registry.hpp:76`)로 판정한다. ②에서 이미 읽는 topology snapshot을 쓰므로 조회가 늘지 않는다. 비admitted 대상은 지금처럼 raw owner의 topology gate가 `not_found`로 판정한다.
- 결과 동일성: Location Store의 role과 admission descriptor의 role이 다를 수 있는 구간(재등록 중)을 단계 2에서 시험으로 확인한다. 다르면 이 항목은 스펙 gap(D)으로 돌려 보고한다.
- 분류 B(#1641).

### ⑦ route channel 요청의 worker 왕복

- 위치: `channel_runtime.cpp:1817–1838`(request), `:2100–2118`, `:1563, 1593, 1667, 1701, 1731, 2067`(다른 route channel 조작). worker에서 `channel_runtime_manager_t (runtime_state).get_route_channel (router_channel_id)`(lane turn 1 + map 조회)를 한 뒤 `request_reply_parts`를 동기로 실행한다. 요청마다 caller → `route_client_executor` worker → 완료 게시로 전환 2와 worker의 동기 대기가 있다.
- 범위: `route_channels`는 `channel_runtime_manager.cpp:170–200`이 route channel 등록에서 만든다. RouteMesh(`add_route_mesh`)는 `mesh_node_transports`(`channel_runtime.cpp:747–758`)를 쓰므로 bench 경로가 아니다.
- 원인(g1672m): route channel의 마지막 참조가 worker에서 풀리면 `_lane_executor` 소멸자(`offload_executor.cpp:41–44`)가 drain·join한다. 그래서 worker가 route channel을 잡지 못하고 매번 manager로 다시 찾는다.
- 수정안: route channel 수명을 runtime state가 teardown까지 소유하게 하고(teardown이 drain), 요청 경로는 그 수명 안에서 참조만 쓴다. 근본적으로는 route channel 요청도 binding 비동기 request(E4)를 쓰게 해서 worker 동기 대기를 없앤다. 소멸 경로 설계 변경이라 단계 2에서 별도 승인을 받는다.
- 다른 언어: 단계 2 착수 전에 Java·.NET·Node의 route channel 요청 경로를 대조한다(이번에는 찾지 않았다).
- 분류 B(설계 변경 동반). bench 경로 밖이라 순서를 뒤로 둔다.

### ⑧ Yield turn mutex 5회

- 위치: `serial_execution_queue.cpp:343–349` `wait_cancellation ()`이 turn의 `_mutex`를 잡고 `stop_source`를 처음 쓸 때 만든다. #1789 뒤 Yield 1회당 비경합 mutex가 4→5다(perf-HANDOFF 인계 뒤 전제).
- 수정안: `stop_source`를 turn 생성 때 만들어 두면 `wait_cancellation`은 lock 없이 token을 돌려줄 수 있다(`cancel_waits` `:350–360`의 lock도 같은 이유로 줄일 수 있는지 확인). turn 생성 시 할당 1이 늘 수 있으므로 할당·mutex 수를 같이 센다.
- 다른 언어: Java·.NET Yield 경로의 lock 수는 단계 2에서 대조한다.
- 분류 B. bench(request-serial의 caller는 handler 밖)에는 나타나지 않는다.

### ⑨ Actor 깊은 대기의 scheduler hop

- 위치: `actor_client.cpp:480–495` `request_erased`가 actor turn 안에서 `resolve_actor`를 co_await한다. 재개가 runtime continuation scheduler를 거쳐 `executor_mutex` 2회와 hop 1회를 만든다(지시 목록 기준, 이번에 줄 단위로 재확인하지 않았다).
- 판단: Handler turn 「3」(`02-handler-turn-and-execution-gate.ko.md:101`)이 Yield 재개를 정하지만, turn을 반납하지 않는 Framework 내부 대기의 재개 자원은 명시하지 않는다. 같은 thread inline 재개가 허용되는지 스펙 조항으로 확정한 뒤 수정한다. 확정하지 못하면 D로 보고한다.
- 분류: 진단 필요.

### ⑩ #1645 브랜치(`origin/framework-cpp/1645-serial-roundtrip`) 잔여 커밋

| 커밋 | 내용 | 판단 |
|---|---|---|
| `c7152b790a` | reply native storage를 completion까지 유지 | ④로 다시 적용 |
| `6f37d668dc` | request admitted·dispatched trace | 성능 항목이 아니다. main의 `route_packet_dispatcher.cpp:142–147`에 dispatched trace가 있으나 같은 동작인지 확인이 필요하다. 이 job 범위 밖으로 보고만 한다 |
| `d1fd378cfa`, `5164da065e` | bench client의 serial 대기 방식·phase 경계 | `framework/bench/`라 이 job의 수정 범위(`framework/languages/cpp/`, `bindings/cpp/`) 밖이다 |
| `e14d98b916` | 시험 fixture 이름 정리 | `c7152b790a`와 함께 필요한 부분만 |

## 5. 제안하는 진행 순서

1. §2.4 WIP 커밋 revert(시험 실패 2건 제거, main과 같은 155/155 기준선 확보).
2. §3 계측 harness와 수정 전 기준표.
3. ③(수신 회전) → ②(제출 층) → ④(reply 복사, `c7152b790a`) → ⑤(claim) → ⑥(분류).
4. ①(§4.4 인계, 3→1 자원 통합). 별도 설계 확인을 받고 시작한다.
5. ⑦·⑧·⑨는 ① 뒤에 승인받는다.

## 6. 범위 밖 보고

- **Core 출처(감독자 판정으로 정정)**: 패키지 빌드가 배포 Core를 쓰는 것은 설계다. 앞선 보고의 "worktree Core로 검증" 표현이 잘못이었고, 로컬 Core 검증은 `ZLINK_CORE_SOURCE=local` 경로(`doc/building/local-core-bindings.ko.md`)로 한다.
- Java `ZLinkJavaRawServicePort.java:348–352`의 record별 poll·lane 2회는 ③과 같은 위반이다. Java job(#1673)에 넘긴다.
- 빌드 부산물: 이번 계측 빌드는 test target만 다시 빌드했고 debug 코드는 되돌린 뒤 다시 빌드했다(`logs/clean-rebuild.log` rc=0). 소스 변경은 merge 커밋 외에 없다(`git status`: `zlink-work/`만 untracked).

로그·계측 자료: `/root/jobs/fx/cx/m1672c/logs/`, `/root/jobs/fx/cx/m1672c/probe/`.
