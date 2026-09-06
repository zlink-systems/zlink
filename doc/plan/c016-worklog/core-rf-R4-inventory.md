# R4 인벤토리 — core/src/runtime/sockets/common/ (Phase 3)

> 읽기 전용 조사. 빌드·테스트·측정·소스 수정 없음. 기준 HEAD `2529709db6`.
> 기준 문서: posddd.ko.md(얕은 모듈 스멜 카탈로그/위험 신호), 08/07-core-source-layout. 관련 실측: S-5(537 Ir/msg 계층 비용), S-A(auto-HWM 원자 카운터).
> 대상 파일 26개(hpp 6 + cpp 20), 총 ~15,100줄. dead-code/duplication 하위 조사는 fork 2건(병행, read-only)에 위임 후 통합.

## 표

| # | 분류 | file:line | 관찰 | 제안 | 반경 | 계약 | 성능 |
|---|---|---|---|---|---|---|---|
| 1 | 1(dead) | `routed_submit_target.hpp:24` | `typedef uint64_t zlink_send_op_id_t;` — 전체 저장소에서 이 정의 외 참조 0(`grep -rn zlink_send_op_id_t .` → 정의 1건뿐). 0.12.0 async-completion 설계 문서(archive)에서만 언급되는 잔재, `zlink_completion_id_t`로 대체된 개념 | 삭제 | 파일 1·1줄 | 없음 | 없음 |
| 2 | 3(얕은 모듈) | `socket_close_ops.hpp`(20줄)+`.cpp`(19줄) | `socket_close_ops_t`는 `stop()+close()+포인터 NULL화` 3줄을 감싸는 정적 메서드 1개짜리 클래스. 호출자 1곳(`monitor_api.cpp:201`) | 클래스 제거, 호출부에 인라인하거나 자유 함수로 축소 | 파일 2+호출부 1(≈40줄) | 없음 | 없음 |
| 3 | 3+4(얕은 모듈/정보 누출) | `socket_base.hpp:1068-1085`(`send_direct_with_retry`, 매개변수 17개), `:294-322`(`send_scoped`/`send_routed`/`send_routed_complete_record`/`send_routed_scoped`), 정의 `socket_base_msg.cpp:320`, `socket_send_complete.cpp:250-390` | S-13 리드 재확인: `send_routed_scoped`→`send_direct_with_retry` 등 4~5개 래퍼가 인자만 그대로 전달(14~19개). 그중 `record_context_admission_`(기본값 true)는 이 체인의 **모든 호출부(5곳: 파일 내 4 + `socket_message_send_api.cpp:206`)에서 한 번도 false로 넘어오지 않음** — 죽은 매개변수 | (a) `record_context_admission_` 매개변수 제거(안전, 우선 적용) (b) S-13 본제안: 5중 프레임 병합 설계(가운데 3개 프레임 통합, admission 규칙 소유권 명확화) | (a) 4~5파일 소규모 (b) 동일 파일군, 설계 필요·별도 job | 없음(내부 전용) | (a) 미미 (b) 이득 후보(S-5: 순수 계층 537 Ir/msg) |
| 4 | 3(얕은 모듈/한 파일 여러 개념) | `socket_base_api.cpp`(2144줄) | sockopt(651-761)·events/poller(762-925)·join/leave(964-978)·count-1 completion pipe drain 상태기계(1042-1602, ≈560줄)·pipe 활성/종료 콜백(1603-2099)·transport pair adopt/release(66-163)가 한 TU에 혼재 | completion-drain 상태기계를 별도 TU로 분리(예: `socket_completion_drain.cpp`) | 파일 1→2, CMake 등록 필요 | 없음 | 확인 필요(S-5: 수동 인라인 재배치가 LTO 결정과 상충한 전례 — 분리 전후 cachegrind 대조 권장) |
| 5 | 4(잘못된 소유) | `socket_base.hpp:370-436`(`test_*` 메서드 17개) | 전부 `socket_base_t` public 멤버지만 프로덕션 호출자 0, 전량 `core/tests/unittest/*.cpp` 6개 파일에서만 사용(개별 확인 완료). 이미 존재하는 friend 기반 테스트 접근 패턴(`friend class session_termination_test_access_t;`, `:252`)과 불일치 | friend 테스트 접근자 클래스로 이전, public 표면에서 제거 | 헤더 1 + 테스트 파일 6~7개 | 없음(테스트 전용) | 없음 |
| 6 | 2(중복) | `socket_base_monitor.cpp:394,401,408,415,422,429,436,443,517,524` | `event_connect_delayed`/`event_connect_retried`/`event_listening`/`event_bind_failed`/`event_accepted`/`event_accept_failed`/`event_closed`/`event_close_failed`/`event_handshake_failed_no_detail`/`event_handshake_failed_protocol` 10개가 동일 형태(`values[1]={x}; event(...)`)이고 이벤트 상수·스칼라 이름만 다름 | 파라미터화된 private 헬퍼 1개로 병합, 각 함수는 1줄 호출로 축소 | 파일 1(≈60~150줄 감소) | 없음 | 없음 |
| 7 | 2(중복, 확인 필요) | `socket_send_submit.cpp:232-278`(`try_dealer_completion_submit_fast`) vs `:542-597`(`try_request_admission_submit_fast`의 DEALER 분기) | 동일 형태(fast_scope→process_submit_commands→xselect_routed_submit_pipe→try_admit_send_parts_scoped→실패 시 target 구체화)의 복사-변형. 단 errno 처리가 다름: 전자는 `retryable_logical_send_errno(fast_errno)` 폭넓은 판정, 후자는 `fast_errno != EAGAIN`만 확인 — 의도된 차이인지 드리프트(버그)인지 불명 | 병합 전에 WRITABLE 토큰 계약 문서·`retryable_logical_send_errno` 호출부(`:114`)를 대조해 두 errno 판정이 같아야 하는지 확인 후 공유 fast-path로 통합 | 파일 1 | 확인 필요(D 후보 — errno 판정 통일 시 동작 변경 가능) | 없음 |
| 8 | 2/3(중복·얕은 모듈, 확인 필요) | `socket_base_lifecycle.cpp`의 `try_inc_mailbox_ref`/`inc_mailbox_ref`/`dec_mailbox_ref` (socket_base_t 레벨) vs `socket_lifecycle_runtime.cpp`(`socket_lifecycle_coordinator_t`) | `socket_base_t` 쪽은 coordinator로의 순수 pass-through로 보임(fork 조사, 정밀 diff 미실시) | pass-through 확인 후 `socket_base_t` 래퍼 제거, 호출자가 coordinator를 직접 사용 | 파일 2 + 호출자 다수 | 확인 필요 | 없음 |
| 9 | 4/5(정보 누출, 확인 필요·집계 항목) | `socket_base_msg.cpp`/`socket_send_submit.cpp`/`socket_send_complete.cpp`/`socket_base_endpoint.cpp`/`socket_base_lifecycle.cpp`/`socket_base_monitor.cpp` 전역 30여 곳 | `options.type == ZLINK_CORE_SOCKET_{PAIR,DEALER,ROUTER,STREAM,...}` 리터럴 분기가 공용 계층에 산재. 예: `socket_send_complete.cpp:365`는 PAIR을 직접 특별 취급하는데, 정확히 이 용도의 가상 함수 `xtry_send_complete_record`(오버라이드는 `pair_t` 하나뿐, `socket_base.hpp:859`)가 이미 존재 — 두 메커니즘이 병존 | 개별 삭제 대상 아님(핫패스 vtable 회피가 의도일 가능성 — S-5 참고) — 별도 확인 job에서 일관성 감사: 이미 있는 술어(`socket_base_flow_state.cpp:21`, `socket_send_complete.cpp:78-80`)로 얼마나 통합 가능한지, 가상 함수와 리터럴 분기 중 무엇이 최신인지 | 잠재적으로 6파일 | 확인 필요 | 확인 필요(vtable 비용 상쇄 여부) |

