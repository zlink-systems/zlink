# Socket — XPUB 스펙-구현 gap 감사

> 감사 도구: codex (GPT-5, 정적 대조) · 2026-08-24
> 검증 범위: `core/include/`, `core/src/`, 필요한 `core/tests/` 표본. 실행 테스트 없음.

판정: **구현/문서 gap 6건, 요확인 1건**. 코드와 대상 스펙은 수정하지 않았으며, 지정된 감사 보고서만 작성했다.

## 대조 완료 계약군

- `zlink_pub_option_t`의 공개 enum 이름·값 `0x3301..0x3309` 및 PUB/XPUB 전용 option API signature: 일치
- `ZLINK_PUB_OPT_NODROP` 기본값 `0`, lossy fanout 및 `NODROP=1`의 HWM admission/backpressure: 일치
- `zlink_publish_part`의 PUB/XPUB type 제한, topic frame 삽입, part 소비, same-thread/topic/flag multipart sequence, 실패 rollback: 대체로 일치
- `zlink_xpub_recv_part`의 구독 byte 형식, subscribe/unsubscribe 값, `EAGAIN`/`ETERM` 및 `EMSGSIZE`/`EINVAL`의 공개 result 변환: 대체로 일치
- XPUB receive-flow 미지원, `ENOTSUP`/`ZLINK_CONFIG_NOT_SUPPORTED`, flow monitor detail/event 비발생: 일치

## Gap 목록

| 분류 | 스펙 근거 | 코드 근거 | 판단 |
|---|---|---|---|
| A. 문서 누락 | `socket/04-xpub.ko.md:78-86,90-91` — option 표는 `NODROP`만 기본값 `0`으로 명시 | `core/src/runtime/sockets/pubsub/xpub.cpp:13-33,88-100,259-275` | 공개 조회 가능한 기본 상태인 `VERBOSE=0`, `VERBOSER=0`, `MANUAL=0`, `MANUAL_LAST_VALUE=0`, 빈 `WELCOME_MSG`가 문서에 없다. 빈 welcome message는 새 pipe에 아무 message도 보내지 않는 동작이며, `TOPICS_COUNT`도 초기에는 0이다. API 사용자가 option 조회·신규 subscriber 수신을 예측할 기본 계약이 빠졌다. |
| C. 문서-코드 모순 | `socket/04-xpub.ko.md:78-82` — `VERBOSE`·`VERBOSER`·`MANUAL`·`MANUAL_LAST_VALUE`·`NODROP`는 각각 `int; 0 또는 1` | `core/src/runtime/sockets/pubsub/xpub.cpp:197-218` | 구현은 크기만 `sizeof(int)`인지 확인하고 음수만 거절한다. 따라서 `2` 이상도 성공하며 모두 enabled로 해석된다. 문서의 허용 값 집합 `0 또는 1`과 실제 setter 계약이 다르다. |
| A. 문서 누락 | `socket/04-xpub.ko.md:41-43,81` — `MANUAL_LAST_VALUE`를 수동 모드 최신 값 caching으로만 설명 | `core/src/runtime/sockets/pubsub/xpub.cpp:212-218,265-269,370-376` | `MANUAL_LAST_VALUE=1`은 caching만 켜는 것이 아니라 `_manual`도 켜며, 마지막 구독 이벤트 pipe에 대한 다음 발행의 matching 방식도 바꾼다. `0`은 manual도 끈다. 이 option의 manual-mode 전이와 last-pipe 전달 효과가 공개 문서에 없다. |
| B. 구현 gap | `socket/04-xpub.ko.md:194-200,239` — 성공한 `*source_rid_out_` pointer는 **그 socket에 대한 다음 호출 전까지** 유효 | `core/src/api/socket/socket_message_recv_api.cpp:19-22,420-451` | 반환 pointer는 socket별 storage가 아니라 호출 thread마다 하나인 `xpub_recv_source_rid_tls()`다. 같은 thread에서 다른 XPUB에 `zlink_xpub_recv_part`를 성공 호출하면 이전 XPUB의 pointer 내용도 즉시 덮어쓴다. 원래 XPUB에 다음 호출이 없더라도 유효해야 한다는 보장을 만족하지 못한다. |
| A. 문서 누락 | `socket/04-xpub.ko.md:194-211,237-242` — topic buffer 용량 초과 시 `EMSGSIZE`만 정의 | `core/src/api/socket/socket_message_recv_api.cpp:391-414,428-451` | 구현은 event를 socket에서 먼저 dequeue한 뒤 용량을 검사한다. 부족하면 `*topic_id_len_out_`을 필요한 topic byte 길이로 바꾸고 `EMSGSIZE`를 반환하며, 해당 event는 재시도할 수 없다. 이 소비·required-length in/out 규칙과 `source_rid_out_ == NULL` 허용 동작이 문서에 없다. |
| B. 구현 gap | `socket/04-xpub.ko.md:227-260` — 열거한 public surface 계약마다 unit test 하나를 요구 | `core/tests/integration/test_xpub_topic.cpp:24-48`; `core/tests/integration/test_xpub_nodrop.cpp:346-409`; `core/tests/integration/test_backpressure_matrix.cpp:1026-1087`; `core/tests/integration/test_flow_state_c_api.cpp:268-275` | XPUB test 표본은 legacy `zlink_recv`/`zlink_send` 또는 socket-type 일괄 flow-state 결과만 확인한다. `zlink_xpub_recv_part`를 직접 호출하는 test와, 그 API의 routing-ID lifetime·buffer-too-small 소비·상세 errno를 확인하는 contract test가 없다. `zlink_publish_part` 직접 test도 multipart의 max-message-size 한 가지 failure만 다루며, §7의 topic insert/type/소비/rollback/NODROP 요구를 각각 검증하지 않는다. |

## 요확인

- `socket/04-xpub.ko.md:36-43,194-200`은 subscription event와 manual approve/reject의 순서를 설명하지만, concurrent arrival 중 어느 event의 peer를 `ZLINK_PUB_OPT_APPROVE_SUBSCRIBE`/`ZLINK_PUB_OPT_REJECT_SUBSCRIBE`가 대상으로 삼는지 명시하지 않는다. 구현은 `core/src/runtime/sockets/pubsub/xpub.cpp:225-232,431-441`의 단일 `_last_pipe`에 의존한다. 연속 event와 다른 thread의 option setter를 섞는 실제 process test가 있어야 이 관찰창의 public 안전성을 확정할 수 있다.
