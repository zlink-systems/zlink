# R5 인벤토리 — socket_request_reply_* (core/src/api/socket)

기준 HEAD: 2529709db6. 읽기 전용 조사, 빌드/테스트 미실행.

범위: socket_request_reply_{runtime_io,submit_api,internal.cpp,internal.hpp,dispatch,
pending_api,pending_internal.hpp,router_api,router_control,api}.cpp/hpp,
request_timeout_scheduler_internal.{hpp,cpp}, request_reply_frame_buffer_internal.hpp,
request_reply_protocol_internal.hpp, request_reply_runtime_core.hpp.

## 요약

D-B85(REQUEST pending pool 제거, 7d8205a028) 잔재를 전수 조사한 결과, 이름의 "pending"은
전부 **admission pool이 아니라 "응답 대기(reply correlation)" 상태**를 가리키는 것으로 확인됨
(pending_request_t/store_t, add/remove/take_next_socket_pending_request_*, arm/cancel_*timeout).
`request_pending_submit / try_admit / drive_request_pending / fail_pull_request_pending_for_logical_*`
같은 옛 admission-pool 심벌은 R5 파일에 전혀 남아있지 않음(grep 0건) — 커밋 7d8205a028이 완전히
정리한 것으로 판단. `ZLINK_OPT_PENDING_MAX_MSGS/BYTES` no-op 저장 경로는 core/src/runtime/core/
(zlink_option.cpp, options_owner.cpp, options_core_socket.cpp, zlink_option_mapping.cpp)에 있으며
R5 파일 범위 밖이라 표에서 제외(참고로만 기록).

R5 내부 함수 심벌 30여 개를 전수 grep(정의+선언+호출 3-way 카운트)한 결과 **dead code 후보 0건**
— 모두 최소 1개의 실제 호출자(운영 코드 또는 유닛 테스트)가 있음. submit 경로는 D-B85가 의도한 대로
socket_send_submit.cpp의 공유 helper(request_admission_submit / try_request_admission_submit_fast /
request_admission_submit_blocking)를 얇게 감싸는 형태로 잘 통합되어 있어 **REQUEST vs SEND 중복은
발견되지 않음**. 대신 얕은 모듈(항목 3) 위반이 주요 소견.

## 표