집계: 분류1(dead) 1건, 분류2(중복) 2건+얕은 모듈 겹침 1건, 분류3(얕은 모듈) 3건(2·4번 겹침 포함), 분류4(소유) 2건, 분류5는 9번에 부분 포함. "확인 필요" 표시 3건(7·8·9). fork 조사에서 반려된 오탐 11건(dead-code 7건, 중복 4건) — 상세는 하위 작업 로그 참고, 본 표에는 미기재.

## 적용 job 묶음 제안

1. **묶음 A(기계적·저위험, ~0.5h)**: #1, #2, #6. 파일: `routed_submit_target.hpp`, `socket_close_ops.{hpp,cpp}`(+`monitor_api.cpp` 호출부 1줄), `socket_base_monitor.cpp`. 순서: 죽은 typedef 삭제 → close_ops 인라인 → 이벤트 래퍼 10개 병합.
2. **묶음 B(S-13 send-submit, ~1.5h)**: #3(a) 안전 삭제 먼저, 이어서 #7 errno 대조(WRITABLE 계약 문서 확인) 후 fast-path 통합, 여유 있으면 #3(b) 프레임 병합 설계 착수(코드 변경 없이 방안만). 파일: `socket_base.hpp`, `socket_base_msg.cpp`, `socket_send_complete.cpp`, `socket_send_submit.cpp`.
3. **묶음 C(테스트 전용 표면 정리, ~1h)**: #5. 파일: `socket_base.hpp`(test_* 구간) + `core/tests/unittest/{unittest_monitor_ready_drain,unittest_flow_state_monitor,unittest_zmp_engine_controls,unittest_flow_state_socket,unittest_phase3_request_reply_owners,unittest_single_lane_accounting,contract_socket_pair_fixture.hpp}`. A·B와 파일 겹침 없음.
4. **후속(별도 확인 job, 코드 적용 아님)**: #4, #8, #9 — 코드 변경 전 조사 필요(파일 분리 시 LTO 영향, pass-through 여부, vtable-vs-리터럴 일관성). 이후 적용 job으로 승격.

보고 경로: `doc/plan/c016-worklog/core-rf-R4-inventory.md`
