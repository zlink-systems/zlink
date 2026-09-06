
[레퍼런스 목차](README.ko.md)

# 13. STREAM

Bind 전용 raw socket으로, 받아들이는 모든 client 연결에 4바이트 routing ID를 부여하고
routed TCP나 WebSocket으로 byte record나 고정 framing packet을 주고받는다. STREAM은
`zlink_connect`를 지원하지 않으며 application payload를 해석하지 않는다.
`zlink_socket`/`zlink_bind`/`zlink_close`(Socket lifecycle category), RAW
`zlink_recv_part`(Raw receive category — STREAM은 `source_rid_out_`가 `NULL` 대신 실제
view를 반환하는 유일한 타입이다), Socket lifecycle category의 completion 기반 send를
재사용한다 — 이 category는 STREAM 고유의 옵션, 자신만의 directed send 버전, 그리고
STREAM에만 있는 packet-framing 수신 모드를 다룬다. 정확한
signature는 [STREAM 스펙](../spec/core/socket/08-stream.ko.md)이 소유한다.

---

## `zlink_set_stream_option` / `zlink_get_stream_option`

STREAM의 타입 전용 receive option을 설정하거나 읽는다.

```c
zlink_stream_recv_mode_t mode = ZLINK_STREAM_RECV_MODE_PACKET;
zlink_set_stream_option(stream_socket, ZLINK_STREAM_OPT_RECV_MODE, &mode, sizeof(mode));
```

**Parameters.** `ZLINK_STREAM_OPT_RECV_MODE`은 RAW 또는 PACKET을 받으며 첫 bind/connect
성공 전에 설정한다. `ZLINK_STREAM_OPT_NOTIFY`는 `int` 0 또는 1을 받는다. 값 `1`은 client
연결·해제를 zero-length RAW record로 드러내며 PACKET과 함께 지원하지 않는다.

**Return과 errno.** 둘 다 `zlink_config_result_t`를 반환한다 — 성공하면
`ZLINK_CONFIG_OK`. 공통 HWM·timeout·linger·TLS·buffer 옵션은
`zlink_set_option`/`zlink_get_option`(Socket options and identity category)을 쓴다.

**선택 기준.** Framing 없는 byte와 선택적인 connection notification에는 RAW를 고른다.
`zlink_stream_recv_packet()`이 소비하는 고정 header/body framing에는 PACKET을 고른다.

---

## `zlink_send_part_rid`(STREAM)

Routing ID로 지정한 특정 연결된 client에게 raw data part 하나를 보낸다 — 같은 함수 이름을
쓰는 ROUTER(ROUTER category)와는 모양이 다른, STREAM의 directed send다.

```c
zlink_send_part_rid(stream_socket, &client_rid, &part, ZLINK_SEND_FLAGS_NONE,
                    ZLINK_PART_FINAL, NULL, NULL);
```

**Parameters.** `target_rid_`는 이 STREAM socket이 부여한 유효한 4바이트 routing
ID여야 하며 `part_flag_`는 `ZLINK_PART_FINAL`이다. STREAM은 단일 part를 전송한다.

**Return과 errno.** `zlink_submit_result_t`를 반환한다 — 성공하면
`ZLINK_SUBMIT_OK`. 연결이 없으면 `ZLINK_SUBMIT_NOT_CONNECTED`. 공통 인자 검증을 통과한
`ZLINK_PART_MORE` 호출은
`ZLINK_SUBMIT_NOT_SUPPORTED`+`ENOTSUP`, ID `0`이며 입력을 전송하지 않는다. 모든 결과에서
`part_`를 소비하므로 application-level 복구가 필요하면 제출 전에 payload의 별도 복사본을 보관한다.

**선택 기준.** NONE FINAL은 `SNDTIMEO` 안에서 같은 RID의 local admission을 기다린다.
DONTWAIT는 backpressure이면 payload를 보관하지 않고 0이 아닌 WRITABLE 대기 토큰을
반환한다. Caller는 completion을 받은 뒤 자신이 보관한 payload를 다시 제출한다.

---

## `zlink_stream_recv_packet`

PACKET mode에서 조립된 header/body packet 하나를 받는다.

```c
zlink_stream_recv_packet(stream_socket, &source_rid, &header, &body,
                         ZLINK_RECV_FLAGS_NONE);
```

**Parameters.** `source_rid_out_`는 socket-owned client RID view를 받는다. `header_out_`와
`body_out_`는 초기화된 message이며 성공하면 채워진다. 길이 0에서도 각각 유효한 message다.
`flags_`는 blocking NONE 또는 DONTWAIT receive를 고른다.

**Return과 errno.** `zlink_recv_result_t`를 반환한다. DONTWAIT에서 packet이 없으면
`ZLINK_RECV_NO_DATA`와 `EAGAIN`, RAW mode이면 `ZLINK_RECV_NOT_SUPPORTED`와 `ENOTSUP`이다.
성공하면 caller가 두 message를 각각 정확히 한 번 close하거나 move한다.

**선택 기준.** Application의 wire protocol이 Core가 각 client의 byte stream에서
조립하는 고정 frame — 2바이트 big-endian `header_size`, 4바이트 big-endian
`body_size`, 그 뒤 정확히 `header_size` 바이트의 header, 정확히 `body_size` 바이트의
body — 일 때 쓴다. 이 framing 없는 raw byte record에는 `zlink_recv_part`를 쓴다.

---

전체 근거는 [STREAM 스펙](../spec/core/socket/08-stream.ko.md)을 참고한다.
