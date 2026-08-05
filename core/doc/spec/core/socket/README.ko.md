---
title: "소켓 — 공통 명세"
---

[English](README.en.md) | 한국어

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

### zlink_send_ready_handler_fn

```c
typedef void (*zlink_send_ready_handler_fn) (void *subject_, void *userdata_);
```

해당 handle이 backpressure 상태에서 벗어나 송신 재시도를 시도할 가치가
있는 시점에 호출되는 콜백입니다. `ZLINK_POLLOUT`과 같은 send-recovery
readiness 축을 공유하며, 콜백 자체는 재시도 성공을 보장하지 않습니다.

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
  ZLINK_OPT_AUTO_HWM_MSG_UNIT_BYTES    = 0x3034,
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
| `ZLINK_OPT_AUTO_HWM_MSG_UNIT_BYTES` | 자동 byte HWM을 계산할 때 쓰는 planning unit (`uint64_t`, byte 단위; `0`=소켓 타입 기본값) |
| `ZLINK_OPT_MAXMSGSIZE` | 최대 인바운드 메시지 크기 (`int64_t`; -1=무제한) |

세 `uint64_t` option은 `zlink_set_option()`과 `zlink_get_option()`에서 정확히
`sizeof(uint64_t)` byte를 사용해야 합니다. 이전 4-byte 값은 과거 message count로
해석하지 않고 `ZLINK_CONFIG_INVALID_ARGUMENT`로 거절합니다. 자동 planning unit은
관찰한 message 크기가 아닙니다. Profile과 connection bucket이 선택한 slot에 이 값을
곱해 계획 byte HWM을 계산합니다. Pipe admission은 실제로 보관한 byte를 계산합니다.

HWM은 각 directional pipe에 적용합니다. Accounted byte가 limit에 도달하면 receiver가
충분한 byte credit을 반환할 때까지 이후 write가 대기합니다. 비어 있는 pipe에는
accounted 크기가 HWM보다 큰 message 한 건을 허용할 수 있습니다. 따라서 유효한 큰
message를 HWM이 작다는 이유만으로 모두 거절하지 않습니다. 이 message도
`ZLINK_OPT_MAXMSGSIZE`를 만족해야 하며, 한 건을 허용한 뒤에는 이후 write가 대기합니다.
`ZLINK_OPT_MAXMSGSIZE`가 무제한인 방향에서는 complete message 한 건에만 이 예외를 적용합니다.
끝나지 않은 multipart에는 일반 byte HWM을 적용하므로 `MORE` frame이 제한 없이 누적되지
않습니다.

Core는 보통 `ceil(hwm_bytes / 2)`에서 credit을 묶어서 반환합니다. Sender가 실제 HWM에
도달한 경우에는 이미 읽힌 누적 byte를 직접 확인하고, 그 뒤 receiver가 현재 보이는 입력을
모두 읽으면 LWM 전에도 한 번 credit을 반환할 수 있습니다. 이 복구는 HWM에 도달한 sender에만
적용하므로 낮은 queue depth의 정상 message마다 cross-thread command를 만들지 않습니다. 이
pipe 기준은 Framework의 receive 재개 기준과 별개입니다.

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

**에러:** 핸들이 유효한 소켓이 아니면 `ENOTSOCK`. 콜백이나 작업이 진행 중이면
`EBUSY`.

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
`ZLINK_OPT_SNDHWM`, `ZLINK_OPT_RCVHWM`,
`ZLINK_OPT_AUTO_HWM_MSG_UNIT_BYTES`는 정확한 `uint64_t` 값을 요구합니다.

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

공통 옵션의 현재 값을 가져온다. `handle_`은 raw socket 또는 discovery다. 세 HWM
byte-count option에는 `uint64_t` output buffer가 필요하고, 호출할 때
`*optvallen_`이 정확히 `sizeof(uint64_t)`여야 합니다. 더 큰 임시 buffer나 이전
4-byte 크기를 포함해 그 밖의 크기는 값을 잘라 쓰거나 일부만 채우지 않고
`ZLINK_CONFIG_INVALID_ARGUMENT`와 `errno == EINVAL`로 실패합니다. 성공하면
`*optvallen_`은 `sizeof(uint64_t)`를 유지합니다.

**반환값:** 성공 시 `ZLINK_CONFIG_OK`, 실패 시 `zlink_config_result_t` 값. `zlink_errno()`는 진단용 내부 errno를 그대로 유지합니다.

**참고:** `zlink_set_option`

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

### zlink_send_ready_handler

send-ready 콜백을 설정하거나 교체합니다.

```c
ZLINK_EXPORT zlink_handler_result_t zlink_send_ready_handler (
  void *s_, zlink_send_ready_handler_fn handler_, void *userdata_);
```

핸들러는 교체 전용입니다. NULL 전달은 유효하지 않습니다. 교체 성공 시 다음 쓰기
가능 전환부터 반영됩니다. 동일 핸들의 send-ready 콜백 내에서 재진입 호출하면
`errno=EDEADLK`로 실패합니다.

지원 대상은 raw `PAIR`, `PUB`, `XPUB`, `DEALER`, `ROUTER`, `STREAM`입니다.
send-ready는 수신 모드와 독립적입니다.

이 콜백과 `ZLINK_POLLOUT`은 같은 send-recovery readiness 축을 가리킵니다.
`BACKPRESSURED` 결과를 본 호출자가 재시도할 가치가 있는 시점을 알립니다.
readiness 신호 자체는 재시도 성공을 보장하지 않으며, 알림 뒤 첫 재시도가
다시 `BACKPRESSURED`로 실패할 수 있습니다. 지원하지 않는 subject는
`ENOTSUP`를 반환합니다.

**반환값:** 성공 시 `ZLINK_HANDLER_OK`, 실패 시 `zlink_handler_result_t` 값. `zlink_errno()`는 진단용 내부 errno를 그대로 유지합니다.

**참고:** `zlink_send_part`, `zlink_send_part_rid`, `zlink_publish_part`

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
