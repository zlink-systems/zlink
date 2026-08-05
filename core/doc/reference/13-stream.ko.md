한국어 | [English](13-stream.en.md)

[레퍼런스 목차](README.ko.md)

# 13. STREAM

Bind 전용 raw socket으로, 받아들이는 모든 client 연결에 4바이트 routing ID를 부여하고
routed TCP나 WebSocket으로 byte record나 고정 framing packet을 주고받는다. STREAM은
`zlink_connect`를 지원하지 않으며 application payload를 해석하지 않는다.
`zlink_socket`/`zlink_bind`/`zlink_close`(Socket lifecycle category),
`zlink_recv_part`/`zlink_recv_handler`(Raw receive category — STREAM은 `source_rid_out_`가
`NULL` 대신 실제 view를 반환하는 유일한 타입이다), `zlink_send_ready_handler`(Socket
lifecycle category)를 변경 없이 재사용한다 — 이 category는 STREAM 고유의 옵션, 자신만의
directed send 버전, 그리고 STREAM에만 있는 packet-framing 수신 모드를 다룬다. 정확한
signature는 [STREAM 스펙](../spec/core/socket/08-stream.ko.md)이 소유한다.

---

## `zlink_set_stream_option` / `zlink_get_stream_option`

STREAM의 유일한 타입 전용 옵션을 설정하거나 읽는다.

```c
int notify = 1;
zlink_set_stream_option(stream_socket, ZLINK_STREAM_OPT_NOTIFY, &notify, sizeof(notify));
```

**Parameters.** `option_`은 `ZLINK_STREAM_OPT_NOTIFY`(`int` 0 또는 1)다 — `zlink_bind`
전에 설정해야 한다. `1`이면 client 연결·해제를 zero-length data record로 드러내며, source
routing ID가 client를 식별한다.

**Return과 errno.** 둘 다 `zlink_config_result_t`를 반환한다 — 성공하면
`ZLINK_CONFIG_OK`. 공통 HWM·timeout·linger·TLS·buffer 옵션은
`zlink_set_option`/`zlink_get_option`(Socket options and identity category)을 쓴다.

**선택 기준.** Application이 별도 monitor 없이 데이터와 같은 수신 경로로 client
연결·해제를 감지해야 하면 `ZLINK_STREAM_OPT_NOTIFY`를 활성화한다.

---

## `zlink_send_part_rid`(STREAM)

Routing ID로 지정한 특정 연결된 client에게 raw data part 하나를 보낸다 — 같은 함수 이름을
쓰는 ROUTER(ROUTER category)와는 모양이 다른, STREAM의 directed send다.

```c
zlink_send_part_rid(stream_socket, &client_rid, &part, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL);
```

**Parameters.** `target_rid_`는 이 STREAM socket이 부여한 유효한 4바이트 routing
ID여야 한다. `part_flag_`는 `ZLINK_PART_FINAL`이어야 한다 — `ZLINK_PART_MORE`를 넘기면
STREAM send가 절대 multipart sequence를 열지 않으므로 `ZLINK_SUBMIT_NOT_SUPPORTED`와
`ENOTSUP`을 반환한다.

**Return과 errno.** `zlink_submit_result_t`를 반환한다 — 성공하면
`ZLINK_SUBMIT_OK`. 연결이 없으면 `ZLINK_SUBMIT_NOT_CONNECTED`. Backpressure면
`ZLINK_SUBMIT_BACKPRESSURED`와 `EAGAIN` — 다른 socket 타입과 달리 이 특정 실패는
`part_`을 caller 소유로 남겨 그대로 재시도할 수 있게 한다 — 다른 모든 실패(그리고 성공)는
여전히 소비한다.

**선택 기준.** STREAM send는 항상 단일 part이므로 PAIR/DEALER/ROUTER/PUB에 적용되는
atomic multipart-abort 규칙이 여기에는 적용되지 않는다 — `ZLINK_PART_MORE` 실패는
아무것도 스테이징하지 않으며, 다음 호출은 독립된 record다. 호출 전에 payload 복사본을
보관하면 원래 내용이 살아남는 한 가지 경우(backpressure)를 포함해 모든 실패 결과에 대해
하나의 일관된 복구 전략을 쓸 수 있다.

---

## `zlink_stream_packet_handler`

Packet-framed 수신 callback을 붙인다 — `zlink_recv_part`, `zlink_recv_handler`(Raw
receive category)와 함께 STREAM의 서로 배타적인 세 수신 모드 중 하나다.

```c
zlink_stream_packet_handler(stream_socket, on_packet, userdata);
```

**Parameters.** `handler_`는 `zlink_stream_packet_handler_fn`이며, 조립된 packet마다
source routing ID와 별도의 `header_`/`body_` message와 함께 호출된다. 둘 다 길이가
0이어도 유효한 `zlink_msg_t` 값으로 전달되며(`NULL`이 아님), 소유권이 callback으로
이전된다 — 각각 정확히 한 번 소비하거나 닫아야 한다.

**Return과 errno.** `zlink_handler_result_t`를 반환한다 — 성공하면
`ZLINK_HANDLER_OK`. Handle에 대한 첫 raw part 수신이나 handler 등록이 수신 모드를
고정시킨다 — 다른 모드를 활성화하거나 이 handler를 다시 등록하면 busy 결과와 `EBUSY`로
실패한다.

**선택 기준.** Application의 wire protocol이 Core가 각 client의 byte stream에서
조립하는 고정 frame — 2바이트 big-endian `header_size`, 4바이트 big-endian
`body_size`, 그 뒤 정확히 `header_size` 바이트의 header, 정확히 `body_size` 바이트의
body — 일 때 쓴다. Framing 없는 raw byte record에는 대신 `zlink_recv_part`를, 이 frame
조립 없는 raw push callback이 필요하면 `zlink_recv_handler`를 쓴다.

---

전체 근거는 [STREAM 스펙](../spec/core/socket/08-stream.ko.md)을 참고한다.
