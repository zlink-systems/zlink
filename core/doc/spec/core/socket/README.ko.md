---
title: "소켓 — 공통 명세"
---

[English](https://zlink-systems.github.io/zlink/spec/core/socket/) | 한국어

<!-- zlink-nav:start -->
[Core 스펙 목차](../README.ko.md) | [이전: Runtime Boundary](../09-runtime-boundary.ko.md) | [다음: PAIR](01-pair.ko.md)
<!-- zlink-nav:end -->

# 소켓 -- 공통 명세

> **이 장이 정의하는 것** — 모든 소켓 타입에 적용되는 공통 기반(옵션·API 형태).
> 타입별 세부사항은 각 소켓 명세가 정의한다.

이 문서는 모든 소켓 타입에 적용되는 공통 기반을 다룹니다.
타입별 명세(타입 전용 옵션, data plane API, 동작 세부사항)는
별도 파일에 있습니다.

| 소켓 타입 | 명세 |
|-----------|------|
| PAIR | [01-pair.ko.md](01-pair.ko.md) |
| DEALER | [06-dealer.ko.md](06-dealer.ko.md) |
| ROUTER | [07-router.ko.md](07-router.ko.md) |
| PUB | [02-pub.ko.md](02-pub.ko.md) |
| SUB | [03-sub.ko.md](03-sub.ko.md) |
| XPUB | [04-xpub.ko.md](04-xpub.ko.md) |
| XSUB | [05-xsub.ko.md](05-xsub.ko.md) |
| STREAM | [08-stream.ko.md](08-stream.ko.md) |

## 스레드 안전성 요약

공개 socket handle API는 기본적으로 thread-safe합니다. 다만 모든 API가 같은
비용 모델을 갖는 것은 아닙니다.

- `send`는 여러 스레드에서 동시 호출을 허용하는 hot path입니다.
- `bind/connect/disconnect`, subscribe/unsubscribe, option/query, monitor는
  runtime에 호출 가능한 control path입니다. correctness는 보장되지만 실행
  순서는 내부 직렬화에 따라 결정될 수 있습니다.
- `close`는 fail-fast lifecycle gate를 사용합니다. 다른 스레드가 같은 handle에서
  admitted API나 callback을 실행 중이면 `EBUSY`, close가 accepted된 뒤 새 API
  진입은 `ESHUTDOWN`입니다.
- 예외는 소수만 남깁니다. init-only 설정, callback context에서 금지된 일부
  reentrant API, 같은 `zlink_msg_t` 인스턴스의 동시 공유는 기본 허용 범위
  밖입니다.

## 수신 모델 요약

소켓 타입별 수신 모델은 아래와 같이 고정합니다. 기본 모델은
`recv + poller`이며, 예외 타입만 콜백 기반 수신을 지원합니다.

| 소켓 타입 | 수신 표면 | 비고 |
|-----------|-----------|------|
| PAIR | `zlink_recv_part()` | part receive 전용 |
| DEALER | `zlink_recv_part()` (+ `zlink_dealer_request_part()` completion callback) | part receive data plane |
| SUB | `zlink_subscribe_part()` | topic part receive 전용 |
| XSUB | `zlink_subscribe_part()` | topic part receive 전용 |
| ROUTER | `zlink_router_recv_part()` (+ `zlink_router_request_part()` completion callback) | part receive data plane |
| STREAM | `zlink_recv_part()` / `zlink_recv_handler()` / `zlink_stream_packet_handler()` | 세 모드 중 하나 선택 (예외) |
| PUB | 해당 없음 | 송신 전용 |
| XPUB | `zlink_xpub_recv_part()` (구독 이벤트 recv-only) | 데이터 plane은 송신 |
| monitor / timer | recv / callback 모두 지원 | 관찰/유틸 계층 |

핵심 원칙:

- raw data-plane 수신은 recv + poller 조합이 기본이며, 서버 루프는
  `ZLINK_POLLIN`을 관찰한 뒤 recv 계열 함수로 데이터를 가져오는 방식을 씁니다.
- `DEALER`/`ROUTER`의 request completion callback은 data-plane receive가
  아니라 비동기 작업 완료 통지입니다. 이 둘은 역할이 다르므로 같은 범주로
  묶지 않습니다.
- STREAM만은 예외입니다. raw transport 특성상 세 가지 수신 모드(raw recv,
  raw callback, packet callback) 중 하나를 선택할 수 있습니다. 한 handle
  에서 두 번째 모드로 전환하려 하면 `EBUSY`로 실패합니다.

## 콜백 타입

### zlink_socket_msg_handler_fn

```c
typedef void (*zlink_socket_msg_handler_fn) (
  const zlink_routing_id_t *source_rid_,
  zlink_msg_t *parts_,
  size_t part_count_,
  void *userdata_);
```

raw `STREAM`의 raw 수신 콜백에 사용되는 타입입니다. 소유 I/O 스레드에서
호출되며, 모든 메시지 파트의 소유권이 콜백으로 이전됩니다. 각 파트는
정확히 한 번 닫거나 소비해야 합니다. `zlink_recv_handler()`와 함께
사용합니다.

### zlink_stream_packet_handler_fn

```c
typedef void (*zlink_stream_packet_handler_fn) (
  void *stream_,
  const zlink_routing_id_t *source_rid_,
  zlink_msg_t *header_,
  zlink_msg_t *body_,
  void *userdata_);
```

raw `STREAM`의 packet 단위 수신 콜백 타입입니다. `source_rid_`는 packet을
보낸 client 연결의 routing id를 가리키는 borrowed view이고, `header_`와
`body_`는 고정 framing 규약에 따라 조립된 packet의 header/body payload
입니다. 길이가 0인 경우에도 NULL이 아닌 유효한 `zlink_msg_t`로 전달되며,
두 `msg_t`의 소유권은 콜백으로 이전됩니다. `zlink_stream_packet_handler()`
와 함께 사용합니다.

### 송신 완료 타입

```c
typedef enum zlink_send_complete_result_t {
  ZLINK_SEND_ADMITTED = 0,
  ZLINK_SEND_TIMED_OUT = 201,
  ZLINK_SEND_TERMINAL = 202
} zlink_send_complete_result_t;

typedef uint64_t zlink_send_op_id_t;

typedef struct zlink_send_complete_event_t {
  zlink_send_op_id_t op_id;
  void *userdata;
  zlink_routing_id_t peer_rid;
  uint64_t transport_pair_id;
  uint64_t transport_pair_generation;
  zlink_send_complete_result_t result;
  int terminal_errno;
} zlink_send_complete_event_t;

typedef void (*zlink_send_complete_handler_fn) (
  void *subject_, const zlink_send_complete_event_t *event_,
  void *userdata_);

typedef struct zlink_send_async_options_t {
  uint32_t struct_size;
  uint32_t timeout_ms;
  void *userdata;
  const zlink_routed_submit_target_t *target;
} zlink_send_async_options_t;
```

`ZLINK_SEND_ADMITTED`는 레코드가 Core 송신 큐에 admit됐다는 뜻입니다. peer가
받았다는 뜻이 아니므로 전달 확인이 필요하면 request/reply를 사용합니다.
`ZLINK_SEND_TIMED_OUT`은 operation별 `timeout_ms` 만료입니다.
`ZLINK_SEND_TERMINAL`은 최종 실패이며 사유는 `terminal_errno`에 담깁니다.
취소와 socket close는 `ECANCELED`, context 종료는 `ETERM`, 그 밖에는 route
실패 errno입니다.

`op_id`는 Core가 부여하는 socket 로컬 단조 증가 값이고 `0`은 유효한 id가 아니며
submit 실패 시 out 파라미터에 남는 값입니다. `userdata`는 submit option에 넘긴
값을 그대로 돌려줍니다. target identity 필드는 항상 채워지며 routed target이
없는 socket에서는 0입니다.

### zlink_reply_handler_fn

```c
typedef void (*zlink_reply_handler_fn) (
  zlink_request_result_t result_,
  zlink_msg_t *parts_,
  size_t part_count_,
  void *userdata_);
```

비동기 request-reply 완료 콜백. 응답이 도착하거나 요청이 타임아웃되면
호출됩니다. 타임아웃 시 `result_`는 `ZLINK_REQUEST_TIMED_OUT`이고 `parts_`는
NULL입니다. 성공 시 `result_`는 `ZLINK_REQUEST_OK`이고 모든 메시지 파트의
소유권이 콜백으로 이전됩니다. `result_`는 submit 실패가 아니라
`zlink_request_result_t` 값으로 request completion 결과를 나타냅니다. 이
콜백은 data-plane receive가 아니라 async operation completion 통지 축이며,
`DEALER`/`ROUTER`의 request API에서만 사용됩니다.

Socket 하나에서 callback이 끝나지 않은 request는 최대 65,536건입니다. Core는
request를 전송하기 전에 completion slot을 예약합니다. Slot이 없으면 submit 결과는
`ZLINK_SUBMIT_BACKPRESSURED`이고 `errno`는 `EAGAIN`입니다. Reply, timeout과
disconnect completion은 같은 예약을 사용하므로 owner thread가 callback 처리를
중단해도 control queue가 이 상한을 넘어 증가하지 않습니다.

## 상수

### 소켓 타입

```c
typedef enum zlink_socket_type_t
{
    ZLINK_SOCKET_ANY    = 0,
    ZLINK_SOCKET_PAIR   = 0x1001,
    ZLINK_SOCKET_PUB    = 0x1002,
    ZLINK_SOCKET_SUB    = 0x1003,
    ZLINK_SOCKET_DEALER = 0x1004,
    ZLINK_SOCKET_ROUTER = 0x1005,
    ZLINK_SOCKET_XPUB   = 0x1006,
    ZLINK_SOCKET_XSUB   = 0x1007,
    ZLINK_SOCKET_STREAM = 0x1008
} zlink_socket_type_t;
```

`ZLINK_SOCKET_ANY`는 생성할 socket type이 아니다. filter API에서 전체 socket type을
뜻하는 wildcard로만 사용한다. 실제 socket 생성에는 위에 표시된 정규화된
`ZLINK_SOCKET_*` 상수를 사용한다.

### 송신 플래그

```c
typedef enum zlink_send_flags_t
{
    ZLINK_SEND_FLAGS_NONE     = 0,
    ZLINK_SEND_FLAGS_DONTWAIT = 0x0001u
} zlink_send_flags_t;
```

`ZLINK_DONTWAIT` 는 `ZLINK_SEND_FLAGS_DONTWAIT` 를 짧게 쓰는 공개 이름이다.

| 상수 | 설명 |
|---|---|
| `ZLINK_SEND_FLAGS_NONE` | 플래그 없음; 블로킹 송신 동작. |
| `ZLINK_SEND_FLAGS_DONTWAIT` | 논블로킹 모드; 블로킹 시 `ZLINK_SUBMIT_BACKPRESSURED` 반환 |
| `ZLINK_DONTWAIT` | `ZLINK_SEND_FLAGS_DONTWAIT` 를 짧게 쓰는 이름 |

### 수신 플래그

```c
typedef enum zlink_recv_flags_t
{
    ZLINK_RECV_FLAGS_NONE     = 0,
    ZLINK_RECV_FLAGS_DONTWAIT = 0x0001u
} zlink_recv_flags_t;
```

`zlink_recv_part`, `zlink_subscribe_part`, 소켓별 `zlink_*_recv_part` 계열, 그리고
monitor `zlink_*_monitor_recv` 함수들이 이 플래그를 사용합니다.

| 상수 | 설명 |
|---|---|
| `ZLINK_RECV_FLAGS_NONE` | 플래그 없음; 블로킹 수신 동작. |
| `ZLINK_RECV_FLAGS_DONTWAIT` | 논블로킹 수신; 수신할 메시지가 없으면 `ZLINK_RECV_NO_DATA` 를 즉시 반환. |

### rid 중복 정책

```c
typedef enum zlink_rid_duplicate_policy_t
{
    ZLINK_RID_DUPLICATE_REJECT = 0,
    ZLINK_RID_DUPLICATE_HANDOVER = 1
} zlink_rid_duplicate_policy_t;
```

`ZLINK_OPT_RID_DUPLICATE_POLICY`는 같은 local socket에 동일한 peer
routing id가 들어왔을 때의 정책을 정합니다. 값은 `int`로 설정하며,
기본값은 `ZLINK_RID_DUPLICATE_REJECT`입니다.

| 값 | 의미 |
|---|---|
| `ZLINK_RID_DUPLICATE_REJECT` | 기존 pipe를 유지하고 새 중복 pipe를 등록하지 않음 |
| `ZLINK_RID_DUPLICATE_HANDOVER` | 같은 방향에서 다시 연결한 pipe는 기존 pipe를 인수한다. 서로 반대 방향의 pipe가 충돌하면 두 피어의 routing id를 비교해 양쪽이 같은 방향 하나를 선택한다. |

이 옵션은 peer가 광고한 routing id를 관찰할 수 있는 socket에서만 의미가
있습니다. STREAM은 서버가 연결별 4바이트 routing id를 직접 만들기 때문에
이 옵션의 영향을 받지 않습니다.

### 송신 재시도 모드

```c
typedef enum zlink_submit_retry_mode_t
{
    ZLINK_SUBMIT_RETRY_OFF = 0,
    ZLINK_SUBMIT_RETRY_LOCAL_FAILURE = 1
} zlink_submit_retry_mode_t;
```

`ZLINK_SUBMIT_RETRY_OFF`는 자동 재시도를 하지 않습니다.
`ZLINK_SUBMIT_RETRY_LOCAL_FAILURE`는 송신을 peer queue에 넘기기 전에 발생한
로컬 실패만 재시도할 수 있음을 나타냅니다. 이 모드는 peer 전달이나 처리
완료를 보장하지 않습니다.

### 메시지 파트 플래그

```c
typedef enum zlink_part_flag_t
{
    ZLINK_PART_FINAL = 0,
    ZLINK_PART_MORE = 1
} zlink_part_flag_t;
```

`ZLINK_PART_MORE`는 같은 멀티파트 메시지에 다음 파트가 있음을 나타내고,
`ZLINK_PART_FINAL`은 현재 파트가 마지막임을 나타냅니다.

### 수신 flow state

```c
typedef enum zlink_receive_flow_state_t
{
    ZLINK_RECEIVE_FLOW_RUNNING = 0,
    ZLINK_RECEIVE_FLOW_PAUSED = 1
} zlink_receive_flow_state_t;
```

DEALER와 ROUTER socket이 paired completion lane으로 자신에게 보내는 peer에게
알리는 receive-flow 상태입니다. `ZLINK_RECEIVE_FLOW_RUNNING`은 계속 보내라는
뜻이고 `ZLINK_RECEIVE_FLOW_PAUSED`는 이 socket으로 새 message를 보내지 말라는
뜻입니다. 이 값은 counter가 아니라 socket 전체에 적용되는 절대 상태이므로, 이미
유지하는 상태를 다시 설정하면 아무것도 바꾸지 않고 성공합니다. 이 lane은 DEALER와
ROUTER에만 있으며 결과 동작은 [DEALER](06-dealer.ko.md)와
[ROUTER](07-router.ko.md)가 소유합니다.

### 송신 결과

```c
typedef enum zlink_submit_result_t
{
    /* Submit succeeded. */
    ZLINK_SUBMIT_OK = 0,

    /* Normal control-flow result. */
    ZLINK_SUBMIT_BACKPRESSURED = 1,
    ZLINK_SUBMIT_NOT_CONNECTED = 2,
    ZLINK_SUBMIT_NOT_FOUND = 3,
    ZLINK_SUBMIT_NOT_ADMITTED = 13,

    /* Runtime / lifecycle failure. */
    ZLINK_SUBMIT_TERMINATED = 4,

    /* Caller contract violation. */
    ZLINK_SUBMIT_INVALID_HANDLE = 5,
    ZLINK_SUBMIT_INVALID_ARGUMENT = 6,
    ZLINK_SUBMIT_NOT_SUPPORTED = 7,
    ZLINK_SUBMIT_INVALID_STATE = 8,
    ZLINK_SUBMIT_THREAD_VIOLATION = 9,

    /* Internal failure. */
    ZLINK_SUBMIT_OUT_OF_MEMORY = 10,
    ZLINK_SUBMIT_SEQ_EXHAUSTED = 11,
    ZLINK_SUBMIT_INTERNAL_ERROR = 12
} zlink_submit_result_t;
```

send, request submit, reply submit API의 공개 결과를 정규화할 때
사용하는 기준 enum입니다. exported C API는 이 enum을 직접 반환합니다.
내부 구현 경로는 계속 상세 `errno`를 사용하고, exported API 경계에서 그
값을 이 공개 결과 계약으로 정규화합니다.

| 상수 | 값 | 설명 |
|---|---|---|
| `ZLINK_SUBMIT_OK` | 0 | 메시지가 성공적으로 송신됨 |
| `ZLINK_SUBMIT_BACKPRESSURED` | 1 | 송신 큐가 가득 참 (HWM 도달) |
| `ZLINK_SUBMIT_NOT_CONNECTED` | 2 | 대상 경로나 peer가 아직 연결되지 않음 |
| `ZLINK_SUBMIT_NOT_FOUND` | 3 | 대상 peer 또는 routed destination을 찾지 못함 |
| `ZLINK_SUBMIT_NOT_ADMITTED` | 13 | Normal control-flow 결과. target route는 식별했지만 handshake 또는 신규 outbound weight 같은 admission 정책이 submit을 거부함 |
| `ZLINK_SUBMIT_TERMINATED` | 4 | context가 종료됨 |
| `ZLINK_SUBMIT_INVALID_HANDLE` | 5 | 핸들이 NULL이거나 유효하지 않음 |
| `ZLINK_SUBMIT_INVALID_ARGUMENT` | 6 | API 계약에 맞지 않는 인자 |
| `ZLINK_SUBMIT_NOT_SUPPORTED` | 7 | 지원하지 않는 작업 또는 flags |
| `ZLINK_SUBMIT_INVALID_STATE` | 8 | 핸들이 잘못된 상태에 있음 |
| `ZLINK_SUBMIT_THREAD_VIOLATION` | 9 | 허용된 스레드 모델을 위반함 |
| `ZLINK_SUBMIT_OUT_OF_MEMORY` | 10 | submit 준비 중 메모리 할당 실패 |
| `ZLINK_SUBMIT_SEQ_EXHAUSTED` | 11 | request sequence 공간이 소진됨 |
| `ZLINK_SUBMIT_INTERNAL_ERROR` | 12 | 내부 send/request/reply submit 오류 |

### Request Completion

```c
typedef enum zlink_request_result_t
{
    /* Reply completed successfully. */
    ZLINK_REQUEST_OK = 0,

    /* Completion failure visible to the requester. */
    ZLINK_REQUEST_TIMED_OUT       = 101,
    ZLINK_REQUEST_NOT_FOUND       = 102,
    ZLINK_REQUEST_TERMINATED      = 103,
    ZLINK_REQUEST_PROTOCOL_ERROR  = 104,
    ZLINK_REQUEST_INTERNAL_ERROR  = 105,
    ZLINK_REQUEST_REJECTED        = 106,
    ZLINK_REQUEST_CONFLICT        = 107,
    ZLINK_REQUEST_BUSY            = 108,
    ZLINK_REQUEST_NOT_CONNECTED   = 109,
    ZLINK_REQUEST_INVALID_ARGUMENT = 110,
    ZLINK_REQUEST_INVALID_STATE   = 111,
    ZLINK_REQUEST_NOT_SUPPORTED   = 112,
    ZLINK_REQUEST_BACKPRESSURED   = 113
} zlink_request_result_t;
```

`zlink_reply_handler_fn`의 completion 결과를 정규화할 때 사용하는 기준
enum입니다. callback은 `result_`를 `zlink_request_result_t` 값으로
직접 전달합니다.

| 상수 | 값 | 설명 |
|---|---|---|
| `ZLINK_REQUEST_OK` | 0 | reply payload를 정상 수신함 |
| `ZLINK_REQUEST_TIMED_OUT` | 101 | 설정된 시간 안에 reply가 도착하지 않음 |
| `ZLINK_REQUEST_NOT_FOUND` | 102 | 대상이 없어 error reply로 완료됨 |
| `ZLINK_REQUEST_TERMINATED` | 103 | terminal reply 전에 Context 또는 socket이 종료됨 (`ETERM` 또는 `ESHUTDOWN`) |
| `ZLINK_REQUEST_PROTOCOL_ERROR` | 104 | reply envelope 또는 error reply payload가 잘못됨 |
| `ZLINK_REQUEST_INTERNAL_ERROR` | 105 | 더 세분화된 public bucket 없이 request completion이 실패함 |
| `ZLINK_REQUEST_REJECTED` | 106 | target이 request를 명시적으로 거부함 |
| `ZLINK_REQUEST_CONFLICT` | 107 | request가 현재 routing 또는 operation 상태와 충돌함 |
| `ZLINK_REQUEST_BUSY` | 108 | target이 바빠 지금은 request를 받을 수 없음 |
| `ZLINK_REQUEST_NOT_CONNECTED` | 109 | target에 대한 활성 연결 없음 |
| `ZLINK_REQUEST_INVALID_ARGUMENT` | 110 | request에 잘못된 인자가 담김 |
| `ZLINK_REQUEST_INVALID_STATE` | 111 | target이 이 request를 거부하는 상태임 |
| `ZLINK_REQUEST_NOT_SUPPORTED` | 112 | target이 지원하지 않는 작업 |
| `ZLINK_REQUEST_BACKPRESSURED` | 113 | non-blocking outbound admission이 capacity 부족으로 실패함 |

### 보안 메커니즘

| 상수 | 값 | 설명 |
|---|---|---|
| `ZLINK_NULL` | 0 | 보안 메커니즘 없음 (기본값) |
| `ZLINK_PLAIN` | 1 | PLAIN 사용자명/비밀번호 인증 |

### 소켓 옵션

소켓 옵션은 타입별 전용 enum과 함수를 사용합니다. 공통 옵션은
`zlink_set_option()` / `zlink_get_option()`으로, 소켓 타입별 옵션은
`zlink_set_router_option()`, `zlink_set_dealer_option()`,
`zlink_set_pub_option()`, `zlink_set_sub_option()`,
`zlink_set_stream_option()` 등 전용 함수로 설정합니다.
ROUTING_ID는 `zlink_set_routing_id()` / `zlink_get_routing_id()` 전용
함수를 사용한다. TLS server/client role의 표준 설정은 `zlink_set_tls_server()` /
`zlink_set_tls_client()`를 사용하고, `ZLINK_OPT_TLS_*`는 지원하는 raw network socket의 개별 TLS 값을
설정하거나 조회할 때만 사용한다. SUBSCRIBE/UNSUBSCRIBE는 `zlink_set_subscription()` /
`zlink_unset_subscription()` 전용 함수를 사용합니다.

#### 공통 옵션 (`zlink_option_t`)

```c
typedef enum zlink_option_t {
  ZLINK_OPT_AFFINITY                  = 0x3001,
  ZLINK_OPT_RATE                      = 0x3003,
  ZLINK_OPT_RECOVERY_IVL              = 0x3004,
  ZLINK_OPT_SNDBUF                    = 0x3005,
  ZLINK_OPT_RCVBUF                    = 0x3006,
  ZLINK_OPT_FD                        = 0x3007,
  ZLINK_OPT_EVENTS                    = 0x3008,
  ZLINK_OPT_TYPE                      = 0x3009,
  ZLINK_OPT_LINGER                    = 0x300A,
  ZLINK_OPT_RECONNECT_IVL             = 0x300B,
  ZLINK_OPT_BACKLOG                   = 0x300C,
  ZLINK_OPT_RECONNECT_IVL_MAX         = 0x300D,
  ZLINK_OPT_MAXMSGSIZE                = 0x300E,
  ZLINK_OPT_SNDHWM                    = 0x300F,
  ZLINK_OPT_RCVHWM                    = 0x3010,
  ZLINK_OPT_MULTICAST_HOPS            = 0x3011,
  ZLINK_OPT_RCVTIMEO                  = 0x3012,
  ZLINK_OPT_SNDTIMEO                  = 0x3013,
  ZLINK_OPT_LAST_ENDPOINT             = 0x3014,
  ZLINK_OPT_TCP_KEEPALIVE             = 0x3015,
  ZLINK_OPT_TCP_KEEPALIVE_CNT         = 0x3016,
  ZLINK_OPT_TCP_KEEPALIVE_IDLE        = 0x3017,
  ZLINK_OPT_TCP_KEEPALIVE_INTVL       = 0x3018,
  ZLINK_OPT_IMMEDIATE                 = 0x3019,
  ZLINK_OPT_IPV6                      = 0x301A,
  ZLINK_OPT_CONFLATE                  = 0x301B,
  ZLINK_OPT_TOS                       = 0x301C,
  ZLINK_OPT_HANDSHAKE_IVL             = 0x301D,
  ZLINK_OPT_BLOCKY                    = 0x301E,
  ZLINK_OPT_INVERT_MATCHING           = 0x3020,
  ZLINK_OPT_CONNECT_TIMEOUT           = 0x3024,
  ZLINK_OPT_TCP_MAXRT                 = 0x3025,
  ZLINK_OPT_MULTICAST_MAXTPDU         = 0x3026,
  ZLINK_OPT_BINDTODEVICE              = 0x3027,
  ZLINK_OPT_TLS_CERT                   = 0x3028,
  ZLINK_OPT_TLS_KEY                    = 0x3029,
  ZLINK_OPT_TLS_CA                     = 0x302A,
  ZLINK_OPT_TLS_VERIFY                 = 0x302B,
  ZLINK_OPT_TLS_REQUIRE_CLIENT_CERT    = 0x302C,
  ZLINK_OPT_TLS_HOSTNAME               = 0x302D,
  ZLINK_OPT_TLS_TRUST_SYSTEM           = 0x302E,
  ZLINK_OPT_TLS_PASSWORD               = 0x302F,
  ZLINK_OPT_ZMP_METADATA               = 0x3030,
  ZLINK_OPT_TCP_NODELAY                = 0x3031,
  ZLINK_OPT_ROUTE_VALUE_MAX_SIZE       = 0x3032,
  ZLINK_OPT_RID_DUPLICATE_POLICY       = 0x3033,
  ZLINK_OPT_SUBMIT_RETRY_MODE          = 0x3037,
  ZLINK_OPT_SUBMIT_RETRY_TIMEOUT       = 0x3038,
  ZLINK_OPT_SUBMIT_RETRY_ATTEMPTS      = 0x3039
} zlink_option_t;
```

`zlink_set_option()` / `zlink_get_option()`과 함께 사용합니다.
raw socket과 discovery에 적용된다.

##### Transport/Buffer

| 상수 | 설명 |
|---|---|
| `ZLINK_OPT_AFFINITY` | I/O 스레드 어피니티 비트마스크 (`uint64_t`) |
| `ZLINK_OPT_RATE` | 멀티캐스트 전송률 (kbps, `int`) |
| `ZLINK_OPT_RECOVERY_IVL` | 멀티캐스트 복구 간격 (ms, `int`) |
| `ZLINK_OPT_SNDBUF` | 커널 송신 버퍼 크기 (`int`; -1=OS 기본값 유지, 0 이상=OS에 크기 요청) |
| `ZLINK_OPT_RCVBUF` | 커널 수신 버퍼 크기 (`int`; -1=OS 기본값 유지, 0 이상=OS에 크기 요청) |
| `ZLINK_OPT_SNDHWM` | Directional send pipe의 accounted byte HWM (`uint64_t`; 기본값 `4,096,000`, `0`=무제한) |
| `ZLINK_OPT_RCVHWM` | Directional receive pipe의 accounted byte HWM (`uint64_t`; 기본값 `4,096,000`, `0`=무제한) |
| `ZLINK_OPT_MAXMSGSIZE` | 최대 인바운드 메시지 크기 (`int64_t`; -1=무제한) |

두 HWM `uint64_t` option은 `zlink_set_option()`과 `zlink_get_option()`에서 정확히
`sizeof(uint64_t)` byte를 사용해야 합니다. 4-byte 값은
`ZLINK_CONFIG_INVALID_ARGUMENT`로 거절합니다. 제거된 socket option 값 `0x3034`도
알 수 없는 option이므로 `ZLINK_CONFIG_INVALID_ARGUMENT`와 `EINVAL`로 실패합니다.
Pipe admission은 실제로 보관한 byte를 계산합니다.

HWM은 각 HWM-controlled application directional pipe에 적용합니다. DEALER·ROUTER의
completion progress lane은 terminal reply와 error reply 전용이며 auto HWM, manual
`SNDHWM`·`RCVHWM`, LWM과 Core budget reservation을 적용하지 않습니다. Accounted byte가 limit에 도달하면 receiver가
충분한 byte credit을 반환할 때까지 이후 write가 대기합니다. 비어 있는 pipe에는
accounted 크기가 HWM보다 큰 message 한 건을 허용할 수 있습니다. 따라서 유효한 큰
message를 HWM이 작다는 이유만으로 모두 거절하지 않습니다. 이 message도
`ZLINK_OPT_MAXMSGSIZE`를 만족해야 하며, 한 건을 허용한 뒤에는 이후 write가 대기합니다.
`ZLINK_OPT_MAXMSGSIZE`가 무제한인 방향에서도 admission 시점에 전체 accounted 크기를 아는
complete message 한 건, 즉 single-part 또는 total-known message에만 이 예외를 적용합니다.
최종 전체 크기를 모르는 incremental multipart에는 첫 `MORE` frame부터 일반 byte HWM을 적용하므로
frame이 제한 없이 누적되지 않습니다. 이 예외를 위해 known-total metadata나 transaction 전체
reservation을 추가하지 않습니다.

Admission은 frame 단위로 charge합니다. 일반 frame의 charge는 payload byte 수에
`sizeof(zlink_msg_t)`를 더한 값이므로 빈 frame도 비용이 0이 아니고, 작은 frame을 많이
보관한 pipe는 payload 합계보다 먼저 HWM에 도달합니다. Delimiter, join과 leave frame은
application payload가 없으므로 `sizeof(zlink_msg_t)` metadata 비용만 charge합니다.
Frame이 pipe에서 빠질 때 같은 charge를 되돌려 줍니다.

Low water mark는 pipe가 대기 중인 writer에게 read credit을 돌려주는 byte 수준입니다.
기본값은 해당 방향에 적용된 HWM의 `ceil(hwm_bytes / 2)`입니다. Pipe는 low water mark
hint를 가질 수도 있습니다. Hint는 그 기본값보다 낮을 때만 사용하며, 기본값 이상인 hint는
기본값을 그대로 둡니다. HWM 이상인 hint는 `hwm_bytes - 1`로 clamp하고, clamp한 값이 `1`
미만이면 `1`로 만들므로 결과는 항상 `1 .. hwm_bytes - 1` 범위 안에 있습니다. Hint `0`은
hint가 없다는 뜻입니다. 무제한 HWM에는 low water mark가 없습니다.

Core는 보통 이 low water mark에서 credit을 묶어서 반환합니다. Sender가 실제 HWM에
도달한 경우에는 이미 읽힌 누적 byte를 직접 확인하고, 그 뒤 receiver가 현재 보이는 입력을
모두 읽으면 LWM 전에도 한 번 credit을 반환하고 대기 중인 writer를 깨울 수 있습니다. 이
복구는 HWM에 도달한 sender에만 적용하므로 낮은 queue depth의 정상 message마다
cross-thread command를 만들지 않습니다. 기다리는 writer가 없는 pipe를 receiver가 비워도
wakeup을 보내지 않습니다. 이 pipe 기준은 Framework의 receive 재개 기준과 별개입니다.

##### Timing

| 상수 | 설명 |
|---|---|
| `ZLINK_OPT_LINGER` | 종료 시 대기 (ms, `int`; -1=무한, 0=즉시) |
| `ZLINK_OPT_RCVTIMEO` | 수신 타임아웃 (ms, `int`; 기본 `1000`; 명시적으로 -1 설정 시 무한) |
| `ZLINK_OPT_SNDTIMEO` | 송신 타임아웃 (ms, `int`; 기본 `1000`; 명시적으로 -1 설정 시 무한) |
| `ZLINK_OPT_CONNECT_TIMEOUT` | 연결 타임아웃 (ms, `int`) |
| `ZLINK_OPT_RECONNECT_IVL` | 초기 재연결 간격 (ms, `int`) |
| `ZLINK_OPT_RECONNECT_IVL_MAX` | 최대 재연결 간격 (ms, `int`; 0=IVL만 사용) |
| `ZLINK_OPT_HANDSHAKE_IVL` | ZMTP 핸드셰이크 타임아웃 (ms, `int`) |
| `ZLINK_OPT_SUBMIT_RETRY_MODE` | local submit 실패 재시도 모드 (`int`; `ZLINK_SUBMIT_RETRY_OFF` 또는 `ZLINK_SUBMIT_RETRY_LOCAL_FAILURE`, raw socket 기본값 off) |
| `ZLINK_OPT_SUBMIT_RETRY_TIMEOUT` | local submit 실패 재시도 예산 (ms, `int`; raw socket 기본값 0, 0이면 재시도 없음) |
| `ZLINK_OPT_SUBMIT_RETRY_ATTEMPTS` | 최초 submit 이후 추가 재시도 횟수 (`int`; raw socket 기본값 0, 현재 상한 16) |

Submit retry는 `ENOTCONN`, `EHOSTUNREACH` 또는 `ECONNREFUSED`로 분류되는 local
submit 실패만 짧게 다시 시도한다. Locally initiated paired endpoint의 blocking
submit은 pair 검증이 끝날 때까지 이 연결 오류를 재시도 대상으로 처리한다. 대기
예산이 끝나면 공개 결과는 `ZLINK_SUBMIT_BACKPRESSURED`, `errno`는 `EAGAIN`이다.
`ZLINK_DONTWAIT` 호출, backpressure(`EAGAIN`), admission 거절, 인자 오류, request
submit 성공 뒤의 reply timeout은 retry 대상이 아니다.

##### TCP

| 상수 | 설명 |
|---|---|
| `ZLINK_OPT_TCP_KEEPALIVE` | SO_KEEPALIVE (`int`; -1=OS, 0=off, 1=on) |
| `ZLINK_OPT_TCP_KEEPALIVE_CNT` | TCP_KEEPCNT (`int`; -1=OS 기본값) |
| `ZLINK_OPT_TCP_KEEPALIVE_IDLE` | TCP_KEEPIDLE (초, `int`; -1=OS 기본값) |
| `ZLINK_OPT_TCP_KEEPALIVE_INTVL` | TCP_KEEPINTVL (초, `int`; -1=OS 기본값) |
| `ZLINK_OPT_TCP_MAXRT` | 최대 TCP 재전송 타임아웃 (ms, `int`) |
| `ZLINK_OPT_TCP_NODELAY` | TCP_NODELAY 활성화 (`int`; 0 또는 1) |

##### Network

| 상수 | 설명 |
|---|---|
| `ZLINK_OPT_IPV6` | 소켓에서 IPv6 활성화 (`int`; 0 또는 1) |
| `ZLINK_OPT_TOS` | IP Type-of-Service 값 (`int`) |
| `ZLINK_OPT_MULTICAST_HOPS` | 멀티캐스트 TTL (`int`) |
| `ZLINK_OPT_MULTICAST_MAXTPDU` | 최대 멀티캐스트 TPDU 크기 (`int`) |
| `ZLINK_OPT_BINDTODEVICE` | 네트워크 인터페이스 바인딩 (`string`) |
| `ZLINK_OPT_BACKLOG` | listener backlog (`int`) |

##### TLS

| 상수 | 설명 |
|---|---|
| `ZLINK_OPT_TLS_CERT` | PEM 인코딩 TLS 인증서 경로 (`string`) |
| `ZLINK_OPT_TLS_KEY` | PEM 인코딩 TLS 개인 키 경로 (`string`) |
| `ZLINK_OPT_TLS_CA` | PEM 인코딩 CA 인증서 번들 경로 (`string`) |
| `ZLINK_OPT_TLS_VERIFY` | TLS 피어 검증 활성화 (`int`; 0 또는 1) |
| `ZLINK_OPT_TLS_REQUIRE_CLIENT_CERT` | 클라이언트 인증서 요구 (`int`; 0 또는 1) |
| `ZLINK_OPT_TLS_HOSTNAME` | SNI 및 인증서 검증용 호스트명 (`string`) |
| `ZLINK_OPT_TLS_TRUST_SYSTEM` | 시스템 CA 인증서 저장소 신뢰 (`int`; 0 또는 1) |
| `ZLINK_OPT_TLS_PASSWORD` | 개인 키 암호 (`string`) |

##### Behavior

| 상수 | 설명 |
|---|---|
| `ZLINK_OPT_IMMEDIATE` | 완료된 연결에만 메시지 큐 사용 (`int`) |
| `ZLINK_OPT_CONFLATE` | 토픽당 최신 메시지만 유지 (`int`) |
| `ZLINK_OPT_BLOCKY` | socket option API가 지원하지 않는 식별자. `zlink_set_option()`/`zlink_get_option()`은 `ZLINK_CONFIG_NOT_SUPPORTED`/`ENOTSUP`을 반환하며 context 종료 동작은 `ZLINK_CTX_OPT_BLOCKY`로 설정 (`int`, 0 또는 1) |
| `ZLINK_OPT_INVERT_MATCHING` | 토픽 매칭 반전 (`int`) |
| `ZLINK_OPT_ZMP_METADATA` | ZMP 메타데이터 첨부 (`binary`) |

##### Read-only

| 상수 | 설명 |
|---|---|
| `ZLINK_OPT_FD` | 파일 디스크립터 (`zlink_fd_t`, 읽기 전용) |
| `ZLINK_OPT_EVENTS` | 이벤트 상태 비트마스크 (`int`, 읽기 전용) |
| `ZLINK_OPT_TYPE` | 소켓 타입 (`int`, 읽기 전용) |
| `ZLINK_OPT_LAST_ENDPOINT` | 바인딩된 엔드포인트 (`string`, 읽기 전용) |
| `ZLINK_OPT_ROUTE_VALUE_MAX_SIZE` | 최대 discovery route value 크기 (`int`, 읽기 전용) |

#### 전용 함수 (옵션 enum이 아님)

- **Routing ID**: `zlink_set_routing_id()` / `zlink_get_routing_id()`
- **TLS**: `zlink_set_tls_server()` / `zlink_set_tls_client()`
- **Subscribe/Unsubscribe**: `zlink_set_subscription()` / `zlink_unset_subscription()`

## 함수

### zlink_socket

소켓을 생성합니다.

```c
ZLINK_EXPORT void *zlink_socket (void *context_, zlink_socket_type_t type_);
```

지정된 context 내에서 새 소켓을 생성합니다. `type_` 매개변수는 메시징 패턴을
선택합니다. raw socket의 수신 모델은 타입별로 고정됩니다. `PAIR`, `DEALER`,
`SUB`, `XSUB`는 part receive를 사용하며, `ROUTER`는
`zlink_router_recv_part()`로 수신합니다. `STREAM`만이 예외 타입으로,
raw part receive / raw callback
(`zlink_recv_handler()`) / packet callback
(`zlink_stream_packet_handler()`) 세 모드 중 하나를 선택해 사용할 수
있습니다. 소켓은 context가 종료되기 전에 `zlink_close()`로 닫아야 합니다.

**반환값:** 성공 시 소켓 핸들, 실패 시 `NULL` (errno가 설정됨).

**에러:** 소켓 타입이 유효하지 않으면 `EINVAL`. 최대 소켓 수에 도달하면
`EMFILE`. Context가 종료된 경우 `ETERM`.

**스레드 안전성:** Context에 대해 스레드 안전합니다.

**참고:** `zlink_close`, `zlink_ctx_new`

---

### zlink_recv_handler

raw `STREAM` 소켓에 raw 수신 콜백을 부착합니다.

```c
ZLINK_EXPORT zlink_handler_result_t zlink_recv_handler (
  void *s_, zlink_socket_msg_handler_fn handler_, void *userdata_);
```

raw `STREAM` 전용 direct receive callback 등록 함수입니다. 지원 대상은
raw `STREAM` 뿐이며, 다른 subject(PAIR, DEALER 등)는 `ENOTSUP`로 실패합니다.
attach 이후 같은 handle의 `zlink_recv_part()`, `zlink_stream_packet_handler()`,
data-plane `ZLINK_POLLIN`은 `errno=EBUSY`로 실패합니다. 동일 handle에 대한
두 번째 attach도 `errno=EBUSY`입니다.

자세한 계약은 [08-stream.ko.md](08-stream.ko.md)를 참조하세요.

**반환값:** 성공 시 `ZLINK_HANDLER_OK`. 실패 시에는 `zlink_handler_result_t`
값을 반환합니다. 상세 내부 errno는 진단을 위해 `zlink_errno()`로 유지됩니다.

**참고:** `zlink_stream_packet_handler`, `zlink_socket`, `zlink_close`

---

### zlink_recv_part

raw 소켓에서 메시지 파트 하나를 수신합니다.

```c
ZLINK_EXPORT zlink_recv_result_t zlink_recv_part (void *s_,
                                                  const zlink_routing_id_t **source_rid_out_,
                                                  zlink_msg_t *part_out_,
                                                  zlink_part_flag_t *has_more_out_,
                                                  zlink_recv_flags_t flags_);
```

지원 타입은 raw `PAIR`, `DEALER`, `STREAM`입니다. raw `PUB`, `XPUB`,
`SUB`, `XSUB`, `ROUTER`에는 사용할 수 없으며
`ZLINK_RECV_NOT_SUPPORTED`를 반환하고 `errno`를 `ENOTSUP`로 설정합니다.
`part_out_`은 초기화된 메시지여야 하고 `part_out_`과 `has_more_out_`은
필수입니다.

성공하면 수신한 파트의 소유권이 호출자에게 이전되므로 호출자는
`zlink_msg_close(part_out_)`를 정확히 한 번 호출해야 합니다. 실패하면 파트
소유권은 이전되지 않습니다. `source_rid_out_`은 선택 사항입니다. `STREAM`은
Core가 소유한 routing ID 보기를 반환하고 `PAIR`와 `DEALER`는 `NULL`을
반환합니다. 이 보기는 다음 raw 수신 호출 뒤에도 유지해야 한다면 호출자가
복사해야 합니다. `*has_more_out_`은 다음 파트가 있으면 `ZLINK_PART_MORE`,
마지막 파트이면 `ZLINK_PART_FINAL`입니다.

한 멀티파트 메시지의 첫 파트부터 마지막 파트까지 같은 스레드에서 이 함수로
계속 수신해야 합니다. `ZLINK_RECV_FLAGS_DONTWAIT`를 사용하고 수신할 파트가
없으면 `ZLINK_RECV_NO_DATA`를 반환하고 `errno`를 `EAGAIN`으로 설정합니다.

---

### zlink_close

소켓을 닫고 리소스를 해제합니다.

```c
ZLINK_EXPORT zlink_close_result_t zlink_close (void *s_);
```

소켓을 닫고 관련된 모든 리소스를 해제합니다. 송신 대기열에 남아 있는 메시지는
`ZLINK_OPT_LINGER` 설정에 따라 폐기되거나 송신됩니다. 공개 핸들은 계층적 계약을
따릅니다: hot-path send 작업은 여러 스레드에서 동시 호출이 가능하고, 저빈도
제어 경로는 정확성을 위해 직렬화되며, close/destroy는 엄격한 lifecycle gate를
사용합니다. 다른 스레드에서 동일 핸들에 대해 콜백이나 API 호출이 진행 중이면
`errno=EBUSY`로 실패합니다. close가 accepted된 뒤 새 API 진입은
`errno=ESHUTDOWN`으로 실패합니다. send-ready 또는 monitor 콜백 내에서의
self-close는 콜백 에필로그까지 지연됩니다.

**반환값:** 성공 시 `ZLINK_CLOSE_OK`, 실패 시 `zlink_close_result_t` 값. `zlink_errno()`는 진단용 내부 errno를 그대로 유지합니다.

**에러:** 포인터가 유효하지 않으면 `EFAULT`, opaque value가 stale 상태이면 `ESTALE`.
콜백이나 작업이 진행 중이면 `EBUSY`.

**참고:** `zlink_socket`

---

### zlink_set_option

공통 옵션을 설정합니다.

```c
ZLINK_EXPORT zlink_config_result_t zlink_set_option (void *handle_,
                      zlink_option_t option_,
                      const void *optval_,
                      size_t optvallen_);
```

공통 옵션을 설정한다. `handle_`은 raw socket 또는 discovery다.
`option_` 매개변수는 `zlink_option_t` enum 값입니다. `optval_`
포인터는 값을 제공하고 `optvallen_`은 크기를 바이트 단위로 지정합니다.
`ZLINK_OPT_SNDHWM`과 `ZLINK_OPT_RCVHWM`은 정확한 `uint64_t` 값을 요구합니다.

Raw socket과 discovery의 설정 시점은 각 option 계약을 따른다.

**반환값:** 성공 시 `ZLINK_CONFIG_OK`, 실패 시 `zlink_config_result_t` 값. `zlink_errno()`는 진단용 내부 errno를 그대로 유지합니다.

**에러:** 옵션을 알 수 없거나 값이 범위를 벗어나거나 byte-count option의 크기가
정확하지 않으면 `EINVAL`. Context가 종료된 경우 `ETERM`.

**참고:** `zlink_get_option`

---

### zlink_get_option

공통 옵션을 조회합니다.

```c
ZLINK_EXPORT zlink_config_result_t zlink_get_option (void *handle_,
                      zlink_option_t option_,
                      void *optval_,
                      size_t *optvallen_);
```

공통 옵션의 현재 값을 가져온다. `handle_`은 raw socket 또는 discovery다. 두 HWM
byte-count option에는 `uint64_t` output buffer가 필요하고, 호출할 때
`*optvallen_`이 정확히 `sizeof(uint64_t)`여야 합니다. 더 큰 임시 buffer나 이전
4-byte 크기를 포함해 그 밖의 크기는 값을 잘라 쓰거나 일부만 채우지 않고
`ZLINK_CONFIG_INVALID_ARGUMENT`와 `errno == EINVAL`로 실패합니다. 성공하면
`*optvallen_`은 `sizeof(uint64_t)`를 유지합니다.

**반환값:** 성공 시 `ZLINK_CONFIG_OK`, 실패 시 `zlink_config_result_t` 값. `zlink_errno()`는 진단용 내부 errno를 그대로 유지합니다.

**참고:** `zlink_set_option`

---

### zlink_socket_set_receive_flow_state

이 socket의 receive-flow 상태를 설정하고 paired completion lane으로 동기화합니다.

```c
ZLINK_EXPORT zlink_config_result_t zlink_socket_set_receive_flow_state (
  void *handle_, zlink_receive_flow_state_t state_);
```

`state_`를 socket 전체의 receive-flow 상태로 저장하고, paired DEALER/ROUTER
completion lane으로 연결된 모든 peer에게 보냅니다. 이 호출은 socket을 소유한
runtime thread가 local 상태를 저장한 시점에 완료되며 peer가 관측할 때까지
기다리지 않습니다. 현재 상태를 다시 설정하면 성공하고 새로 보내는 것은 없습니다.

**반환값:** 성공 시 `ZLINK_CONFIG_OK`이며 현재 상태를 다시 설정한 경우도
포함합니다. Completion lane이 없는 socket 유형은 `ZLINK_CONFIG_NOT_SUPPORTED`를
반환하고 기존 byte HWM과 transport backpressure를 그대로 유지합니다. 전체 결과
표는 [Errors](../03-errors.ko.md)가 소유합니다.

**참고:** `zlink_monitor_status`

---

### zlink_set_routing_id

소켓의 라우팅 아이덴티티를 설정합니다.

```c
ZLINK_EXPORT zlink_config_result_t zlink_set_routing_id (void *handle_,
                           const void *data_,
                           size_t size_);
```

raw socket의 routing ID를 설정한다. 길이는 1..255 bytes이며 값은 binary-safe하다. bind 또는 connect 전에
설정한다. 다른 handle 종류는
`ZLINK_CONFIG_NOT_SUPPORTED`, `errno == ENOTSUP`이다.
Caller가 routing ID를 설정하지 않으면 Core는 socket 생성 시 RFC 4122 UUID v4 bit layout의 16-byte binary
routing ID를 발급한다. 이 기본값은 UUID 문자열이 아니라 raw 16 bytes다.

**반환값:** 성공 시 `ZLINK_CONFIG_OK`, 실패 시 `zlink_config_result_t` 값. `zlink_errno()`는 진단용 내부 errno를 그대로 유지합니다.

**참고:** `zlink_get_routing_id`

---

### zlink_get_routing_id

소켓의 라우팅 아이덴티티를 조회합니다.

```c
ZLINK_EXPORT zlink_config_result_t zlink_get_routing_id (void *handle_,
                           zlink_routing_id_t *out_);
```

raw socket에 설정되었거나 Core가 자동 발급한 routing ID를 caller-owned `zlink_routing_id_t`에 복사한다.

**반환값:** 성공 시 `ZLINK_CONFIG_OK`, 실패 시 `zlink_config_result_t` 값. `zlink_errno()`는 진단용 내부 errno를 그대로 유지합니다.

**참고:** `zlink_set_routing_id`

---

### zlink_set_tls_server

서버 측 TLS를 구성합니다.

```c
ZLINK_EXPORT zlink_config_result_t zlink_set_tls_server (void *handle_,
                           const char *cert_,
                           const char *key_,
                           int require_client_cert_);
```

서버 소켓에 TLS 인증서, 개인 키를 설정하고, 클라이언트 인증서 요구 여부를
지정합니다.

이 함수는 TLS를 지원하는 raw server socket에 적용된다. 지원하지 않는 raw socket type과 다른 handle은
`ZLINK_CONFIG_NOT_SUPPORTED`, `errno == ENOTSUP`이다.

**반환값:** 성공 시 `ZLINK_CONFIG_OK`, 실패 시 `zlink_config_result_t` 값. `zlink_errno()`는 진단용 내부 errno를 그대로 유지합니다.

**참고:** `zlink_set_tls_client`, `zlink_bind`

---

### zlink_set_tls_client

클라이언트 측 TLS를 구성합니다.

```c
ZLINK_EXPORT zlink_config_result_t zlink_set_tls_client (void *handle_,
                           const char *ca_cert_,
                           const char *hostname_,
                           int trust_system_);
```

클라이언트 소켓에 CA 인증서, 호스트명(SNI 및 인증서 검증용), 시스템 CA
저장소 신뢰 여부를 설정합니다.

이 함수는 TLS를 지원하는 raw client socket에 적용된다. 지원하지 않는 raw socket type과 다른 handle은
`ZLINK_CONFIG_NOT_SUPPORTED`, `errno == ENOTSUP`이다.

**반환값:** 성공 시 `ZLINK_CONFIG_OK`, 실패 시 `zlink_config_result_t` 값. `zlink_errno()`는 진단용 내부 errno를 그대로 유지합니다.

**참고:** `zlink_set_tls_server`, `zlink_connect`

---

### zlink_bind

소켓을 주소에 바인딩합니다.

```c
ZLINK_EXPORT zlink_bind_result_t zlink_bind (void *s_, const char *addr_);
```

소켓을 로컬 엔드포인트에 바인딩합니다. 엔드포인트 문자열은
`transport://address` 형식을 사용하며, 지원되는 `transport`는 다음과 같습니다:

- `tcp://interface:port` 또는 `tcp://*:port`
- `inproc://name` (프로세스 내 직접 연결, in-process transport)
- `ipc://pathname` (프로세스 간, POSIX 전용)
- `ws://interface:port` (WebSocket)
- `tls://interface:port` (TLS 암호화 TCP)

소켓은 여러 엔드포인트에 바인딩할 수 있습니다. TCP의 경우 포트 0을 지정하면
시스템이 임시 포트를 할당합니다. 실제 엔드포인트를 가져오려면
`ZLINK_OPT_LAST_ENDPOINT`를 사용하세요.

**반환값:** 성공 시 `ZLINK_BIND_OK`, 실패 시 `zlink_bind_result_t` 값. `zlink_errno()`는 진단용 내부 errno를 그대로 유지합니다.

**에러:** 주소가 이미 사용 중이면 `EADDRINUSE`. 인터페이스가 존재하지 않으면
`EADDRNOTAVAIL`. `transport`가 지원되지 않으면 `EPROTONOSUPPORT`.

**참고:** `zlink_connect`, `zlink_unbind`

---

### zlink_connect

소켓을 원격 주소에 연결합니다.

```c
ZLINK_EXPORT zlink_connect_result_t zlink_connect (void *s_, const char *addr_);
```

소켓을 원격 엔드포인트에 연결합니다. 엔드포인트 형식은 `zlink_bind()`와
동일합니다. 소켓은 여러 엔드포인트에 연결할 수 있으며, 피어가 사용 불가능해지면
라이브러리가 자동으로 재연결을 처리합니다.

**반환값:** 성공 시 `ZLINK_CONNECT_OK`, 실패 시 `zlink_connect_result_t` 값. `zlink_errno()`는 진단용 내부 errno를 그대로 유지합니다.

**참고:** `zlink_bind`, `zlink_disconnect`

---

### zlink_unbind

소켓의 주소 바인딩을 해제합니다.

```c
ZLINK_EXPORT zlink_connect_result_t zlink_unbind (void *s_, const char *addr_);
```

이전에 설정된 바인딩을 제거합니다.

**반환값:** 성공 시 `ZLINK_CONNECT_OK`, 실패 시 `zlink_connect_result_t` 값. `zlink_errno()`는 진단용 내부 errno를 그대로 유지합니다.

**참고:** `zlink_bind`

---

### zlink_disconnect

소켓의 원격 주소 연결을 해제합니다.

```c
ZLINK_EXPORT zlink_connect_result_t zlink_disconnect (void *s_, const char *addr_);
```

이전에 설정된 연결을 제거합니다.

**반환값:** 성공 시 `ZLINK_CONNECT_OK`, 실패 시 `zlink_connect_result_t` 값. `zlink_errno()`는 진단용 내부 errno를 그대로 유지합니다.

**참고:** `zlink_connect`

---

### zlink_disconnect_rid

소켓에 연결된 peer를 routing id로 찾아 종료합니다.

```c
ZLINK_EXPORT zlink_connect_result_t zlink_disconnect_rid (
  void *s_,
  const zlink_routing_id_t *peer_rid_);
```

`peer_rid_`는 비어 있으면 안 됩니다. 성공하면 해당 peer pipe는 비동기
종료 절차에 들어갑니다. 성공 반환은 remote peer가 종료 이벤트를 이미
처리했다는 뜻이 아닙니다.

ROUTER와 STREAM은 routing map을 사용해 대상을 찾습니다. STREAM에서는
`peer_rid_`가 반드시 4바이트 연결 routing id여야 합니다. 그 외 socket은
현재 연결된 pipe의 source routing id snapshot에서 일치하는 peer를 찾습니다.
동일한 routing id가 둘 이상이면 대상을 확정할 수 없으므로 실패합니다.

**반환값:** 성공 시 `ZLINK_CONNECT_OK`. 대상 없음은
`ZLINK_CONNECT_NOT_FOUND`, 중복 routing id는 `ZLINK_CONNECT_CONFLICT`,
lifecycle 소유권 충돌은 `ZLINK_CONNECT_BUSY`입니다. `zlink_errno()`는
진단용 내부 errno를 그대로 유지합니다.

**참고:** `zlink_disconnect`, `ZLINK_OPT_RID_DUPLICATE_POLICY`

---

### zlink_disconnect_transport_pair

모니터 이벤트에서 얻은 transport pair identity와 일치하는 연결만
비동기 종료 대상으로 지정한다.

```c
ZLINK_EXPORT zlink_connect_result_t zlink_disconnect_transport_pair (
  void *s_, uint64_t transport_pair_id_, uint64_t transport_pair_generation_);
```

`transport_pair_id_`와 `transport_pair_generation_`은 종료할 연결의 모니터
이벤트에서 복사해야 한다. 이 함수는 해당 pair에 속한 모든 lane을 종료
대상으로 지정하며, 같은 peer routing id를 사용하는 다른 연결에는 영향을
주지 않는다. 두 값 중 하나라도 0이면 잘못된 인자이며, 이미 제거된
identity를 지정하면 `ZLINK_CONNECT_NOT_FOUND`를 반환한다.

**반환값:** 하나 이상의 lane을 종료 대상으로 지정하면
`ZLINK_CONNECT_OK`를 반환한다. 그 밖의 경우에는
`zlink_connect_result_t` 오류를 반환하고, 자세한 원인은 `zlink_errno()`로
확인한다.

**참고:** `zlink_disconnect_rid`, `zlink_socket_monitor_recv`

---

### 비동기 송신 admission

완전한 멀티파트 레코드 하나를 Core에 인계하고 그에 대한 완료 통지를 정확히
한 번 받습니다.

```c
ZLINK_EXPORT zlink_submit_result_t zlink_send_async (
  void *s_, zlink_msg_t *parts_, size_t part_count_,
  const zlink_send_async_options_t *options_,
  zlink_send_op_id_t *op_id_out_);

ZLINK_EXPORT zlink_handler_result_t zlink_send_complete_handler (
  void *s_, zlink_send_complete_handler_fn handler_, void *userdata_);

ZLINK_EXPORT zlink_submit_result_t zlink_send_async_cancel (
  void *s_, zlink_send_op_id_t op_id_);
```

`zlink_send_async` 지원 대상은 raw `PAIR`, `DEALER`, `ROUTER`, `STREAM`입니다.
그 밖의 socket 타입은 `ZLINK_SUBMIT_NOT_SUPPORTED`입니다. STREAM은 프레임
경계가 없는 raw 바이트를 나르므로 STREAM 레코드는 항상 정확히 1 part입니다.

`ZLINK_SUBMIT_OK`이면 `parts_[0 .. part_count_)` 전부의 소유권이 Core로
넘어가고 호출자는 이후 close를 포함해 그 메시지를 만지지 않습니다. 그 밖의
결과에서는 소유권이 호출자에게 남습니다.

이 호출은 블로킹하지 않습니다. target에 여유가 있으면 호출 스레드에서 그대로
admit되며 완료 콜백이 이 함수가 반환하기 전에 인라인으로 실행될 수 있습니다.
target이 backpressure 상태면 레코드는 pending operation으로 예약되고 완료는
나중에 도착합니다. byte HWM 회계는 동기 멀티파트 send와 동일하게 레코드
하나를 메시지 하나로 계산합니다.

Pending operation은 socket 단위로 `ZLINK_OPT_SEND_PENDING_MAX_MSGS`와
`ZLINK_OPT_SEND_PENDING_MAX_BYTES`에 의해 유한합니다. 둘 중 하나를 넘기면
`ZLINK_SUBMIT_BACKPRESSURED`를 반환하고 part 소유권은 호출자에게 남습니다 —
여기가 앱이 정책을 소유하는 지점입니다. 두 옵션 모두 `0`을 무제한으로 받지
않습니다. 무제한 예약 큐는 HWM 우회이기 때문입니다.

같은 target의 pending operation은 제출 순서대로 admit되고 그 순서대로
완료됩니다. target 내부의 head-of-line 차단은 의도된 동작입니다. 그 target
큐가 하나의 논리 스트림이기 때문입니다. 서로 다른 target 사이에는 순서
보장이 없고, 동기 send는 같은 HWM을 두고 동등하게 경쟁합니다. 동기 send를
pending 앞뒤로 재배치하는 특례는 없습니다.

ROUTER는 `options_->target`이 필요합니다. DEALER는 `NULL`을 넘길 수 있으며 이
경우 Core가 제출 시점에 선택을 확정합니다. 선택을 완료 시점까지 미루면
target별 순서를 정의할 수 없기 때문입니다. PAIR는 이 필드를 무시합니다.

`zlink_send_complete_handler`는 교체 전용이고 `NULL`은 유효하지 않습니다. 첫
`zlink_send_async` 이전에 반드시 설치해야 하며, 그렇지 않으면 submit이
`errno=EINVAL`로 실패합니다. 결과를 보고할 곳이 없는 operation이 되기
때문입니다. 이 socket 자신의 완료 콜백 안에서 핸들러를 교체하면
`errno=EDEADLK`로 실패합니다.

콜백 계약은 다음과 같습니다.

- `ZLINK_SUBMIT_OK`을 반환한 operation마다 완료가 정확히 한 번 실행됩니다.
- 같은 target의 완료는 제출 순서대로 실행됩니다.
- 한 socket의 완료끼리는 절대 동시에 실행되지 않습니다.
- 고정된 스레드를 약속하지 않습니다. 콜백은 `zlink_send_async` 안에서
  인라인으로, backpressure가 풀린 뒤에는 Core async mailbox 스레드에서,
  timeout에서는 Core deadline 스레드에서, close나 context 종료에서는 그것을
  호출한 스레드에서, 그리고 이 socket에 `ZLINK_POLLCOMPLETION` 등록이 있으면
  `zlink_poller_wait`를 호출한 스레드에서 실행될 수 있습니다.
- 콜백은 완료를 앱 상태에 전달하는 일만 해야 합니다. 콜백 안에서 send,
  publish, request 계열 진입점을 호출하면 `errno=EDEADLK`로 실패합니다.

`ZLINK_POLLCOMPLETION`으로 socket을 poller에 등록하면 이 콜백의 디스패치
소유권이 Core async mailbox 스레드에서 `zlink_poller_wait` 호출 스레드로
넘어갑니다. 디스패치 위치만 달라질 뿐 등록 API도, 콜백도, 이벤트도, 보장도
같습니다. 두 디스패치 소유자는 socket 단위로 상호 배타적입니다. pending
상한이 콜백을 기다릴 수 있는 operation 수를 제한하므로 완료가 유실되는 일은
없습니다.

`zlink_send_async_cancel`은 요청입니다. `ZLINK_SUBMIT_OK`은 취소가 접수됐고
완료가 `ZLINK_SEND_TERMINAL` + `ECANCELED`로 온다는 뜻입니다.
`ZLINK_SUBMIT_NOT_FOUND`는 그 id의 pending operation이 없다는 뜻입니다.
`ZLINK_SUBMIT_INVALID_STATE`는 admit이 이미 커밋되어 완료가
`ZLINK_SEND_ADMITTED`로 온다는 뜻입니다. 취소된 operation도 완료는 정확히 한
번 발생합니다. 통지가 없으면 호출자의 suspension이 영원히 매달리기 때문입니다.

`zlink_close`와 `zlink_ctx_term`은 반환 전에 모든 pending operation을 각각
`ECANCELED`와 `ETERM`으로 즉시 실패시킵니다. `ZLINK_OPT_LINGER`는 적용되지
않습니다. linger는 이미 pipe에 admit된 바이트를 다루고 pending operation은
아직 admit되지 않았기 때문입니다.

**반환값:** `zlink_send_async`와 `zlink_send_async_cancel`은 성공 시
`ZLINK_SUBMIT_OK`, `zlink_send_complete_handler`는 `ZLINK_HANDLER_OK`를
반환합니다. `zlink_errno()`는 진단용 내부 errno를 그대로 유지합니다.

**참고:** `zlink_send_part`, `zlink_send_part_rid`,
`zlink_select_routed_submit_target`

---

### Routed submit target 선택

```c
typedef struct zlink_routed_submit_target_t {
  zlink_routing_id_t peer_rid;
  uint64_t transport_pair_id;
  uint64_t transport_pair_generation;
} zlink_routed_submit_target_t;

ZLINK_EXPORT zlink_submit_result_t zlink_select_routed_submit_target (
  void *socket_, const zlink_routing_id_t *router_rid_or_null_,
  zlink_routed_submit_target_t *target_out_);

```

`zlink_select_routed_submit_target()`은 binding이 pending operation을 등록하기 전에 사용할
exact value identity를 반환한다. ROUTER에서는 `router_rid_or_null_`에 non-NULL RID를 전달하고,
DEALER에서는 NULL을 전달한다. ROUTER는 해당 RID의 admitted application pipe를 snapshot한다.
DEALER는 연결됐고 가중치가 양수인 application pipe 전체를 대상으로 weighted selection 한 단계를
확정한다. 이 후보 집합에는 HWM으로 일시 정지된 pipe도 포함된다. 따라서 A가 막혔다는 이유로
선택 자체가 B로 우회되지 않으며, A를 선택한 operation은 A의 exact readiness만 기다린다.

반환값은 pipe lifetime, HWM credit 또는 Core resource를 점유하는 lease가 아니다. 선택 직후에도
연결 상태나 credit은 바뀔 수 있으므로 이후의 exact submit은 `BACKPRESSURED` 또는 terminal
route 결과를 반환할 수 있다. 이 값은 이후 exact submit이 가리킬 target을 지정하며
`zlink_send_async_options_t`의 `target` 필드가 그중 하나다. 그 target의 pending 상태는 Core가
소유한다. Stale pair generation은 다른 연결로 retarget하지 않는다.

Core part sequence는 첫 part가 선택한 exact pair fence를 FINAL까지 유지하고 중간 실패를
전체 rollback하므로 peer에 prefix가 보이지 않는다. `zlink_send_async`는 완전한 레코드를 한
번의 호출로 제출하므로 이 sequence를 앱 코드 구간에 걸쳐 점유하는 일이 없다.

Request part API는 첫 frame이 wire에 보이기 전에 reply correlation과 timeout lifecycle을
등록하며, submit 실패 시 이를 제거하고 handler를 호출하지 않는다. `ZLINK_SUBMIT_OK` 뒤에는
handler가 reply 또는 terminal 결과로 정확히 한 번 호출된다.

`ZLINK_SEND_TERMINAL` 완료는 application pipe detach·disconnect에 `ENOTCONN`, 취소와
socket close에 `ECANCELED`, context 종료에 `ETERM`을 전달한다. 여러 종료 원인이
경합하면 처음 확정된 원인을 싣고, operation은 그래도 정확히 한 번 완료된다.

### Retained-credit receive

```c
typedef struct zlink_hwm_budget_lease_t zlink_hwm_budget_lease_t;

ZLINK_EXPORT int zlink_recv_with_hwm_budget_lease (
  void *socket_, zlink_msg_t *message_,
  zlink_hwm_budget_lease_t **lease_out_, int flags_);
ZLINK_EXPORT zlink_recv_result_t zlink_recv_part_with_hwm_budget_lease (
  void *s_, const zlink_routing_id_t **source_rid_out_,
  zlink_msg_t *part_out_, zlink_hwm_budget_lease_t **lease_out_,
  zlink_part_flag_t *has_more_out_, zlink_recv_flags_t flags_);
ZLINK_EXPORT zlink_recv_result_t
zlink_dealer_recv_part_with_hwm_budget_lease (
  void *dealer_, uint8_t *message_type_out_, uint64_t *request_seq_out_,
  zlink_msg_t *part_out_, zlink_hwm_budget_lease_t **lease_out_,
  zlink_part_flag_t *has_more_out_, zlink_recv_flags_t flags_);
ZLINK_EXPORT zlink_recv_result_t
zlink_router_recv_part_v2_with_hwm_budget_lease (
  void *router_, const zlink_routing_id_t **source_node_rid_out_,
  uint64_t *request_seq_out_, uint64_t *transport_pair_id_out_,
  uint64_t *transport_pair_generation_out_, zlink_msg_t *part_out_,
  zlink_hwm_budget_lease_t **lease_out_,
  zlink_part_flag_t *has_more_out_, zlink_recv_flags_t flags_);
ZLINK_EXPORT zlink_recv_result_t
zlink_subscribe_part_with_hwm_budget_lease (
  void *sub_, const zlink_routing_id_t **source_rid_out_,
  char *topic_id_buf_, size_t topic_id_capacity_,
  size_t *topic_id_len_out_, zlink_msg_t *part_out_,
  zlink_hwm_budget_lease_t **lease_out_,
  zlink_part_flag_t *has_more_out_, zlink_recv_flags_t flags_);
ZLINK_EXPORT void zlink_hwm_budget_lease_release (
  zlink_hwm_budget_lease_t **lease_p_);
```

각 variant는 대응하는 기존 receive의 framing, metadata와 반환값을 그대로 유지하며,
성공한 호출 하나가 caller-visible physical payload frame 하나를 반환하면 그 frame의
accounting owner만 queue에서 opaque lease로 원자적으로 옮긴다. 새 multipart transaction을
만들지 않는다. Part variant는 기존처럼 파트마다 호출하며 호출마다 lease 하나를 반환한다.
Dealer의 message type·request sequence, Router의 source RID·request sequence·transport pair,
SUB의 topic과 raw `STREAM`의 source RID는 각 기존 API와 같은 Core-owned parsing 결과다.

Core가 합성한 raw `ROUTER`·`STREAM` routing-ID frame이나 `XPUB` local subscription event처럼
physical application queue charge가 없는 성공 호출은 `*lease_out_ == NULL`이다. Dealer와
Router의 내부 envelope, SUB topic metadata, credential과 handshake frame은 caller-visible
payload가 아니므로 Core가 즉시 소비하고 lease로 노출하지 않는다. 그 뒤 실제 physical
payload를 반환하는 성공 호출은 non-NULL lease를 반환한다.

Lease는 origin directional queue id·generation과 accounted byte를 보존하고 writer credit을
즉시 반환하지 않는다. `core_queue_accounted_bytes`에서
`application_accounted_bytes`로 owner가 이동하지만 두 값을 합한
`current_accounted_bytes`는 변하지 않는다. 일반 receive는 기존처럼 dequeue에서 credit을
반환한다. Retained receive를 활성화한 socket의 내부 command worker는 deferred credit만
처리하며 receive handler가 없으면 caller-visible payload를 대신 소비하지 않는다.

Lease pointer는 복사해 이중 소유하면 안 되며 thread 간 소유권을 이전할 수 있다.
`zlink_hwm_budget_lease_release()`는 NULL과 `*lease_p_ == NULL`에 안전하고, 성공적으로
소유권을 반환하면 `*lease_p_`를 NULL로 만든다. 따라서 같은 pointer 변수에 대한 반복
release는 효과가 없다. Release는 exact origin generation에 credit을 한 번만 반환한다.
Origin이 먼저 detach되거나 generation이 교체되면 retired record를 유지하고, old lease의
release는 새 generation의 credit이나 wake를 변경하지 않는다. 마지막 old lease release 뒤
retired record를 제거한다. Context shutdown은 신규 lease 이전을 막으며 강제 종료는 남은
lease를 invalid 처리하고 counter를 한 번만 정리한다. 이후 caller의 release는 안전하다.

---

### zlink_multipart_close

멀티파트 메시지 배열의 모든 파트를 close합니다.

```c
ZLINK_EXPORT void zlink_multipart_close (zlink_msg_t *parts, size_t part_count);
```

각 원소에 대해 `zlink_msg_close()`를 호출하는 편의 함수입니다.

**참고:** `zlink_msg_close`

---

## 소켓 모니터

### zlink_socket_monitor_open

recv 모드로 소켓 모니터 핸들을 열고 반환합니다.

```c
ZLINK_EXPORT void *zlink_socket_monitor_open (void *s_,
                                 const zlink_socket_monitor_open_options_t *options_);
```

소켓 `s_`에 대한 모니터를 생성하고 핸들을 반환합니다. `options_->events`
비트마스크로 관찰할 이벤트를 선택합니다. 모니터는 **recv 모드**로 시작합니다.
`zlink_socket_monitor_recv()`로 이벤트를 직접 수신하거나,
`zlink_socket_monitor_handler()`로 callback-only 모드로 전환할 수 있습니다.
반환된 핸들은 더 이상 필요하지 않을 때 `zlink_monitor_close()`로 닫아야 합니다.

**반환값:** 성공 시 모니터 핸들, 실패 시 `NULL` (errno가 설정됨).

**참고:** `zlink_socket_monitor_handler`, `zlink_socket_monitor_recv`,
`zlink_monitor_status`, `zlink_monitor_close`