| # | 분류 | file:line | 관찰 | 제안 | 반경 | 계약 | 성능 |
|---|------|-----------|------|------|------|------|------|
| 1 | 3 | core/src/api/socket/socket_request_reply_submit_api.cpp:673-948 (`request_part_common`) | 278행, 파라미터 10개(socket_handle_, handle_, peer_rid_, part_, flags_, part_flag_, timeout_ms_, user_context_, family_, completion_id_out_). MORE/FINAL 분기·1-part fast path·helper-state 조립·admission 호출이 한 함수에 혼재 | fast-path(1-part FINAL, 801-838행)와 buffered multipart 경로(840-947행)를 별도 static 함수로 분리, 공통 실패 처리(finish_request_submit_failure 계열)만 공유 | 파일 1개, 예상 ~120행 이동 | 없음(내부 리팩터, 공개 동작 불변) | 없음(순수 함수 분리, inline 유지 시 hot path 영향 없음) |
| 2 | 3 | core/src/api/socket/socket_request_reply_submit_api.cpp:249-271 (`finish_dontwait_request_admission_failure`) | 파라미터 7개, 서로 다른 성격(socket/peer/user_context/state/identity/errno/completion_id/wait) 뒤섞임 | 관련 파라미터를 하나의 `request_admission_failure_ctx_t` 구조체로 묶어 함수 시그니처 단순화 (항목 1 리팩터와 함께 진행 권장) | 파일 1개(호출부 socket_request_reply_submit_api.cpp:809,828,891,902 등도 갱신), ~40행 | 없음 | 없음 |
| 3 | 3 | core/src/api/socket/socket_request_reply_submit_api.cpp:389-397 (`send_public_router_reply_with_wait`) | 파라미터 10개(socket_, send_scope_, request_state_, target_, peer_rid_, reply_token_, staged_parts_, staged_part_count_, final_part_, started_at_) | 항목 2와 동일 패턴 — reply-submit context 구조체로 축약. 확인 필요: send_scope_/started_at_가 호출부마다 다른 값인지(단일 호출자면 인라인도 대안) | 확인 필요: 호출자 1곳뿐인지 grep으로 재확인 후 결정. 파일 1개, ~30행 | 없음 | 없음 |
| 4 | 3 | core/src/api/socket/socket_request_reply_runtime_io.cpp:995-1230 (`recv_router_message_direct`) | 235행 — 250행 임계치는 아직 안 넘었지만 근접. ROUTER 수신 검증·멀티파트 수집·리플라이 타겟 커밋이 한 함수에 있음 | 우선순위 낮음(임계치 미달) — 항목 1 처리 후 여유 있으면 payload 수집 부분(collect_multipart_payload_parts 호출 전후)을 별도 헬퍼로 추출 | 파일 1개, ~60행 | 없음 | 없음 |
| 5 | 5 | core/src/api/socket/socket_request_reply_api.cpp (파일 전체, 53행) | 파일명은 "api"이지만 실제로는 `validate_socket_type` + `stage_request_payload_part` 단 2개의 하위 유틸 함수만 포함 — 공개 API 진입점(zlink_request_part 등)은 submit_api.cpp에 있어 이름이 오해를 줌 | 확인 필요: 이 2개 함수를 submit_api.cpp 또는 internal.cpp로 옮기고 이 파일을 없애거나, 파일명을 `socket_request_reply_shared_helpers.cpp` 등으로 변경. 감독관 판단 필요(파일 삭제는 계약 영향 없으나 빌드 스크립트의 파일 목록 갱신 필요) | 파일 2개(이관 시), ~50행 | 없음(내부 파일 구성) | 없음 |
| 6 | 3 | core/src/api/socket/socket_request_reply_router_control.cpp (90행) vs router_api.cpp:145 | `validate_socket_type (handle, ZLINK_CORE_SOCKET_ROUTER)` 검증 패턴이 router_control.cpp:20과 router_api.cpp:145 두 곳에 반복 — 중복은 아니고 같은 유틸 재사용이라 문제 없음(항목 2 아님, 정상) — 표에는 "이상 없음" 확인 결과로 기록 | 조치 불필요 | — | — | — |
| 7 | 확인 필요 | core/src/api/socket/socket_request_reply_internal.cpp:594-670 (`dispatch_aggregate_timeouts_one_by_one`) vs :670-797(`on_socket_aggregate_request_timeout`) | 이름이 유사하고 인접해 있어 책임 분리가 애매해 보임(aggregate timeout 처리가 두 함수로 나뉜 이유 불명) — 실제 중복인지 역할 분담인지 코드만으로는 판단 보류 | 확인 방법: 두 함수의 diff 이력(git blame)과 D-137 관련 커밋 확인 후 판단. 리팩터 없이 유지 권장(REQUEST 타임아웃 로직은 위험도 높음) | — | 확인 필요 | 확인 필요 |
| 8 | 없음(음성 결과) | R5 전체 | dead code(호출자 0) 없음, REQUEST↔SEND 중복 없음, 옛 pending-pool 심벌 없음 — 3-way grep(선언/정의/호출)으로 30개 심벌 전수 확인 | — | — | — | — |

## 적용 job 묶음 제안

1. **묶음 A** (항목 1, 2) — `socket_request_reply_submit_api.cpp`만 수정: `request_part_common` 분리 + `finish_dontwait_request_admission_failure` 파라미터 구조체화. 파일 1개, 1.5h 이내.
2. **묶음 B** (항목 3, 4) — 같은 파일(submit_api.cpp, runtime_io.cpp)이지만 A와 겹치지 않는 함수만: `send_public_router_reply_with_wait` 구조체화(확인 후) + `recv_router_message_direct` 경미한 추출. A 완료·머지 후 순차 진행 권장(같은 파일 동시 편집 충돌 방지).
3. **묶음 C** (항목 5) — `socket_request_reply_api.cpp` 파일 재배치/삭제. 감독관 결정 필요(POSDDD 파일-개념 매핑 정책 확인 후). 단독으로 빠르게 끝남.

항목 7은 리팩터 대상 아님(확인 필요로 남김, 별도 조사 job 권장, 코드 변경 없음).

보고서 경로: doc/plan/c016-worklog/core-rf-R5-inventory.md
