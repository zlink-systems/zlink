# Phase 3 인벤토리 — R7 (`core/src/api/core/` + `core/src/api/socket/`(request_reply 제외) + `core/src/api/monitoring/` + `core/src/api/message/`)

기준 커밋: f29848edab. 읽기 전용 조사, 빌드·실행 없이 정적 검토(grep + 수동 대조)만 수행.

## 표

| # | 분류 | file:line | 관찰 | 제안 | 반경 | 계약 | 성능 |
|---|---|---|---|---|---|---|---|
| 1 | dead code | `core/src/api/core/zlink_option_internal.hpp:17`, `zlink_option_mapping.cpp:24-108`(전체 테이블), `zlink_option.cpp:122-125,149-152` | `option_descriptor_t::unsupported_on_socket` 필드가 모든 테이블 항목(common/router/dealer/stream/pub/sub, 총 60여 개)에서 항상 `false`. 이를 검사하는 `if (descriptor->unsupported_on_socket) { … EINVAL … }` 분기 2곳은 정적으로 항상 미실행(dead branch). 4534b0705b가 ZLINK_OPT_TYPE 매핑을 고쳤을 때 이 필드가 실제로 쓰인 적이 있는지 git log 확인 필요 — 지금은 완전히 죽은 플래그. | 필드와 두 dead 분기를 제거. 제거 전 `git log -p` 로 도입 이력과 원래 의도(향후 옵션을 위한 자리인지) 확인. | 파일 3개, ~15행 | 없음(항상 false였으므로 외부에서 관측 불가 동작 없음) | 없음 |
| 2 | 중복 | `core/src/api/socket/socket_message_send_api.cpp:63`(로컬 `validate_send_flags(int)`, errno=ENOTSUP) vs `core/src/api/socket/part_helper_api.cpp:245`(`zlink::part_helper_internal::validate_send_flags(zlink_send_flags_t)`, errno=EINVAL) | 이름이 완전히 같은 두 함수가 동일한 검사(`flags_ != 0 && flags_ != ZLINK_DONTWAIT`)를 하면서 **다른 errno**를 설정한다. 호출 경로에 따라 잘못된 flags 값을 넘긴 사용자가 ENOTSUP(불특정 send 경로: `send_socket_unrouted_parts`/`send_socket_routed_parts`가 거치는 `validate_socket_send_request`)를 받거나 EINVAL(routed/part-by-part 경로, `socket_message_send_api.cpp:588,658,720`)를 받는다. | 계약 문서(03-errors 스펙) 확인 후 하나로 통일 — 어느 errno가 맞는 계약인지 확인 필요. 통일되면 로컬 사본 제거하고 `part_helper_internal::validate_send_flags` 하나만 사용. | 파일 2개, ~15행 | **있음 → D 필요**: 어느 errno가 옳은 계약인지 spec 확인 없이 바꾸면 회귀. "확인 필요" — 03-errors 문서와 기존 유닛테스트에서 flags 오류 시 기대 errno를 먼저 확인. | 없음 |
| 3 | 얕은 모듈/중복 | `core/src/api/core/zlink.cpp:157-167`(`api_sync_mutex()` 있는 분기 내부) vs `zlink.cpp:170-180`(같은 함수 `zlink_close` 안, else 경로) | `drain_close_request_reply_socket` → `cleanup_request_reply_socket` → `complete_close_handoff` → 에러 처리 6줄이 `zlink_close` 안에서 글자 그대로 복제되어 있음(STREAM 뮤텍스 보유 여부만 다름). | 공통 tail을 로컬 helper 함수(예: `finish_close_after_drain(handle, deferred_close)`)로 추출. | 파일 1개, ~20행 | 없음(순수 리팩터, 동작 불변) | 없음 |
| 4 | 중복 | `core/src/api/monitoring/poller_registration.cpp:73-125`(`poller_add_registration`, 소켓용) vs `:130-175`(`poller_add_fd_registration`, fd용) | reserve→native add→registration 채우기→push_back+index map emplace→실패 시 롤백까지 구조가 거의 동일(소켓/포인터 필드만 상이). `poller_remove_registration_at`(`:217-`)와 대칭되는 짝도 유사 패턴일 가능성. | 공통 골격을 템플릿/공통 helper로 뽑는 리팩터 고려. 다만 poller add/remove는 계약 경계(`zlink_poller_add*`)와 밀접하므로 우선순위는 낮음 — "확인 필요": 실제 호출부(zlink_poller_add, zlink_poller_add_fd)가 이 구조 차이를 계약상 요구하는지 재확인 후 진행. | 파일 1개, ~60행(추출 시) | 없음(추출만 하면) 있음(실패 롤백 로직을 잘못 합치면 있음) | 없음 |
| 5 | 얕은 모듈(경미) | `core/src/api/core/zlink_option_specialized_api.cpp:236-305` (`zlink_set_pub_option`/`zlink_get_pub_option`/`zlink_set_sub_option`/`zlink_get_sub_option`) | 4개 함수가 `map_*_option`→`resolve_option_target`→`set/get_socket_option_checked` 순서를 그대로 반복(각 8~14행), 소켓 타입 쌍(PUB/XPUB vs SUB/XSUB)만 다름. | 공통 템플릿 helper(`option_type_a`, `option_type_b`, setter/getter 함수포인터)로 축약 가능하나 이득 대비 반경이 애매 — 낮은 우선순위. | 파일 1개, ~40행 | 없음 | 없음 |
| 6 | dead code(경미) | `core/src/api/socket/inline_msg_buffer_internal.hpp:82` (`capacity()`) | 템플릿 멤버 `capacity()`가 `part_helper_internal.hpp`/`request_reply_frame_buffer_internal.hpp`의 3개 typedef 인스턴스화 어디에서도 호출되지 않음(`grep -rn "\.capacity ()"` 결과 core/src 전체에서 이 타입에 대한 호출 0). | 제거 또는 유지(향후 계측용) 판단 필요 — "확인 필요": 템플릿이라 인스턴스화되지 않은 멤버는 바이너리에 영향 없음(성능 영향 없음), 제거는 순수 클린업. | 파일 1개, 1줄 | 없음 | 없음 |
| 7 | 중복(중간확신) | `core/src/api/core/close_result_internal.hpp`, `config_result_internal.hpp`, `core/src/api/message/{bind,connect,handler,recv,request,submit}_result_internal.hpp` | 8개의 `*_result_internal.hpp`가 각자 `from_errno`/`from_rc`를 손으로 구현. 공통 helper(`result_errno_internal::is_not_supported`, `rc_errno_or_io`)는 이미 공유되고 있어 완전한 중복은 아니지만, `0→OK`, `EFAULT→INVALID_HANDLE`, `EINVAL→INVALID_ARGUMENT`, `EBUSY→BUSY류` 매핑이 파일마다 반복 등장. | 완전 통합은 각 API의 의미론적 차이(예: request/submit은 `internal_errno::classify_*` 사용) 때문에 위험 — "확인 필요": 03-errors 스펙 문서가 이 매핑을 어디까지 표준으로 규정하는지 먼저 대조. 통합보다는 "표준 사례는 그대로 두고, 진짜 다른 것만 override"하는 매크로/공통 base가 실익 있는지 재평가 필요. | 파일 8개(전수 조사 시) | 있음 → D(계약 상 각 result enum이 이미 다르므로 기계적 병합 금지) | 없음 |

