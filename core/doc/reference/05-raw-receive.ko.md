한국어 | [English](05-raw-receive.en.md)

[레퍼런스 목차](README.ko.md)

# 05. Raw receive

이 category는 socket 타입 전체가 공유하는 두 수신 진입점 — part 기반 수신과 raw callback을
다룬다. 둘 중 무엇을(또는 socket-type 전용 수신 family를) 특정 socket 타입에 쓸지는 실행 중
선택이 아니라 고정되어 있다 — 아래 표와 각 socket 타입 자신의 category를 참고한다. 정확한
signature는 [Socket 공통 스펙](../spec/core/socket/README.ko.md)이 소유한다.

---

## `zlink_recv_part`

Raw socket에서 메시지 part 하나를 동기적으로 수신한다.

```c
zlink_msg_t part;
zlink_msg_init(&part);

const zlink_routing_id_t *source_rid;
zlink_part_flag_t has_more;
zlink_recv_result_t result = zlink_recv_part(s, &source_rid, &part, &has_more, ZLINK_RECV_FLAGS_NONE);
```

**Parameters.** `source_rid_out_`는 선택적이며 Core 소유 view를 받는다(같은 스레드의 다음
raw recv 호출 이후에도 살려야 하면 복사한다 — `STREAM`은 실제 view를, `PAIR`와 `DEALER`는
`NULL`을 반환한다). `part_out_`는 이미 초기화된 message를 가리켜야 하며 필수다.
`has_more_out_`는 필수이며 `ZLINK_PART_MORE` 또는 `ZLINK_PART_FINAL`로 설정된다. `flags_`는
`ZLINK_RECV_FLAGS_NONE`(blocking) 또는 `ZLINK_RECV_FLAGS_DONTWAIT`다.

**Return과 errno.** `zlink_recv_result_t`를 반환한다 — 성공하면 `ZLINK_RECV_OK`이며 수신한
part의 소유권이 caller에게 이전된다(`zlink_msg_close`/`zlink_multipart_close`로 정확히 한 번
닫는다 — Message category). 지원하지 않는 socket 타입이면 `ZLINK_RECV_NOT_SUPPORTED`와
`ENOTSUP`. `ZLINK_RECV_FLAGS_DONTWAIT`를 쓰면 받을 part가 없을 때 `ZLINK_RECV_NO_DATA`와
`EAGAIN`을 반환한다. 실패 시에는 part 소유권이 이전되지 않는다.

**선택 기준.** 지원하는 타입은 raw `PAIR`, `DEALER`, `STREAM`이다. `PUB`, `XPUB`, `SUB`,
`XSUB`, `ROUTER`는 여기서 지원하지 않는다 — 대신 각자의 전용 수신 진입점을 쓴다(SUB·XSUB
category의 `zlink_subscribe_part`, ROUTER category의 `zlink_router_recv_part`). 한
multipart message의 모든 part를 첫 part부터 마지막 part까지 같은 스레드에서 이 함수로
수신한다. 기본 `recv + poller` 모델을 위해 `ZLINK_POLLIN`을 관찰하는 poller(Polling and
pollers category)와 짝지어 쓴다.

---

## `zlink_recv_handler`

`STREAM` socket에 raw 수신 callback을 붙여 `recv + poller` 모델을 push 전달로 대체한다.

```c
zlink_recv_handler(stream_socket, on_raw_message, userdata);
```

**Parameters.** `handler_`는 `zlink_socket_msg_handler_fn`이다 — 소유 I/O thread에서
source routing ID, message part 배열, part 개수와 함께 호출된다. 모든 part의 소유권이
callback으로 이전되며 각 part는 정확히 한 번 닫아야 한다. `userdata_`는 그대로 전달된다.

**Return과 errno.** `zlink_handler_result_t`를 반환한다 — 성공하면
`ZLINK_HANDLER_OK`. Raw `STREAM` 외의 subject면 `ENOTSUP`. 성공적으로 attach된 뒤에는
같은 handle의 `zlink_recv_part`, `zlink_stream_packet_handler`(STREAM category),
data-plane poller `ZLINK_POLLIN`이 `EBUSY`로 실패한다 — 같은 handle에 두 번째 attach도
`EBUSY`로 실패한다.

**선택 기준.** 이것 또는 `zlink_recv_part`, `zlink_stream_packet_handler`(STREAM category)
중 하나를 고른다 — `STREAM` handle마다 정확히 하나의 raw 수신 모드다. 아래 수신 표면 표와
packet-framed 대안은 STREAM category를 참고한다.

---

## Socket 타입별 수신 표면

| Socket 타입 | 수신 표면 | 비고 |
|---|---|---|
| PAIR | `zlink_recv_part()` | part 수신만 |
| DEALER | `zlink_recv_part()`(+ `zlink_dealer_request_part()` 완료 callback) | part-receive data plane |
| SUB / XSUB | `zlink_subscribe_part()` | topic-part 수신만 — SUB·XSUB category 참고 |
| ROUTER | `zlink_router_recv_part()`(+ `zlink_router_request_part()` 완료 callback) | part-receive data plane — ROUTER category 참고 |
| STREAM | `zlink_recv_part()` / `zlink_recv_handler()` / `zlink_stream_packet_handler()` | 예외: 정확히 하나의 모드를 고른다 — STREAM category 참고 |
| PUB | N/A | send 전용 |
| XPUB | `zlink_xpub_recv_part()`(구독 이벤트, recv 전용) | data plane은 send — XPUB category 참고 |
| monitor / timer | recv와 callback 둘 다 지원 | Socket monitor·Timers category 참고 |

`DEALER`/`ROUTER`의 request 완료 callback은 비동기 operation 완료 표면이지 data-plane
수신 callback이 아니다 — 둘 다 callback을 쓰지만 역할은 분리되어 있다. `STREAM`은
application이 handle마다 세 수신 모델 중 하나를 고르는 유일한 예외다 — 같은 handle에서 다른
모드를 활성화하려는 두 번째 시도는 `EBUSY`로 실패한다.

---

전체 근거는 [Socket 공통 스펙](../spec/core/socket/README.ko.md)을 참고한다.
