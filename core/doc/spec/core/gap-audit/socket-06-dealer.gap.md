# DEALER 스펙–구현 gap 감사

> 감사 도구: codex (정적 대조, read-only) · 2026-08-24
> 감독 검증: 공개 C ABI, DEALER load balancer, request/reply receive·completion, receive-flow 경로를 코드 표본으로 대조

판정: **구현/문서 gap 5건, 요확인 0건**. 코드와 스펙 문서는 수정하지 않았고, 실행 테스트 없이 정적 대조만 수행했다.

## 대조 완료 계약군

- 공개 enum 값과 `zlink_send_part`·DEALER part API signature: 일치
- `ZLINK_DEALER_OPT_WEIGHT`의 기본값 `100`, 허용 범위 `0..10000`, peer 전파: 일치
- smooth weighted 선택의 누적값 갱신, routing-ID 우선 tie-break, multipart exact-pipe fence: 일치
- request의 final-handler 요구, 기본 timeout `5000ms`, pending correlation의 wire 전 등록: 일치
- 수신 part의 multipart 순서·소유권, request reply token의 final 성공 후 소진: 일치
- completion lane의 HWM/LWM·Core budget 비적용: 일치

## Gap 목록

| 분류 | 스펙 근거 | 코드 근거 | 판단 |
|---|---|---|---|
| C. 문서-코드 모순 | `06-dealer.ko.md:229-233` — `ZLINK_DEALER_OPT_PROBE`는 `int`, 허용값 `0` 또는 `1` | `core/src/runtime/sockets/dealer/dealer.cpp:83-103` | 구현은 `sizeof(int)`인 모든 `value >= 0`을 성공으로 처리하고 0 이외를 모두 enabled로 저장한다. 예를 들어 `2`가 성공하므로 0/1 전용 계약과 다르다. |
| C. 문서-코드 모순 | `06-dealer.ko.md:236-238`, `398` — get의 `*optvallen_`은 입력 용량이고 성공하면 실제 쓴 byte 수로 갱신 | `core/src/runtime/sockets/dealer/dealer.cpp:106-117` | PROBE get은 입력 크기가 정확히 `sizeof(int)`일 때만 성공하며, 성공해도 `*optvallen_`을 갱신하지 않는다. 더 큰 유효 용량을 허용하고 실제 크기를 반환한다는 계약을 충족하지 않는다. |
| C. 문서-코드 모순 | `06-dealer.ko.md:39-47`, `196-207`, `357-363` — receive는 RAW·REQUEST·REPLY·ERROR_REPLY record 종류를 반환 | `core/src/api/socket/socket_request_reply_runtime_io.cpp:588-655`; `core/include/zlink_enum.h:150-156` | DEALER receive 경로는 request envelope만 `REQUEST`로 export한다. reply 또는 error-reply envelope는 `EPROTO`로 폐기하며, 공개 enum의 `REPLY(2)`·`ERROR_REPLY(3)`를 반환하는 코드 경로가 없다. |
| B. 구현 gap | `06-dealer.ko.md:161-166`, `442` — 현재 pair를 지칭하지 않는 frame도 stale로 무시하고 `ZLINK_EVENT_FLOW_STATE_STALE`로 보고 | `core/src/runtime/sockets/common/socket_base_flow_state.cpp:204-217`, `224-250` | pair ID가 다른 frame, pair/generation이 0인 frame, transport-pair table에 없는 frame, 등록 completion pipe가 아닌 frame은 단순히 소비하고 반환한다. generation mismatch와 epoch stale만 event를 낸다. 따라서 pair 불일치 stale 보고 보장이 빠져 있다. |
| B. 구현 gap | `06-dealer.ko.md:384-388`, `432-434` — completion lane capacity 때문에 `ZLINK_SUBMIT_BACKPRESSURED`를 반환하지 않고, 연결 failure는 그에 맞는 즉시 result | `core/src/api/socket/socket_request_reply_runtime_io.cpp:786-828`; `core/src/api/message/submit_result_internal.hpp:18-31`; `core/src/api/socket/socket_request_reply_submit_api.cpp:594-600` | reply target의 completion pipe를 찾지 못하거나 write가 실패하면 helper가 `EAGAIN`을 설정한다. public mapper는 이를 `ZLINK_SUBMIT_BACKPRESSURED`로 변환한다. completion lane 자체의 HWM은 없더라도, 연결 소멸/pipe race가 backpressure 결과로 노출되어 문서의 failure 구분과 맞지 않는다. |

## 요확인

- 없음.
