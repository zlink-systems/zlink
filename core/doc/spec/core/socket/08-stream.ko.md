---
title: "소켓 — STREAM"
---

[English](08-stream.en.md) | 한국어

<!-- zlink-nav:start -->
[소켓 목차](README.ko.md) | [이전: ROUTER](07-router.ko.md)
<!-- zlink-nav:end -->

# 소켓 — STREAM

> **이 장이 정의하는 것** — STREAM 소켓의 raw TCP 연결 노출과
> [result/errno](../04-errno-map.ko.md) 공개 계약.

이 문서는 ZLink Core의 범용 raw STREAM 공개 계약을 정의합니다.
TCP/WS 연결의 byte record 또는 고정 framing packet을 routing ID로 송수신하는
C API와 bindings 개발자를 대상으로 합니다.

## 1. 범위

STREAM은 accept한 client 연결마다 4바이트 routing ID를 부여하는 bind 전용
raw socket입니다. `zlink_connect()`는 지원하지 않습니다. application은
routing ID로 client를 선택해 전송하고 수신 결과에서 source routing ID를
확인합니다.

STREAM은 application payload와 상위 protocol 의미를 해석하지 않습니다.

## 2. 생성, bind와 option

```c
ZLINK_EXPORT void *zlink_socket (void *context_, zlink_socket_type_t type_);
ZLINK_EXPORT zlink_bind_result_t zlink_bind (void *s_, const char *addr_);
ZLINK_EXPORT zlink_close_result_t zlink_close (void *s_);

typedef enum zlink_stream_option_t
{
    ZLINK_STREAM_OPT_NOTIFY = 0x3501
} zlink_stream_option_t;

ZLINK_EXPORT zlink_config_result_t zlink_set_stream_option (
  void *handle_, zlink_stream_option_t option_,
  const void *optval_, size_t optvallen_);
ZLINK_EXPORT zlink_config_result_t zlink_get_stream_option (
  void *handle_, zlink_stream_option_t option_,
  void *optval_, size_t *optvallen_);
```

`zlink_socket(context_, ZLINK_SOCKET_STREAM)`으로 생성합니다.
`ZLINK_STREAM_OPT_NOTIFY`는 bind 전에 설정하는 `int` 0 또는 1입니다. 값 1은
client 연결과 해제를 길이 0인 data record로 수신하게 하며 source routing ID가
대상 client를 식별합니다.

공통 HWM, timeout, linger, TLS와 buffer option은 `zlink_set_option()`과
`zlink_get_option()`을 사용합니다.

## 3. 수신 모드

한 STREAM handle은 다음 세 모드 가운데 하나만 사용합니다.

1. raw part receive: `zlink_recv_part()`로 raw record의 파트를 수신합니다.
2. raw callback: `zlink_recv_handler()`가 raw record를 callback에 전달합니다.
3. packet callback: `zlink_stream_packet_handler()`가 고정 framing packet을
   조립해 전달합니다.

첫 raw part 수신 또는 handler 등록이 수신 모드를 고정합니다. 같은 handle에서
다른 수신 모드를 활성화하거나 handler를 다시 등록하면 busy 결과와
`errno == EBUSY`로 실패합니다. data-plane `ZLINK_POLLIN`은 raw part receive
모드에 속합니다. send-ready handler와 `ZLINK_POLLOUT`은 수신 모드와
독립적입니다.

## 4. Routed part send

```c
ZLINK_EXPORT zlink_submit_result_t zlink_send_part_rid (
  void *s_,
  const zlink_routing_id_t *target_rid_,
  zlink_msg_t *part_,
  zlink_send_flags_t flags_,
  zlink_part_flag_t part_flag_);
```

STREAM은 한 번에 raw data part 하나를 target client로 전송합니다.
`target_rid_`는 STREAM이 해당 연결에 부여한 유효한 4바이트 routing ID여야
하며 `part_flag_`는 `ZLINK_PART_FINAL`이어야 합니다. `ZLINK_PART_MORE`는
`ZLINK_SUBMIT_NOT_SUPPORTED`, `errno == ENOTSUP`입니다.

STREAM send는 multipart sequence를 열지 않습니다. `ZLINK_PART_MORE` 실패 뒤에도
staging된 파트는 없고 다음 호출은 독립된 single-part record입니다. 따라서 다른
raw socket의 multipart 원자적 abort 규칙은 STREAM에 적용되지 않습니다.