## 확인 필요 상세

- #2: 어느 errno(EINVAL vs ENOTSUP)가 계약인지 03-errors 스펙/기존 테스트로 확정 후 처리. 잘못 고치면 사용자 관측 가능한 errno 회귀.
- #4, #7: 구조적 유사성은 명확하지만 병합이 계약 안전한지는 스펙 대조 필요. 이번 인벤토리에서는 항목만 등록하고 병합 여부는 apply 단계에서 별도 D로 판단 권고.

## 항목 수(카테고리별)

- dead code: 2 (#1, #6)
- 중복: 4 (#2, #3, #4, #7)
- 얕은 모듈: 1 (#5, #3도 얕은 모듈성 겸함)
- 잘못된 소유: 0 (조사 범위 내 확인된 것 없음 — socket_base.hpp 등 헤더 노출 문제는 R7 파일들 자체에서는 발견 안 됨)
- 이름-개념 불일치: 0 (`stream_api_lock_t`는 처음 의심했으나 `api_sync_mutex()`가 실제로 `stream_t`에서만 override되어 이름과 실제 일치함을 확인 — 오탐 제외)

합계 7항목 (경미 항목 2개 포함). 확신 없는 항목(#2 errno 선택, #4/#7 병합 여부)은 위 "확인 필요"에 명시.

## 적용 job 묶음 제안 (파일 겹치지 않음, 각 ≤1.5h)

- **묶음 A** — `core/src/api/core/` 중심, 저위험 클린업: #1(unsupported_on_socket 제거) + #3(zlink_close 중복 tail 추출). 파일: `zlink_option_internal.hpp`, `zlink_option_mapping.cpp`, `zlink_option.cpp`, `zlink.cpp`. 순서: #1 → #3 (서로 독립, #1 먼저 컴파일 확인 후 #3).
- **묶음 B** — send-flags errno 정합화: #2 하나만. 먼저 03-errors 스펙 확인(D 필요) 후 코드 수정. 파일: `socket_message_send_api.cpp`, `part_helper_api.cpp`(또는 `part_helper_internal.hpp` 시그니처). D가 나기 전에는 착수 보류 권고.
- **묶음 C** — 경미 클린업 일괄: #5(pub/sub option helper 축약) + #6(`capacity()` 제거). 파일: `zlink_option_specialized_api.cpp`, `inline_msg_buffer_internal.hpp`. 서로 무관, 같은 job에서 순차 처리 가능.
- (#4, #7은 이번 라운드에서 적용 보류 — 계약 대조 D 이후 별도 라운드로 넘김)
