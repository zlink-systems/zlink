
[레퍼런스 목차](README.ko.md)

# 05. Raw receive

이 category는 raw socket 타입이 공유하는 part 기반 DATA 수신을 다룬다. Socket-type 전용 수신
family가 적용될 수도 있다. 아래 표와 각 socket 타입 자신의 category를 참고한다. STREAM은 첫
bind 또는 connect 전에 RAW/PACKET도 명시적으로 선택한다. 정확한
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

**Parameters.** `source_rid_out_`는 선택적이며 Core 소유 view를 받는다(같은 socket의 다음
data receive 진입 이후에도 필요하면 복사한다 — `STREAM`은 실제 view를, `PAIR`와 `DEALER`는
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

## STREAM RAW와 PACKET receive

STREAM socket은 첫 bind 또는 connect에 성공하기 전에 pull receive family 하나를 고른다.

```c
zlink_stream_recv_mode_t mode = ZLINK_STREAM_RECV_MODE_PACKET;
zlink_set_stream_option(stream_socket, ZLINK_STREAM_OPT_RECV_MODE,
                        &mode, sizeof(mode));
```

**Parameters.** `ZLINK_STREAM_RECV_MODE_RAW`는 `zlink_recv_part()`를,
`ZLINK_STREAM_RECV_MODE_PACKET`은 `zlink_stream_recv_packet()`을 고른다. PACKET receive는
caller가 초기화한 `header_out_`와 `body_out_` message를 채우고 source routing-id view를 반환한다.

**Return과 errno.** Mode를 고르지 않고 bind 또는 connect하면 `EINVAL`로 실패한다. 첫 bind 또는
connect 성공 뒤에는 현재 값으로 설정해도 `EBUSY`로 실패한다. 다른 mode의 receive family를
호출하면 `ZLINK_RECV_NOT_SUPPORTED`와 `ENOTSUP`을 반환한다.

**선택 기준.** Framing 없는 byte record에는 RAW를 골라 `ZLINK_POLLIN`과 함께 쓴다. Wire
protocol이 Core의 고정 header/body framing을 사용하면 PACKET을 고른다. STREAM category를 참고한다.

---

## Socket 타입별 수신 표면

| Socket 타입 | 수신 표면 | 비고 |
|---|---|---|
| PAIR | `zlink_recv_part()` | part 수신만 |
| DEALER | `zlink_recv_part()` + `zlink_completion_recv()` | DATA는 part receive, 제출한 request 결과는 completion receive |
| SUB / XSUB | `zlink_subscribe_part()` | topic-part 수신만 — SUB·XSUB category 참고 |
| ROUTER | `zlink_router_recv_part()` + `zlink_completion_recv()` | DATA/REQUEST는 part receive, 제출한 request 결과는 completion receive |
| STREAM | `zlink_recv_part()` 또는 `zlink_stream_recv_packet()` | bind/connect 전에 RAW 또는 PACKET 선택 — STREAM category 참고 |
| PUB | N/A | send 전용 |
| XPUB | `zlink_xpub_recv_part()`(구독 이벤트, recv 전용) | data plane은 send — XPUB category 참고 |
| monitor / timer | pull receive | Socket monitor·Timers category 참고 |

`DEALER`/`ROUTER`의 REQUEST completion queue는 operation 완료 표면이지 DATA 수신이 아니다.
STREAM은 endpoint를 활성화하기 전에 두 pull receive mode 중 정확히 하나를 고른다.

---

전체 근거는 [Socket 공통 스펙](../spec/core/socket/README.ko.md)을 참고한다.