성공하면 `part_`의 내용이 소비됩니다. backpressure로
`ZLINK_SUBMIT_BACKPRESSURED`, `errno == EAGAIN`을 반환하면 내용은 호출자에게
남으므로 같은 메시지로 재시도할 수 있습니다. 그 밖의 실패에서는 내용이
소비됩니다. 호출 전에 재사용 가능성이 있는 payload의 복사본을 준비하면 실패
종류와 무관하게 호출자 코드의 소유권 처리를 일정하게 유지할 수 있습니다.

연결을 찾을 수 없으면 `ZLINK_SUBMIT_NOT_CONNECTED`를 반환합니다. 전체 결과
대응은 [errno map](../04-errno-map.ko.md)을 따릅니다.

## 5. Raw part receive

```c
ZLINK_EXPORT zlink_recv_result_t zlink_recv_part (
  void *s_,
  const zlink_routing_id_t **source_rid_out_,
  zlink_msg_t *part_out_,
  zlink_part_flag_t *has_more_out_,
  zlink_recv_flags_t flags_);
```

`part_out_`은 초기화된 메시지여야 하며 `has_more_out_`과 함께 필수입니다.
`source_rid_out_`은 선택 사항입니다. 성공하면 source client의 routing ID를
가리키는 Core 소유 borrowed view를 받습니다. 이 view가 다음 raw receive 뒤에도
필요하면 그 전에 복사해야 합니다.

성공하면 수신 파트의 소유권이 호출자에게 이전되며 호출자는
`zlink_msg_close(part_out_)`를 정확히 한 번 호출해야 합니다. 파트를 받기 전에
실패하면 소유권이 이전되지 않습니다. `*has_more_out_`은 다음 파트가 있으면
`ZLINK_PART_MORE`, 마지막 파트이면 `ZLINK_PART_FINAL`입니다.
`ZLINK_DONTWAIT` 호출에 데이터가 없으면 `ZLINK_RECV_NO_DATA`를 반환합니다.

## 6. Raw callback

```c
ZLINK_EXPORT zlink_handler_result_t zlink_recv_handler (
  void *s_, zlink_socket_msg_handler_fn handler_, void *userdata_);
```

raw callback은 STREAM에서만 지원합니다. source routing ID는 callback 동안만
유효한 borrowed view입니다. callback으로 전달된 모든 message part의 소유권은
callback으로 이전되며 각 파트를 정확히 한 번 소비하거나 닫아야 합니다.
callback 안에서 같은 handle에 receive handler를 다시 등록하거나 close하면
busy 결과와 `errno == EBUSY`로 실패합니다.

## 7. Packet callback과 framing

```c
typedef void (*zlink_stream_packet_handler_fn) (
  void *stream_,
  const zlink_routing_id_t *source_rid_,
  zlink_msg_t *header_,
  zlink_msg_t *body_,
  void *userdata_);

ZLINK_EXPORT zlink_handler_result_t zlink_stream_packet_handler (
  void *stream_, zlink_stream_packet_handler_fn handler_, void *userdata_);
```

packet mode는 각 client byte stream에서 다음 frame을 순서대로 조립합니다.

```text
+----------------+----------------+----------------+---------------+
| header_size:u16| body_size:u32  | header bytes   | body bytes    |
+----------------+----------------+----------------+---------------+
| big endian     | big endian     | exact length   | exact length  |
+----------------+----------------+----------------+---------------+
```

`header_size`는 unsigned 16-bit, `body_size`는 unsigned 32-bit 길이입니다.
두 payload 길이는 0일 수 있으며 callback은 이 경우에도 `NULL`이 아니라 길이
0인 유효한 `zlink_msg_t`를 받습니다. source routing ID는 callback 동안만
유효한 borrowed view입니다. `header_`와 `body_`의 소유권은 callback으로
이전되므로 각각 정확히 한 번 소비하거나 닫아야 합니다.

## 8. Send readiness와 thread safety

```c
ZLINK_EXPORT zlink_handler_result_t zlink_send_ready_handler (
  void *s_, zlink_send_ready_handler_fn handler_, void *userdata_);
```

send-ready는 이전 submit이 backpressure였을 때 다시 시도할 가치가 있음을
알리지만 다음 submit의 성공을 보장하지 않습니다. handler는 교체할 수 있지만
`NULL`로 제거할 수 없습니다. 같은 send-ready callback 안에서 재등록하면
`ZLINK_HANDLER_DEADLOCK`, `errno == EDEADLK`입니다.

공개 socket handle의 thread-safety와 close 계약은 [소켓 공통](README.ko.md)을
따릅니다. 같은 `zlink_msg_t`를 여러 스레드가 동시에 사용할 수 없습니다.
