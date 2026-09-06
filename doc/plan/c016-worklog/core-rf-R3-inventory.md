# R3 인벤토리 — pipe/ypipe 계열 (core/src/runtime/core/pipe.{cpp,hpp} 외)

> Phase 3 인벤토리, 읽기 전용. 대상: `pipe.cpp`(4128), `pipe.hpp`(931),
> `pipe_stream_packet_state.hpp`(57), `ypipe.hpp`(219), `ypipe_base.hpp`(76),
> `ypipe_conflate.hpp`(122), `yqueue.hpp`(188), `session_base_pipe_io.cpp`(252),
> `ctx_physical_queue_registry.cpp`(1263, pipe.cpp가 호출하는 17개 API만).
> Repo HEAD 5a5b111139 (브리프 지정 2529709db6 이후 문서 커밋만 추가, 대상 파일 변경 없음 확인).
> 빌드·측정 없음. dead-code 판정은 전부 전체 리포 `grep -rn` 참조수 교차검증(선언+정의 제외 외부 호출 0건 기준)만 사용했고,
> `pipe.cpp`의 190개 자체 메서드 전수 검사 결과 **참조수 0(선언+정의만 남은 것)은 없었다** — 명백한 dead 함수 없음.

## 표

| # | 분류 | file:line | 관찰 | 제안 | 반경 | 계약 | 성능 |
|---|---|---|---|---|---|---|---|
| 1 | 3(얕은 모듈) | `pipe.hpp:1-931`, `pipe.cpp:1-4128` | `pipe_t` 한 클래스가 ypipe 전송 배관, credit/HWM 회계(`account_inbound_frame`, `can_commit_bytes_*`), remote flow-state 제어 프레임(`apply_remote_flow_state`, `write_flow_state_control_and_flush`, peer weight), STREAM 패킷 상태(`transport_lifetime_t`의 `pipe_stream_packet_state_t`), ROUTER route-binding/RID 발행, `ctx_physical_queue_registry` 연동(17개 API 직접 호출), lifetime-ref 카운팅(`lifetime_state_t` 내부 클래스)까지 6개 이상 개념을 한 파일에 담고 있다. 멤버 필드 약 60개(`pipe.hpp:709-905`). | 리팩터 후보(이번 job 범위 아님): credit/HWM 회계와 flow-state 제어를 별도 helper 클래스(`pipe_t`가 소유하는 컴포지션)로 분리하는 것을 다음 R 단계 후보로 제안. 즉시 조치는 없음 — 분리는 광범위한 반경. | 파일 다수(pipe.hpp/cpp 전체 재구성), 500행+ | 없음(내부 구조만) | 없음(순수 구조 이동이면) |
| 2 | 3(매개변수 과다) | `pipe.hpp:110-119` | `pipepair()` 파라미터 10개(그중 불리언 4개: `conflate_`, `session_pipe_`, `planning_enabled_`, 그리고 enum 2개). 호출자가 내부 조합 규칙을 알아야 하는 정보 누출 소지. | 옵션 구조체(`pipepair_options_t`)로 `session_pipe_/lane_/role_/planning_enabled_/queue_class_/session_owner_index_` 묶기. hwms/conflate는 배열 특성상 별도 유지 가능. | 1파일(pipe.hpp) + 호출자 전수(`grep pipepair(` 호출처, 대략 5-10곳) | 없음(내부 API) | 없음 |
| 3 | 5(확인 필요) | `session_base_pipe_io.cpp:211-213` | `session_base_t::reset ()` 본체가 비어 있다(`void reset () {}`에 준함). `session_base.hpp:40`에 `virtual`로 선언. 이 저장소에서 `session_base_t`를 상속하는 클래스가 없어(grep 0건) virtual일 이유가 약하고, 폴리모픽 호출부(`->reset ()`를 세션 인터페이스 포인터로 호출하는 곳)를 좁은 grep으로 찾지 못했다. | **확인 필요**: (1) `i_msg_source`/디코더 계열 인터페이스가 `reset()`을 콜백/함수포인터로 요구하는지 헤더 전수 확인, (2) core 밖(bindings/framework)에서 `session_base_t*`를 통해 호출하는 곳이 있는지 리포 전체 grep. 사용처가 진짜 0이면 선언·정의 삭제 + `virtual` 제거. | 2파일(session_base.hpp, session_base_pipe_io.cpp), 소규모 | 없음(내부) | 없음 |
| 4 | 1(디버그 잔재, 확인 필요) | `session_base_pipe_io.cpp:14-74, 148-206` | `ZLINK_DEBUG_ROUTER_ROUTE` 환경변수로 게이트된 `trace_router_session_push` fprintf 트레이싱이 `push_msg_internal`(세션→소켓 hot path) 4개 지점에 박혀 있다. 매 메시지마다 `router_route_trace_on` 정적 초기화 후 분기 1회 + ROUTER 소켓일 때 atomic 카운터 증가. 스펙 문서에 이 환경변수가 지원 디버그 knob으로 문서화되어 있는지 확인 안 됨 — 버그 조사용 임시 계측일 가능성. | **확인 필요**: `grep -rn ZLINK_DEBUG_ROUTER_ROUTE doc/` 로 공식 문서화 여부 확인. 문서화 안 됐고 최근 진단(decisions.ko.md D-090대 근방 router 조사) 잔재라면 제거, 문서화됐다면 유지. | 1파일, ~90행 삭제 후보 | 없음 | 이득(hot path 분기 1개 제거, 미미) |
| 5 | 5(스펙 BLOCKER 재평가, 미해결) | `core/src/runtime/sockets/router/router_recv_path.cpp:36-108,378-477`(참조), `pipe.hpp:219-226`(관련 API) | rf2 BLOCKER(`posddd-rf2-summary.md:204`): `router_t::copy_router_pipe_source_rid`의 recv hot path가 여전히 `_out_pipes_sync` lock + `_standby_pipes.find` 선형 조회로 route source RID를 얻는다. pipe 쪽엔 이미 `publish_router_route_source`/`try_copy_router_route_binding`(exact-route용)이 있지만, standby-lookup 케이스(핸드오버 중 이전 RID 조회)를 대체할 topology-time snapshot 필드/발행 API가 pipe 쪽에 아직 없다 — **HEAD 기준 미해결, 여전히 유효**. | pipe.hpp/cpp에 read-mostly snapshot(예: standby RID를 attach/handover 시점에 pipe에 캐시해 lock-free로 노출하는 API) 추가를 다음 apply job으로 제안. router_recv_path.cpp 쪽 소비는 범위 밖(sockets 모듈)이라 pipe 쪽 API 설계까지만 이번 R3 산출물. | 2파일 이상(pipe.hpp/cpp + router_recv_path.cpp), 중간 | 있음→D 필요(새 pipe-owned 상태 추가이므로 소유권/무효화 규칙 결정 필요) | 이득(hot path lock+선형탐색 제거 가능성), 단 설계 전 확정 불가 |
| 6 | 1(BLOCKER 해소 확인, 조치 불요) | (해당 없음 — 이미 삭제됨) | rf2 BLOCKER 2번째 항목(`posddd-rf2-summary.md:205`): `pipe_t::check_read_with_record_admission` dead code로 지목됐던 것. 전체 리포 grep 결과 현재 이 심볼은 **0건**(선언·정의 모두 이미 제거됨). | 조치 불요 — 기록용으로만 남김. | 0 | 없음 | 없음 |
| 7 | 관찰(성능, 변경 아님) | `pipe.cpp:1024-1054`(`check_read`), `1135-1231`(`read_internal`), `2736-2778`(`process_activate_read`) | S-1/S-11이 `_in_active`/`_state`를 atomic으로 승격하고 `process_activate_read` 핫패스에서 `_out_sync`를 이미 제거했다(주석 `pipe.cpp:2741-2749`가 근거를 명시). `check_read`/`read_internal`도 이미 `_out_sync` 없이 두 멤버를 읽는다 — 즉 **이 세 함수는 더 줄일 `_out_sync` 구간이 남아있지 않다**(S-1/S-11이 이미 처리). | 변경 제안 없음(관찰만). | - | - | - |
| 8 | 관찰(성능, 변경 아님) | `pipe.cpp:1233-1260`(`reserve_inbound_decoder_frame`) | `_session_pipe`가 아닐 때만 `_out_sync`를 잡고 `_state`, `_out_physical_queue`, `_out_incomplete_bytes`, `_bytes_written`, `_peers_bytes_read`(outbound 회계 클러스터)를 읽는다 — 이 멤버들은 S-1/S-11이 다룬 `_in_active`/`_state` 단일 read-mostly 플래그와 달리 여러 필드가 얽힌 실제 회계 갱신이라 잠금 축소 여지가 뚜렷하지 않다. **확인 필요**: `_state`만 놓고 보면 이미 atomic이므로 이 함수의 첫 `_state != active` 체크만은 잠금 밖으로 뺄 수 있을지(다른 필드는 잠금 유지) 다음 job에서 별도 검토. | 확인 필요(별도 job) | 1파일, 소규모 | 없음(hot path 아님, decoder 프레임 예약 시점) | 이득 가능성 낮음(콜드~중간 경로) |
| 9 | 4(확인 필요, 소유권) | `pipe.cpp`가 호출하는 `ctx_physical_queue_registry` API 17개(`account_provisional_frame`, `advance_generation`, `applied_hwm`, `bind_application_pipe_queue`, `classify_pipepair_queues`, `commit_decoder_frame`, `commit_message`, `current_accounted_bytes`, `generation`, `planned_hwm`, `refresh_application_hwm_if_drained`, `release_committed_frame`, `release_decoder_frame`, `release_endpoint`, `reserve_decoder_frame`, `rollback_provisional`, `unbind_application_pipe_endpoint`, `update_hwm_target`) | `pipe_t`가 물리 큐 레지스트리의 세부 상태 전이(provisional/committed 분류, HWM plan 적용 등)를 17개의 개별 API로 직접 오케스트레이션한다. registry 내부 개념(provisional vs committed byte 분류)이 `pipe.cpp` 여러 곳(`publish_outbound_frame_unlocked`, `account_inbound_frame` 등)에 노출돼 있어 "하위 계층이 상위 계층을 아는" 것은 아니지만 호출 표면이 넓다. | **확인 필요**: registry 쪽 인벤토리(R 다른 모듈)와 교차 검토해 17개 호출 중 항상 같이 호출되는 쌍(예: reserve+release, provisional+rollback)이 있으면 pipe 쪽에 조합 헬퍼를 둘지 판단. 이번 job만으로는 registry 내부를 보지 않아 결론 보류. | 확인 필요 | 확인 필요 | 확인 필요 |
| 10 | 3(파일 크기, 관찰) | `pipe.cpp` 4128행 | 단일 .cpp 파일 4128행은 `07-core-source-layout.ko.md` 기준으로도 이례적으로 크다(항목 1과 동일 원인, 별도 파일 분할 관점에서 재기재). | 항목 1과 동일 리팩터 후보에 통합. | 항목 1과 동일 | 없음 | 없음 |

