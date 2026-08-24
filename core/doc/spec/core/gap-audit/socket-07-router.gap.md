# Socket — ROUTER 스펙-구현 gap 감사

> 감사 도구: codex (GPT-5, 정적 대조) · 2026-08-24
> 검증 범위: `core/include/`, `core/src/`, 필요한 `core/tests/` 표본. 실행 테스트 없음.

판정: **구현/문서 gap 4건, 요확인 없음**. 코드와 대상 스펙은 수정하지 않았으며, 지정된 감사 보고서만 작성했다.

## 대조 완료 계약군

- `zlink_router_option_t` ABI 값, `ZLINK_PART_FINAL`/`ZLINK_PART_MORE` 값, ROUTER option 기본값(`MANDATORY=1`, `PROBE=0`, request timeout `5000`, weight `100`): 대체로 일치
- ROUTER option set/get signature, `CONNECT_ROUTING_ID`의 다음 connect 한정 alias 소비, mandatory directed submit 및 probe 전송: 대체로 일치
- directed/exact raw submit의 RID·pair generation fence, stale/detach no-reroute, multipart staging·rollback·part 소비: 일치
- request의 중간/마지막 part 인자 제약, pending correlation 선등록·실패 제거, completion 전달과 raw receive 분리: 일치
- `zlink_router_recv_part()`/`_v2()`의 필수 output, `DONTWAIT`의 `EAGAIN`, multipart metadata·part ownership: 일치
- receive-flow의 socket 범위 absolute state, ready pair fanout·신규 pair 동기화, generation/epoch의 정상 stale 검출과 peer별 PAUSE 적용: 대체로 일치

## Gap 목록

| 분류 | 스펙 근거 | 코드 근거 | 판단 |
|---|---|---|---|
| A. 문서 누락 | `socket/07-router.ko.md:230-261` — ROUTER receive 공개 surface는 `zlink_router_recv_part()`와 `_v2()`만 정의 | `core/include/zlink/socket/api.h:543-548`; `core/src/api/socket/socket_retained_part_api.cpp:376-477`; `core/tests/integration/test_retained_hwm_credit.cpp:281-305` | export된 `zlink_router_recv_part_v2_with_hwm_budget_lease()`가 없다. 이 variant는 `_v2` metadata에 필수 `lease_out_`을 더해 payload part의 retained HWM credit lease를 반환한다. Socket 공통의 generic retained-credit 설명은 있어도, ROUTER 스펙의 receive API·검증 요구에는 이 signature와 ROUTER별 lease 결과가 없다. |
| C. 문서-코드 모순 | `socket/07-router.ko.md:79-80,118-119` — `MANDATORY`와 `PROBE`는 각각 `int; 0 또는 1` | `core/src/runtime/sockets/router/router.cpp:189-228` | 두 setter는 `sizeof(int)` 및 `value >= 0`만 검증하고 `2` 이상의 값도 성공시킨 뒤 enabled로 해석한다. 문서의 허용 값 집합과 실제 입력 계약이 다르다. |
| B. 구현 gap | `socket/07-router.ko.md:278-283,382-384` — completion lane capacity를 이유로 raw/error reply가 `ZLINK_SUBMIT_BACKPRESSURED`를 반환하지 않음 | `core/src/api/socket/socket_request_reply_runtime_io.cpp:786-816`; `core/src/api/socket/socket_request_reply_submit_api.cpp:515-524`; `core/src/api/message/submit_result_internal.hpp:20-28` | completion pipe가 없거나 `write`/`write_and_flush`가 실패하면 구현은 `EAGAIN`으로 끝낸다. `zlink_router_reply_part()`는 이를 submit result로 변환하고, `EAGAIN`은 `ZLINK_SUBMIT_BACKPRESSURED`다. 따라서 completion lane capacity가 raw reply를 backpressure할 수 없다는 보장을 만족하지 못한다. |
| B. 구현 gap | `socket/07-router.ko.md:312-318,404-408` — 현재 pair/generation·새 epoch 이외의 flow frame은 stale로 무시하고 `ZLINK_EVENT_FLOW_STATE_STALE`로 보고 | `core/src/runtime/sockets/common/socket_base_flow_state.cpp:204-218,244-250,380-401`; `core/tests/integration/test_flow_state_c_api.cpp:408-478` | generation 불일치와 duplicate/reversed epoch는 stale event를 남기지만, 다른 pair ID를 가리키는 frame은 line 209-210에서 event 없이 버린다. 문서가 포괄하는 “그 밖의 frame” 보고 의무와 다르며, 현재 test도 generation/epoch stale만 확인한다. |

## 요확인

- 없음. 위 판단은 실행 없이 공개 header·구현 경로·기존 test 표본을 정적으로 대조한 결과다.