## 적용 job 묶음 제안

파일이 겹치지 않고 각 1.5h 안에 끝나도록 3개로 분리한다(항목 1, 10은 구조 리팩터라 이번 apply 대상에서 제외 — 별도 R 후속 job으로).

1. **묶음 A (확인·소규모 삭제)** — 항목 3, 4: `session_base_t::reset()` 사용처 전수 확인 후 죽었으면 제거, `ZLINK_DEBUG_ROUTER_ROUTE` 트레이싱 문서화 여부 확인 후 조치. 파일: `session_base.hpp`, `session_base_pipe_io.cpp`.
2. **묶음 B (pipepair 옵션 구조체화)** — 항목 2: `pipepair()` 파라미터를 옵션 구조체로 정리, 호출자 전수 갱신. 파일: `pipe.hpp`(+선언부), 호출자들(사전 grep으로 확정).
3. **묶음 C (route-binding cache 설계)** — 항목 5: pipe 쪽 standby-RID snapshot API 설계 + D 결정 상정(새 상태 추가이므로 감독 결정 필요), router_recv_path.cpp 소비는 후속. 파일: `pipe.hpp`, `pipe.cpp` (+ 결정 문서).

항목 8, 9는 단독으로 적용하기엔 근거 부족("확인 필요")이라 묶음에서 제외 — 각각 별도 확인 job 선행 후 재평가 권장.

보고 경로: `doc/plan/c016-worklog/core-rf-R3-inventory.md`
