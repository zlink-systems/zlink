---
title: "Socket 공통"
---

[English](https://zlink-systems.github.io/zlink/spec/core/socket/) | 한국어

<!-- zlink-nav:start -->
[Core 스펙 목차](../README.ko.md) | [이전: Runtime 경계](../08-runtime-boundary.ko.md) | [다음: PAIR](01-pair.ko.md)
<!-- zlink-nav:end -->

# Socket 공통

> **이 장이 정의하는 것** — 모든 socket type에 적용되는 공통 기반(옵션·API 형태)의
> 공개 계약. type별 세부사항은 각 socket 명세가 정의한다.

## 1. Socket 공통 개요

zlink의 [socket](../glossary.ko.md#socket)은 message를 주고받는 endpoint이며, 반드시 어떤
[Context](../glossary.ko.md#context)에 속한다. 이 문서는 모든 socket type에 적용되는 공통
기반 — 생성·연결·종료, 공통 옵션, 송수신 API의 형태와 thread 안전성 — 을 다룬다.
type별 명세(type 전용 옵션, data plane API, 동작 세부사항)는 별도 파일에 있다.

| socket type | 명세 |
|-----------|------|
| 01. PAIR | [pair.ko.md](01-pair.ko.md) |
| 02. PUB | [pub.ko.md](02-pub.ko.md) |
| 03. SUB | [sub.ko.md](03-sub.ko.md) |
| 04. XPUB | [xpub.ko.md](04-xpub.ko.md) |
| 05. XSUB | [xsub.ko.md](05-xsub.ko.md) |
| 06. DEALER | [dealer.ko.md](06-dealer.ko.md) |
| 07. ROUTER | [router.ko.md](07-router.ko.md) |
| 08. STREAM | [stream.ko.md](08-stream.ko.md) |

관련 계약의 소유 문서는 다음과 같다.

| 관련 계약 | 정의하는 문서 |
|---|---|
| type별 전용 옵션·data plane·동작 세부 | 위 표의 각 socket 명세 |
| Context 수명과 context 옵션 | [Context](../01-context.ko.md) |
| message lifecycle와 ownership | [Message](../02-message.ko.md) |
| Auto HWM budget 계산·admission | [Auto HWM](../systems/06-auto-hwm.ko.md) |
| 결과 enum 전체와 오류 표 | [Errors](../03-errors.ko.md) |

## 2. 스레드 안전성

공개 socket 핸들 API는 기본적으로 thread-safe하다. 다만 모든 API가 같은
비용 모델을 갖는 것은 아니다.

- `send`는 여러 thread에서 동시 호출을 허용하는 hot path다.
- `bind/connect/disconnect`, subscribe/unsubscribe, option/query, monitor는
  runtime에 호출 가능한 control path다. correctness는 보장되지만 실행
  순서는 내부 직렬화에 따라 결정될 수 있다.
- `close`는 fail-fast lifecycle gate를 사용한다. 다른 thread가 같은 핸들에서
  admitted API를 실행 중이면 `EBUSY`, close가 accepted된 뒤 새 API
  진입은 `ESHUTDOWN`이다.
- 예외는 소수만 남긴다. init-only 설정과 같은 `zlink_msg_t` instance의 동시
  공유는 기본 허용 범위
  밖이다.

## 3. Pull 수신과 completion 모델

Core가 application에 처리할 항목이 있음을 알리는 경로는 poller readiness와 pull receive다.
Core는 application notification callback을 호출하지 않는다.

| 받을 내용 | readiness | 내용을 꺼내는 함수 |
|---|---|---|
| 일반 DATA | `ZLINK_POLLIN` | socket 종류에 맞는 `*_recv_part()` |
| STREAM packet | `ZLINK_POLLIN` | `zlink_stream_recv_packet()` |
| REQUEST 완료와 SEND·REQUEST WRITABLE 대기 토큰 | `ZLINK_POLLCOMPLETION` (읽지 않은 WRITABLE record는 `ZLINK_POLLOUT`도 level로 유지) | `zlink_completion_recv()` |
| socket monitor event | `ZLINK_POLLIN` | `zlink_socket_monitor_recv()` |
| timer fire count | timer readiness | `zlink_timer_recv()` |

일반 DATA 수신 함수는 다음과 같이 나뉜다.

| 함수 | 사용하는 socket과 record |
|---|---|
| `zlink_recv_part()` | PAIR·DEALER의 DATA, RAW mode STREAM byte record |
| `zlink_router_recv_part()` | ROUTER의 DATA 또는 REQUEST |
| `zlink_subscribe_part()` | SUB·XSUB의 topic DATA |
| `zlink_xpub_recv_part()` | XPUB의 subscribe·unsubscribe event |

`ZLINK_POLLCOMPLETION`은 payload가 아니다. Poller wait는 completion을 제거하지 않으며
`zlink_poller_event_t`에 operation payload를 추가하지 않는다. 준비된 socket의 caller는
`zlink_completion_recv(..., ZLINK_RECV_FLAGS_DONTWAIT)`를 `ZLINK_RECV_NO_DATA`가 나올
때까지 호출해 queue를 비운다. `zlink_free_fn`은 zero-copy memory를 반납하는 함수이고
`zlink_thread_fn`은 application notification이 아니라 사용자 thread entry를 나타내는 공개 타입이다.

## 4. 타입과 상수

### Socket type

```c
typedef enum zlink_socket_type_t
{
    ZLINK_SOCKET_ANY    = 0,       // 예약된 wildcard 값 — 생성용 아님, 소비 API 없음
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

`ZLINK_SOCKET_ANY`는 예약된 wildcard 값이다. socket 생성에 사용하지 않으며 이를 소비하는
API도 없다. 실제 socket 생성에는 위에 표시된 정규화된 `ZLINK_SOCKET_*` 상수를 사용한다.

### 송신 flag

```c
typedef enum zlink_send_flags_t
{
    ZLINK_SEND_FLAGS_NONE     = 0,       // flag 없음; blocking 송신 동작
    ZLINK_SEND_FLAGS_DONTWAIT = 0x0001u  // non-blocking 모드; blocking 시 ZLINK_SUBMIT_BACKPRESSURED 반환
} zlink_send_flags_t;

#define ZLINK_DONTWAIT ZLINK_SEND_FLAGS_DONTWAIT  // 짧게 쓰는 공개 이름
```

### 수신 flag

```c
typedef enum zlink_recv_flags_t
{
    ZLINK_RECV_FLAGS_NONE     = 0,       // flag 없음; blocking 수신 동작
    ZLINK_RECV_FLAGS_DONTWAIT = 0x0001u  // non-blocking 수신; 수신할 message가 없으면 ZLINK_RECV_NO_DATA를 즉시 반환
} zlink_recv_flags_t;
```

`zlink_recv_part`, `zlink_subscribe_part`, socket별 `zlink_*_recv_part` 계열, 그리고
monitor `zlink_*_monitor_recv` 함수들이 이 flag를 사용한다.

### Message part flag

```c
typedef enum zlink_part_flag_t
{
    ZLINK_PART_FINAL = 0,  // 현재 part가 마지막
    ZLINK_PART_MORE = 1    // 같은 multipart message에 다음 part가 있음
} zlink_part_flag_t;
```

### rid 중복 정책

```c
typedef enum zlink_rid_duplicate_policy_t
{
    ZLINK_RID_DUPLICATE_REJECT = 0,   // 기존 pipe 유지, 새 중복 pipe를 등록하지 않음 (기본값)
    ZLINK_RID_DUPLICATE_HANDOVER = 1  // 같은 방향의 재연결 pipe가 기존 pipe를 인수
} zlink_rid_duplicate_policy_t;
```

`ZLINK_OPT_RID_DUPLICATE_POLICY`는 같은 local socket에 동일한 peer
routing id가 들어왔을 때의 정책을 정한다. 값은 `int`로 설정하며,
기본값은 `ZLINK_RID_DUPLICATE_REJECT`다.

`ZLINK_RID_DUPLICATE_REJECT`는 기존 pipe를 유지하고 새 중복 pipe를 등록하지
않으며, 등록하지 않은 중복 pipe는 즉시 닫는다. 따라서 connector는 그 pipe의 종료를
monitor로 관찰하고 connect intent에 따라 다시 연결하며, 기존 pipe가 종료된 뒤의 시도가
admission된다. 거부된 pipe에 이미 제출된 request는 `ZLINK_REQUEST_NOT_CONNECTED`(errno
`EHOSTUNREACH`)로 정확히 한 번 종결된다. Connector가 관찰하는 READY event는 transport 연결
성립을 뜻하며 peer ROUTER의 routing id admission을 뜻하지 않는다. `ZLINK_RID_DUPLICATE_HANDOVER`에서는 같은 방향에서 다시 연결한 pipe가
기존 pipe를 인수한다. 서로 반대 방향의 pipe가 충돌하면 두 peer의 routing id를
비교해 양쪽이 같은 방향 하나를 선택한다. 그 선택으로 물러나는 방향에서 이미 admit된
request는 선택된 방향으로 이어지지 않는다. 그 request의 reply는 submit 시점에 사용한 바로 그
transport pair로만 전달되도록 제한되어 있어 request를 완료하지 못한다. Core는 그 pair가
handover로 물러나는 즉시 해당 request를 `ZLINK_REQUEST_NOT_CONNECTED`(errno `EHOSTUNREACH`)로
정확히 한 번 종결하며, request의 자기 timeout까지 기다리지 않는다. Caller는 그 completion을
받은 뒤 handover된 방향으로 다시 보낸다.

이 옵션은 peer가 광고한 routing id를 관찰할 수 있는 socket에서만 의미가
있다. STREAM은 server가 연결별 4-byte routing id를 직접 만들기 때문에
이 옵션의 영향을 받지 않는다.

### 송신 재시도 모드

```c
typedef enum zlink_submit_retry_mode_t
{
    ZLINK_SUBMIT_RETRY_OFF = 0,           // 자동 재시도를 하지 않음
    ZLINK_SUBMIT_RETRY_LOCAL_FAILURE = 1  // peer queue에 넘기기 전의 local 실패만 재시도 가능
} zlink_submit_retry_mode_t;
```

`ZLINK_SUBMIT_RETRY_OFF`는 자동 재시도를 하지 않는다.
`ZLINK_SUBMIT_RETRY_LOCAL_FAILURE`는 송신을 peer queue에 넘기기 전에 발생한
local 실패만 재시도할 수 있음을 나타낸다. 이 모드는 peer 전달이나 처리
완료를 보장하지 않는다. 재시도 대상과 결과는 [§5 옵션의 송신 재시도](#송신-재시도)가
설명한다.

### 수신 flow state

```c
typedef enum zlink_receive_flow_state_t
{
    ZLINK_RECEIVE_FLOW_RUNNING = 0,  // 계속 보내라는 뜻
    ZLINK_RECEIVE_FLOW_PAUSED = 1    // 이 socket으로 새 message를 보내지 말라는 뜻
} zlink_receive_flow_state_t;
```

DEALER와 ROUTER socket이 자신에게 보내는 peer에게 알리는 receive-flow 상태다. Count `1`인
DEALER-DEALER와 DEALER-ROUTER는 single Application connection의 Core control 경로를 사용하고,
count `2`인 ROUTER-ROUTER는 [completion progress lane](../glossary.ko.md#completion-progress-lane)을 사용한다.
`ZLINK_RECEIVE_FLOW_RUNNING`은
계속 보내라는 뜻이고 `ZLINK_RECEIVE_FLOW_PAUSED`는 이 socket으로 새 message를 보내지
말라는 뜻이다. 이 값은 counter가 아니라 socket 전체에 적용되는 절대 상태이므로, 이미
유지하는 상태를 다시 설정하면 아무것도 바꾸지 않고 성공한다. Receive-flow는 DEALER와
ROUTER에서만 지원하며 결과 동작은 [DEALER](06-dealer.ko.md)와
[ROUTER](07-router.ko.md)가 소유한다.

### 송신 결과

```c
typedef enum zlink_submit_result_t
{
    /* Submit succeeded. */
    ZLINK_SUBMIT_OK = 0,                 // message가 성공적으로 송신됨

    /* Normal control-flow result. */
    ZLINK_SUBMIT_BACKPRESSURED = 1,      // 송신 queue가 가득 참 (HWM 도달)
    ZLINK_SUBMIT_NOT_CONNECTED = 2,      // 대상 경로나 peer가 아직 연결되지 않음
    ZLINK_SUBMIT_NOT_FOUND = 3,          // 대상 peer 또는 routed destination을 찾지 못함
    ZLINK_SUBMIT_NOT_ADMITTED = 13,      // target route는 식별했지만 handshake 또는 신규 outbound weight 같은 admission 정책이 submit을 거부함

    /* Runtime / lifecycle failure. */
    ZLINK_SUBMIT_TERMINATED = 4,         // context가 종료됨

    /* Caller contract violation. */
    ZLINK_SUBMIT_INVALID_HANDLE = 5,     // 핸들이 NULL이거나 유효하지 않음
    ZLINK_SUBMIT_INVALID_ARGUMENT = 6,   // API 계약에 맞지 않는 인자
    ZLINK_SUBMIT_NOT_SUPPORTED = 7,      // 지원하지 않는 작업 또는 flags
    ZLINK_SUBMIT_INVALID_STATE = 8,      // 핸들이 잘못된 상태에 있음
    ZLINK_SUBMIT_THREAD_VIOLATION = 9,   // 허용된 thread 모델을 위반함

    /* Internal failure. */
    ZLINK_SUBMIT_OUT_OF_MEMORY = 10,     // submit 준비 중 memory 할당 실패
    ZLINK_SUBMIT_SEQ_EXHAUSTED = 11,     // request sequence 공간이 소진됨
    ZLINK_SUBMIT_INTERNAL_ERROR = 12     // 내부 send/request/reply submit 오류
} zlink_submit_result_t;
```

send, request submit, reply submit API의 공개 결과를 정규화할 때
사용하는 기준 enum이다. exported C API는 이 enum을 직접 반환한다.
내부 구현 경로는 계속 상세 `errno`를 사용하고, exported API 경계에서 그
값을 이 공개 결과 계약으로 정규화한다.

### Completion result와 record

```c
typedef enum zlink_request_result_t
{
    /* Reply completed successfully. */
    ZLINK_REQUEST_OK = 0,                  // reply payload를 정상 수신함

    /* Completion failure visible to the requester. */
    ZLINK_REQUEST_TIMED_OUT       = 101,   // 설정된 시간 안에 reply가 도착하지 않음
    ZLINK_REQUEST_NOT_FOUND       = 102,   // 대상이 없어 error reply로 완료됨
    ZLINK_REQUEST_TERMINATED      = 103,   // terminal reply 전에 Context 또는 socket이 종료됨 (ETERM 또는 ESHUTDOWN)
    ZLINK_REQUEST_PROTOCOL_ERROR  = 104,   // reply metadata 또는 error reply payload가 잘못됨
    ZLINK_REQUEST_INTERNAL_ERROR  = 105,   // 더 세분화된 public bucket 없이 request completion이 실패함
    ZLINK_REQUEST_REJECTED        = 106,   // target이 request를 명시적으로 거부함
    ZLINK_REQUEST_CONFLICT        = 107,   // request가 현재 routing 또는 operation 상태와 충돌함
    ZLINK_REQUEST_BUSY            = 108,   // target이 바빠 지금은 request를 받을 수 없음
    ZLINK_REQUEST_NOT_CONNECTED   = 109,   // target에 대한 활성 연결 없음
    ZLINK_REQUEST_INVALID_ARGUMENT = 110,  // request에 잘못된 인자가 담김
    ZLINK_REQUEST_INVALID_STATE   = 111,   // target이 이 request를 거부하는 상태임
    ZLINK_REQUEST_NOT_SUPPORTED   = 112,   // target이 지원하지 않는 작업
    ZLINK_REQUEST_BACKPRESSURED   = 113    // non-blocking outbound admission이 capacity 부족으로 실패함
} zlink_request_result_t;
```

REQUEST completion 결과를 정규화할 때 사용하는 기준 enum이다.

```c
typedef uint64_t zlink_completion_id_t;
typedef uint64_t zlink_reply_token_t;  // DATA는 0, REQUEST는 nonzero

typedef enum zlink_completion_kind_t {
  ZLINK_COMPLETION_SEND = 1,     // ABI 보존 전용, Core는 이 kind를 발행하지 않음
  ZLINK_COMPLETION_REQUEST = 2,  // request의 reply·timeout·terminal 결과
  ZLINK_COMPLETION_WRITABLE = 3  // DONTWAIT SEND·REQUEST 대기 토큰의 target에 write credit이 생김
} zlink_completion_kind_t;

typedef enum zlink_send_complete_result_t {
  ZLINK_SEND_ADMITTED = 0,       // WRITABLE: 같은 target에 다시 submit할 수 있음
  ZLINK_SEND_TERMINAL = 202      // WRITABLE: target이 제거됨, send_terminal_errno에 사유가 있음
} zlink_send_complete_result_t;

typedef struct zlink_completion_t {
  uint32_t struct_size;                  // sizeof(zlink_completion_t)
  zlink_completion_kind_t kind;          // REQUEST 또는 WRITABLE
  zlink_completion_id_t completion_id;   // socket-local, 항상 nonzero; WRITABLE은 submit이 반환한 대기 토큰
  void *user_context;                    // submit 값을 그대로 돌려줌
  zlink_routing_id_t peer_rid;           // ROUTER·STREAM WRITABLE과 ROUTER REQUEST는 submit RID, 그 외 empty
  zlink_send_complete_result_t send_result; // WRITABLE에서만 사용; ADMITTED는 재submit 가능, TERMINAL은 target 제거
  int send_terminal_errno;               // WRITABLE TERMINAL에서만 사용, 그 외 0
  zlink_request_result_t request_result; // REQUEST에서만 사용
  zlink_msg_t *reply_parts;               // REQUEST payload, 없으면 NULL
  size_t reply_part_count;                // REQUEST payload part 수
} zlink_completion_t;
```

완료 ID는 REQUEST completion과 SEND·REQUEST 대기 토큰이 공유하는 socket-local correlation 값이다.
`0`은 SEND가 이미 admission됐거나 Core가 operation을 접수하지 않아 후속 completion이 없다는
뜻이다. `ZLINK_SUBMIT_OK`와 함께 반환하는 REQUEST의 nonzero ID는 admission된 request의 ID이며
REQUEST completion 한 건이 뒤따른다. SEND와 REQUEST `DONTWAIT FINAL`이
`ZLINK_SUBMIT_BACKPRESSURED`와 함께 반환하는 nonzero ID는 대기 토큰이며, 같은 ID를 가진
`ZLINK_COMPLETION_WRITABLE` record가 정확히 한 번 뒤따른다. Nonzero ID는 socket을
닫기 전까지 재사용하지 않으며 취소 handle이 아니다. 다음 nonzero ID를 만들 수 없으면 submit은
`ZLINK_SUBMIT_SEQ_EXHAUSTED`, `errno == EOVERFLOW`, ID `0`으로 실패한다.

### 보안 메커니즘

```c
#define ZLINK_NULL 0   // 보안 메커니즘 없음 (기본값)
#define ZLINK_PLAIN 1  // PLAIN 사용자명/비밀번호 인증
```

## 5. 옵션

socket 옵션은 type별 전용 enum과 함수를 사용한다. 공통 옵션은
`zlink_set_option()` / `zlink_get_option()`으로, socket 타입별 옵션은
`zlink_set_router_option()`, `zlink_set_dealer_option()`,
`zlink_set_pub_option()`, `zlink_set_sub_option()`,
`zlink_set_stream_option()` 등 전용 함수로 설정한다.
ROUTING_ID는 `zlink_set_routing_id()` / `zlink_get_routing_id()` 전용
함수를 사용한다. TLS server/client role의 표준 설정은 `zlink_set_tls_server()` /
`zlink_set_tls_client()`를 사용하고, `ZLINK_OPT_TLS_*`는 지원하는 raw network socket의 개별 TLS 값을
설정하거나 조회할 때만 사용한다. SUBSCRIBE/UNSUBSCRIBE는 `zlink_set_subscription()` /
`zlink_unset_subscription()` 전용 함수를 사용한다.

### 공통 옵션 (`zlink_option_t`)

```c
typedef enum zlink_option_t {
  ZLINK_OPT_AFFINITY                  = 0x3001,  // I/O thread affinity bitmask (uint64_t)
  ZLINK_OPT_RATE                      = 0x3003,  // multicast 전송률 (kbps, int)
  ZLINK_OPT_RECOVERY_IVL              = 0x3004,  // multicast 복구 간격 (ms, int)
  ZLINK_OPT_SNDBUF                    = 0x3005,  // kernel 송신 buffer 크기 (int; -1=OS 기본값 유지, 0 이상=OS에 크기 요청)
  ZLINK_OPT_RCVBUF                    = 0x3006,  // kernel 수신 buffer 크기 (int; -1=OS 기본값 유지, 0 이상=OS에 크기 요청)
  ZLINK_OPT_FD                        = 0x3007,  // file descriptor (zlink_fd_t, 읽기 전용)
  ZLINK_OPT_EVENTS                    = 0x3008,  // 이벤트 상태 bitmask (int, 읽기 전용)
  ZLINK_OPT_TYPE                      = 0x3009,  // socket type (int, 읽기 전용)
  ZLINK_OPT_LINGER                    = 0x300A,  // 종료 시 대기 (ms, int; -1=무한, 0=즉시)
  ZLINK_OPT_RECONNECT_IVL             = 0x300B,  // 초기 재연결 간격 (ms, int)
  ZLINK_OPT_BACKLOG                   = 0x300C,  // listener backlog (int)
  ZLINK_OPT_RECONNECT_IVL_MAX         = 0x300D,  // 최대 재연결 간격 (ms, int; 0=IVL만 사용)
  ZLINK_OPT_MAXMSGSIZE                = 0x300E,  // 최대 인바운드 message 크기 (int64_t; 양수=상한, 0 이하=무제한, 기본값 -1)
  ZLINK_OPT_SNDHWM                    = 0x300F,  // directional send pipe의 accounted byte HWM (uint64_t; 기본값 4,096,000, 0=무제한)
  ZLINK_OPT_RCVHWM                    = 0x3010,  // directional receive pipe의 accounted byte HWM (uint64_t; 기본값 4,096,000, 0=무제한)
  ZLINK_OPT_MULTICAST_HOPS            = 0x3011,  // multicast TTL (int)
  ZLINK_OPT_RCVTIMEO                  = 0x3012,  // 수신 timeout (ms, int; 기본 1000; 명시적으로 -1 설정 시 무한)
  ZLINK_OPT_SNDTIMEO                  = 0x3013,  // 송신 timeout (ms, int; 기본 1000; 명시적으로 -1 설정 시 무한)
  ZLINK_OPT_LAST_ENDPOINT             = 0x3014,  // binding된 endpoint (string, 읽기 전용)
  ZLINK_OPT_TCP_KEEPALIVE             = 0x3015,  // SO_KEEPALIVE (int; -1=OS, 0=off, 1=on)
  ZLINK_OPT_TCP_KEEPALIVE_CNT         = 0x3016,  // TCP_KEEPCNT (int; -1=OS 기본값)
  ZLINK_OPT_TCP_KEEPALIVE_IDLE        = 0x3017,  // TCP_KEEPIDLE (초, int; -1=OS 기본값)
  ZLINK_OPT_TCP_KEEPALIVE_INTVL       = 0x3018,  // TCP_KEEPINTVL (초, int; -1=OS 기본값)
  ZLINK_OPT_IMMEDIATE                 = 0x3019,  // 완료된 연결에만 message queue 사용 (int)
  ZLINK_OPT_IPV6                      = 0x301A,  // socket에서 IPv6 활성화 (int; 0=off, 양수=on, getter는 0/1 반환)
  ZLINK_OPT_CONFLATE                  = 0x301B,  // PUB/SUB에서 topic당 최신 message만 유지 (int; DEALER는 활성화 불가)
  ZLINK_OPT_TOS                       = 0x301C,  // IP Type-of-Service 값 (int)
  ZLINK_OPT_HANDSHAKE_IVL             = 0x301D,  // ZMTP handshake timeout (ms, int)
  ZLINK_OPT_BLOCKY                    = 0x301E,  // socket option API가 지원하지 않는 식별자 — 아래 설명 참조
  ZLINK_OPT_INVERT_MATCHING           = 0x3020,  // topic 매칭 반전 (int)
  ZLINK_OPT_CONNECT_TIMEOUT           = 0x3024,  // 연결 timeout (ms, int)
  ZLINK_OPT_TCP_MAXRT                 = 0x3025,  // 최대 TCP 재전송 timeout (ms, int)
  ZLINK_OPT_MULTICAST_MAXTPDU         = 0x3026,  // 최대 multicast TPDU 크기 (int)
  ZLINK_OPT_BINDTODEVICE              = 0x3027,  // network interface binding (string)
  ZLINK_OPT_TLS_CERT                   = 0x3028,  // PEM 인코딩 TLS 인증서 경로 (string)
  ZLINK_OPT_TLS_KEY                    = 0x3029,  // PEM 인코딩 TLS 개인 키 경로 (string)
  ZLINK_OPT_TLS_CA                     = 0x302A,  // PEM 인코딩 CA 인증서 번들 경로 (string)
  ZLINK_OPT_TLS_VERIFY                 = 0x302B,  // TLS peer 검증 활성화 (int; 0=off, 양수=on, getter는 0/1 반환)
  ZLINK_OPT_TLS_REQUIRE_CLIENT_CERT    = 0x302C,  // client 인증서 요구 (int; 0=off, 양수=on, getter는 0/1 반환)
  ZLINK_OPT_TLS_HOSTNAME               = 0x302D,  // SNI 및 인증서 검증용 hostname (string)
  ZLINK_OPT_TLS_TRUST_SYSTEM           = 0x302E,  // 시스템 CA 인증서 저장소 신뢰 (int; 0=off, 양수=on, getter는 0/1 반환)
  ZLINK_OPT_TLS_PASSWORD               = 0x302F,  // 개인 키 암호 (string)
  ZLINK_OPT_ZMP_METADATA               = 0x3030,  // ZMP metadata 첨부 on/off (int; 0=off, 양수=on, getter는 0/1 반환)
  ZLINK_OPT_TCP_NODELAY                = 0x3031,  // TCP_NODELAY 활성화 (int; 0=off, 양수=on, getter는 0/1 반환)
  ZLINK_OPT_RID_DUPLICATE_POLICY       = 0x3033,  // peer routing id 중복 정책 (int; 기본 REJECT — §4 rid 중복 정책 참조)
  ZLINK_OPT_SUBMIT_RETRY_MODE          = 0x3037,  // local submit 실패 재시도 모드 (int; ZLINK_SUBMIT_RETRY_OFF 또는 ZLINK_SUBMIT_RETRY_LOCAL_FAILURE, raw socket 기본값 off)
  ZLINK_OPT_SUBMIT_RETRY_TIMEOUT       = 0x3038,  // local submit 실패 재시도 예산 (ms, int; raw socket 기본값 0, 0이면 재시도 없음)
  ZLINK_OPT_SUBMIT_RETRY_ATTEMPTS      = 0x3039,  // 최초 submit 이후 추가 재시도 횟수 (int; raw socket 기본값 0, 현재 상한 16)
  ZLINK_OPT_PENDING_MAX_MSGS           = 0x303A,  // ABI 보존 전용 (uint64_t, 기본 0; 저장·반환만 하고 동작에 영향 없음)
  ZLINK_OPT_PENDING_MAX_BYTES          = 0x303B   // ABI 보존 전용 (uint64_t, 기본 0; 저장·반환만 하고 동작에 영향 없음)
} zlink_option_t;
```

`zlink_set_option()` / `zlink_get_option()`과 함께 사용하며,
raw socket과 discovery에 적용된다.

`ZLINK_OPT_PENDING_MAX_MSGS`와 `ZLINK_OPT_PENDING_MAX_BYTES`는 ABI 호환을 위해 set한 값을 저장하고
get으로 돌려줄 뿐 Core 동작에 영향을 주지 않는다. Core는 SEND와 REQUEST 어느 쪽의 payload도
admission 전에 보관하지 않으므로 두 option이 제한할 pending record가 없다. 기본값은 `0`이다.

두 option의 get/set은 PAIR·DEALER·ROUTER·STREAM에서만 지원하며, 네 type 모두 option 저장을
ABI로 유지한다. 다른 socket의 get/set은
`ZLINK_CONFIG_NOT_SUPPORTED`, `errno == ENOTSUP`로 실패하고 기존 option 상태를 바꾸지 않는다.

`ZLINK_OPT_BLOCKY`는 socket option API가 지원하지 않는 식별자다.
`zlink_set_option()`/`zlink_get_option()`은 `ZLINK_CONFIG_NOT_SUPPORTED`/`ENOTSUP`을
반환하며, context 종료 동작은 `ZLINK_CTX_OPT_BLOCKY`로 설정한다 (`int`, 0=off, 양수=on,
getter는 0/1 반환).

#### Conflation

`ZLINK_OPT_CONFLATE`는 PUB와 SUB에서 계속 활성화할 수 있고 getter가 `1`을 반환한다. DEALER에서
`1`을 설정하면 `ZLINK_CONFIG_NOT_SUPPORTED`와 `ENOTSUP`이고, `0` 설정은 no-op으로 성공하며
getter는 `0`을 반환한다.

DEALER는 같은 Application pipe로 Application record와 내부 protocol control을 전달한다.
Frame 단위 conflation은 두 종류를 함께 보존할 수 없어 최신 Application record 또는 필요한
control 중 하나를 유실할 수 있다. 따라서 DEALER는 부분적인 conflation을 제공하지 않는다.

#### Transport/Buffer

두 [HWM](../glossary.ko.md#hwm) `uint64_t` option(`ZLINK_OPT_SNDHWM`, `ZLINK_OPT_RCVHWM`)은
`zlink_set_option()`과 `zlink_get_option()`에서 정확히
`sizeof(uint64_t)` byte를 사용해야 한다. 4-byte 값은
`ZLINK_CONFIG_INVALID_ARGUMENT`로 거절한다. 제거된 socket option 값 `0x3034`도
알 수 없는 option이므로 `ZLINK_CONFIG_INVALID_ARGUMENT`와 `EINVAL`로 실패한다.
pipe admission은 실제로 보관한 byte를 계산한다.

HWM은 각 HWM-controlled application directional pipe에 적용한다. DEALER-ROUTER의 DATA·REQUEST·
REPLY·error reply는 single Application physical pipe에서 같은 HWM과 peer의 PAUSED 상태를
적용한다. ROUTER-ROUTER의 completion progress lane만 terminal reply와 error reply에 auto HWM,
manual `SNDHWM`·`RCVHWM`, LWM과 Core budget reservation을 적용하지 않는다. Accounted byte가
limit에 도달하면 receiver가 충분한 byte credit을
반환할 때까지 이후 write가 대기한다 — 이 제한 동작이
[backpressure](../glossary.ko.md#backpressure)다. 비어 있는 pipe에는
accounted 크기가 HWM보다 큰 message 한 건을 허용할 수 있다. 따라서 유효한 큰
message를 HWM이 작다는 이유만으로 모두 거절하지 않는다. 이 message도
`ZLINK_OPT_MAXMSGSIZE`를 만족해야 하며, 한 건을 허용한 뒤에는 이후 write가 대기한다.
`ZLINK_OPT_MAXMSGSIZE`가 무제한인 방향에서도 admission 시점에 전체 accounted 크기를 아는
complete message 한 건, 즉 single-part 또는 total-known message에만 이 예외를 적용한다.
최종 전체 크기를 모르는 incremental multipart에는 첫 `MORE` frame부터 일반 byte HWM을 적용하므로
frame이 제한 없이 누적되지 않는다. 이 예외를 위해 known-total metadata나 transaction 전체
reservation을 추가하지 않는다.

admission은 frame 단위로 charge한다. 일반 frame의 charge는 payload byte 수에
`sizeof(zlink_msg_t)`를 더한 값이므로 빈 frame도 비용이 0이 아니고, 작은 frame을 많이
보관한 pipe는 payload 합계보다 먼저 HWM에 도달한다. delimiter, join과 leave frame은
application payload가 없으므로 `sizeof(zlink_msg_t)` metadata 비용만 charge한다.
frame이 pipe에서 빠질 때 같은 charge를 되돌려 준다.

low water mark는 pipe가 대기 중인 writer에게 read credit을 돌려주는 byte 수준이다.
기본값은 해당 방향에 적용된 HWM의 `ceil(hwm_bytes / 2)`다. pipe는 low water mark
hint를 가질 수도 있다. hint는 그 기본값보다 낮을 때만 사용하며, 기본값 이상인 hint는
기본값을 그대로 둔다. HWM 이상인 hint는 `hwm_bytes - 1`로 clamp하고, clamp한 값이 `1`
미만이면 `1`로 만들므로 결과는 항상 `1 .. hwm_bytes - 1` 범위 안에 있다. hint `0`은
hint가 없다는 뜻이다. 무제한 HWM에는 low water mark가 없다.

Core는 보통 이 low water mark에서 credit을 묶어서 반환한다. sender가 실제 HWM에
도달한 경우에는 이미 읽힌 누적 byte를 직접 확인하고, 그 뒤 receiver가 현재 보이는 입력을
모두 읽으면 LWM 전에도 한 번 credit을 반환하고 대기 중인 writer를 깨울 수 있다. 이
복구는 HWM에 도달한 sender에만 적용하므로 낮은 queue depth의 정상 message마다
cross-thread command를 만들지 않는다. 기다리는 writer가 없는 pipe를 receiver가 비워도
wakeup을 보내지 않는다. 이 pipe 기준은 Framework의 receive 재개 기준과 별개다.

#### 송신 재시도

submit retry는 `ENOTCONN`, `EHOSTUNREACH` 또는 `ECONNREFUSED`로 분류되는 local
submit 실패만 짧게 다시 시도한다. locally initiated paired endpoint의 blocking
submit은 pair 검증이 끝날 때까지 이 연결 오류를 재시도 대상으로 처리한다. 대기
예산이나 시도 횟수가 끝나면 마지막 시도의 연결 실패 errno를 보존해 공개 result로
정규화한다. `ENOTCONN`과 `EHOSTUNREACH`는 `ZLINK_SUBMIT_NOT_CONNECTED`,
`ECONNREFUSED`는 `ZLINK_SUBMIT_NOT_ADMITTED`로 반환한다.
`ZLINK_DONTWAIT` 호출, backpressure(`EAGAIN`), admission 거절, 인자 오류, request
submit 성공 뒤의 reply timeout은 retry 대상이 아니다.

### 전용 함수 (옵션 enum이 아님)

- **Routing ID**: `zlink_set_routing_id()` / `zlink_get_routing_id()`
- **TLS**: `zlink_set_tls_server()` / `zlink_set_tls_client()`
- **Subscribe/Unsubscribe**: `zlink_set_subscription()` / `zlink_unset_subscription()`

## 6. 함수

### zlink_socket

socket을 생성한다.

```c
ZLINK_EXPORT void *zlink_socket (void *context_, zlink_socket_type_t type_);
```

지정된 context 내에서 새 socket을 생성한다. `type_` 매개변수는 messaging pattern을
선택한다. Raw socket은 [§3](#3-pull-수신과-completion-모델)의 pull 함수를 사용한다.
STREAM은 첫 successful bind 또는 connect 전에 RAW나 PACKET receive mode를 명시적으로
선택한다. Socket은 context가 종료되기 전에 `zlink_close()`로 닫아야 한다.

**반환값:** 성공 시 socket 핸들, 실패 시 `NULL` (errno가 설정됨).

**에러:** socket 타입이 유효하지 않으면 `EINVAL`. 최대 socket 수에 도달하면
`EMFILE`. Context가 종료된 경우 `ETERM`.

**스레드 안전성:** Context에 대해 스레드 안전하다.

**참고:** `zlink_close`, `zlink_ctx_new`

---

### zlink_recv_part

raw socket에서 message part 하나를 수신한다.

```c
ZLINK_EXPORT zlink_recv_result_t zlink_recv_part (void *s_,
                                                  const zlink_routing_id_t **source_rid_out_,
                                                  zlink_msg_t *part_out_,
                                                  zlink_part_flag_t *has_more_out_,
                                                  zlink_recv_flags_t flags_);
```

지원 타입은 raw `PAIR`, `DEALER`, `STREAM`이다. raw `PUB`, `XPUB`,
`SUB`, `XSUB`, `ROUTER`에는 사용할 수 없으며
`ZLINK_RECV_NOT_SUPPORTED`를 반환하고 `errno`를 `ENOTSUP`로 설정한다.
`part_out_`과 `has_more_out_`은 필수이고 `part_out_`은 호출 전에 초기화되어 있어야 한다.
`source_rid_out_`은 선택 사항이다. Successful receive는 기존 `part_out_` content를 닫고
새 part의 소유권을 caller에게 옮긴다. Caller는 다음 successful overwrite 전에 message를
옮기거나 `zlink_msg_close()`로 닫는다. `STREAM`은 Core가 소유한 routing ID view를 반환하고
PAIR와 DEALER는 `NULL`을 반환한다. `*has_more_out_`은 다음 part가 있으면
`ZLINK_PART_MORE`, 마지막 part이면 `ZLINK_PART_FINAL`이다.

한 multipart record의 첫 part부터 `FINAL`까지 같은 thread와 같은 recv family를 사용한다.
다른 thread나 family가 중간에 진입하면 `ZLINK_RECV_INVALID_STATE`, `errno == EBUSY`이고
원래 owner는 staged record를 계속 받을 수 있다. `flags_`는 `NONE` 또는 `DONTWAIT`만 허용한다.
알 수 없는 bit는 `ZLINK_RECV_INVALID_STATE`, `errno == EINVAL`이다.

`DONTWAIT`에 record가 없으면 즉시 `ZLINK_RECV_NO_DATA`, `errno == EAGAIN`이다. `NONE`은
호출 진입 시 `ZLINK_OPT_RCVTIMEO`를 snapshot한다. 기본값은 1,000 ms이고 `0`은 즉시,
`-1`은 무한 대기다. Timeout은 `ZLINK_RECV_NO_DATA`, `errno == EAGAIN`이다. Blocking wait
중 context termination은 `ZLINK_RECV_TERMINATED`, `errno == ETERM`, socket shutdown은
`ZLINK_RECV_INVALID_STATE`, `errno == ESHUTDOWN`이다. 모든 실패는 output과 message content를
변경하지 않는다.

반환한 RID view는 같은 socket의 다음 data recv API에 진입하거나 socket을 close할 때까지
유효하다. Poller wait, completion recv, monitor recv와 다른 socket의 data recv는 이 view를
무효화하지 않는다. 같은 socket의 다음 data recv는 성공 여부와 관계없이 진입 시 이전 view를
무효화한다. 더 오래 보관할 caller와 binding은 receive 직후 owned RID로 복사한다.

---

### Routed·subscription receive family

ROUTER DATA·REQUEST, SUB·XSUB topic DATA와 XPUB subscription event는 각각 전용 pull 함수로
받는다.

```c
ZLINK_EXPORT zlink_recv_result_t zlink_router_recv_part(
  void *router_,
  const zlink_routing_id_t **source_rid_out_,
  zlink_reply_token_t *reply_token_out_,
  zlink_msg_t *part_out_,
  zlink_part_flag_t *has_more_out_,
  zlink_recv_flags_t flags_);

ZLINK_EXPORT zlink_recv_result_t zlink_subscribe_part(
  void *sub_,
  const zlink_routing_id_t **source_rid_out_,
  char *topic_id_buf_,
  size_t topic_id_capacity_,
  size_t *topic_id_len_out_,
  zlink_msg_t *part_out_,
  zlink_part_flag_t *has_more_out_,
  zlink_recv_flags_t flags_);

ZLINK_EXPORT zlink_recv_result_t zlink_xpub_recv_part(
  void *xpub_,
  const zlink_routing_id_t **source_rid_out_,
  int *subscribed_out_,
  char *topic_id_buf_,
  size_t topic_id_capacity_,
  size_t *topic_id_len_out_,
  zlink_recv_flags_t flags_);
```

| 함수 | 필수 output | 선택 output | 성공 시 값 |
|---|---|---|---|
| `zlink_router_recv_part` | `source_rid_out_`, `reply_token_out_`, initialized `part_out_`, `has_more_out_` | 없음 | DATA token `0`, REQUEST의 모든 part에 같은 nonzero token |
| `zlink_subscribe_part` | `topic_id_len_out_`, initialized `part_out_`, `has_more_out_` | `source_rid_out_` | SUB·XSUB source는 `NULL`, topic byte는 NUL 없이 복사 |
| `zlink_xpub_recv_part` | `subscribed_out_`, `topic_id_len_out_` | `source_rid_out_` | subscribe `1`/unsubscribe `0`, peer RID와 topic byte |

필수 handle/output이 `NULL`이면 `ZLINK_RECV_INVALID_HANDLE`+`EFAULT`다. 알 수 없는 flags bit,
multipart owner가 아닌 thread·family의 진입은 각각 `ZLINK_RECV_INVALID_STATE`+`EINVAL`,
`ZLINK_RECV_INVALID_STATE`+`EBUSY`다. `NONE`의 timeout·종료와 DONTWAIT, part ownership,
실패 시 output 불변 및 borrowed RID 수명은 [`zlink_recv_part`](#zlink_recv_part)의 공통 규칙을
따른다. ROUTER의 DATA는 source logical RID와 token `0`, REQUEST는 같은 source RID와 Core가
만든 nonzero opaque reply token을 반환한다. Multipart REQUEST의 모든 part에 같은 RID와 token을
반복한다. Token은 wire sequence가 아니며 application은 이를 해석·생성·변경하지 않는다.

SUB·XSUB와 XPUB에서 `topic_id_capacity_`가 필요한 길이보다 작으면 필요한 길이만
`*topic_id_len_out_`에 쓰고 `ZLINK_RECV_BUFFER_TOO_SMALL`+`ENOBUFS`를 반환한다. Queue record와
다른 output은 그대로이므로 충분한 buffer로 재시도하면 같은 record를 정확히 한 번 받는다.
길이 0 topic은 capacity 0·NULL buffer로 성공하고 record를 소비한다. Positive capacity와 NULL
buffer는 실제 topic 길이와 관계없이 `ZLINK_RECV_INVALID_HANDLE`+`EFAULT`이며 비소비다.

Requester가 보낸 REQUEST의 reply는 어느 data recv 함수에도 나타나지 않고 REQUEST completion으로
queue에 들어간다. DEALER는 inbound typed REQUEST를 받거나 reply하는 socket이 아니다.

---

### zlink_close

socket을 닫고 자원을 해제한다.

```c
ZLINK_EXPORT zlink_close_result_t zlink_close (void *s_);
```

socket을 닫고 관련된 모든 자원을 해제한다. 송신 대기열에 남아 있는 message는
`ZLINK_OPT_LINGER` 설정에 따라 폐기되거나 송신된다. 공개 핸들은 계층적 계약을
따른다: hot-path send 작업은 여러 thread에서 동시 호출이 가능하고, 저빈도
제어 경로는 정확성을 위해 직렬화되며, close/destroy는 엄격한 lifecycle gate를
사용한다. 다른 thread에서 동일 핸들에 대해 API 호출이 진행 중이면 `errno=EBUSY`로
실패한다. close가 accepted된 뒤 새 API 진입은 `errno=ESHUTDOWN`으로 실패한다.
Close는 pending operation과 application이 아직 꺼내지 않은 completion·packet을 내부에서
정리한다. Caller가 결과나 payload를 필요로 하면 close 전에 queue를 비워야 한다.

**반환값:** 성공 시 `ZLINK_CLOSE_OK`, 실패 시 `zlink_close_result_t` 값. `zlink_errno()`는 진단용 내부 errno를 그대로 유지한다.

**에러:** pointer가 유효하지 않으면 `EFAULT`, opaque value가 stale 상태이면 `ESTALE`.
다른 작업이 진행 중이면 `EBUSY`.

**참고:** `zlink_socket`

---

### zlink_set_option

공통 옵션을 설정한다.

```c
ZLINK_EXPORT zlink_config_result_t zlink_set_option (void *handle_,
                      zlink_option_t option_,
                      const void *optval_,
                      size_t optvallen_);
```

공통 옵션을 설정한다. `handle_`은 raw socket 또는 discovery다.
`option_` 매개변수는 `zlink_option_t` enum 값이다. `optval_`
pointer는 값을 제공하고 `optvallen_`은 크기를 byte 단위로 지정한다.
`ZLINK_OPT_SNDHWM`과 `ZLINK_OPT_RCVHWM`은 정확한 `uint64_t` 값을 요구한다.

raw socket과 discovery의 설정 시점은 각 option 계약을 따른다.

**반환값:** 성공 시 `ZLINK_CONFIG_OK`, 실패 시 `zlink_config_result_t` 값. `zlink_errno()`는 진단용 내부 errno를 그대로 유지한다.

**에러:** 옵션을 알 수 없거나 값이 범위를 벗어나거나 byte-count option의 크기가
정확하지 않으면 `EINVAL`. Context가 종료된 경우 `ETERM`.

**참고:** `zlink_get_option`

---

### zlink_get_option

공통 옵션을 조회한다.

```c
ZLINK_EXPORT zlink_config_result_t zlink_get_option (void *handle_,
                      zlink_option_t option_,
                      void *optval_,
                      size_t *optvallen_);
```

공통 옵션의 현재 값을 가져온다. `handle_`은 raw socket 또는 discovery다. 두 HWM
byte-count option에는 `uint64_t` output buffer가 필요하고, 호출할 때
`*optvallen_`이 정확히 `sizeof(uint64_t)`여야 한다. 더 큰 임시 buffer나 이전
4-byte 크기를 포함해 그 밖의 크기는 값을 잘라 쓰거나 일부만 채우지 않고
`ZLINK_CONFIG_INVALID_ARGUMENT`와 `errno == EINVAL`로 실패한다. 성공하면
`*optvallen_`은 `sizeof(uint64_t)`를 유지한다.

**반환값:** 성공 시 `ZLINK_CONFIG_OK`, 실패 시 `zlink_config_result_t` 값. `zlink_errno()`는 진단용 내부 errno를 그대로 유지한다.

**참고:** `zlink_set_option`

---

### zlink_socket_set_receive_flow_state

이 socket의 receive-flow 상태를 설정하고 peer type별 Core control 경로로 동기화한다.

```c
ZLINK_EXPORT zlink_config_result_t zlink_socket_set_receive_flow_state (
  void *handle_, zlink_receive_flow_state_t state_);
```

`state_`를 socket 전체의 receive-flow 상태로 저장하고 모든 ready DEALER·ROUTER peer에게
보낸다. Count `1`인 DEALER-DEALER·DEALER-ROUTER에는 single Application connection의 Core
control 경로를, count `2`인 ROUTER-ROUTER에는 Completion connection을 사용한다. 이 호출은 socket을 소유한
runtime thread가 local 상태를 저장한 시점에 완료되며 peer가 관측할 때까지
기다리지 않는다. 현재 상태를 다시 설정하면 성공하고 새로 보내는 것은 없다.

**반환값:** 성공 시 `ZLINK_CONFIG_OK`이며 현재 상태를 다시 설정한 경우도
포함한다. DEALER·ROUTER가 아닌 socket 유형은 `ZLINK_CONFIG_NOT_SUPPORTED`를
반환하고 기존 byte HWM과 transport backpressure를 그대로 유지한다. 전체 결과
표는 [Errors](../03-errors.ko.md)가 소유한다.

**참고:** `zlink_monitor_status`

---

### zlink_set_routing_id

socket의 routing identity를 설정한다.

```c
ZLINK_EXPORT zlink_config_result_t zlink_set_routing_id (void *handle_,
                           const void *data_,
                           size_t size_);
```

raw socket의 routing ID를 설정한다. 길이는 1..255 bytes이며 값은 binary-safe하다. bind 또는 connect 전에
설정한다. 다른 핸들 종류는
`ZLINK_CONFIG_NOT_SUPPORTED`, `errno == ENOTSUP`이다. raw `STREAM`은 예외다 —
Core가 연결별 4-byte routing ID를 발급하므로 이 함수로 설정하면
`ZLINK_CONFIG_INVALID_ARGUMENT`, `errno == EINVAL`로 거절한다.
caller가 routing ID를 설정하지 않으면 Core는 socket 생성 시 RFC 4122 UUID v4 bit layout의 16-byte binary
routing ID를 발급한다. 이 기본값은 UUID 문자열이 아니라 raw 16 bytes다.

**반환값:** 성공 시 `ZLINK_CONFIG_OK`, 실패 시 `zlink_config_result_t` 값. `zlink_errno()`는 진단용 내부 errno를 그대로 유지한다.

**참고:** `zlink_get_routing_id`

---

### zlink_get_routing_id

socket의 routing identity를 조회한다.

```c
ZLINK_EXPORT zlink_config_result_t zlink_get_routing_id (void *handle_,
                           zlink_routing_id_t *out_);
```

raw socket에 설정되었거나 Core가 자동 발급한 routing ID를 caller-owned `zlink_routing_id_t`에 복사한다.

**반환값:** 성공 시 `ZLINK_CONFIG_OK`, 실패 시 `zlink_config_result_t` 값. `zlink_errno()`는 진단용 내부 errno를 그대로 유지한다.

**참고:** `zlink_set_routing_id`

---

### zlink_set_tls_server

서버 측 TLS를 구성한다.

```c
ZLINK_EXPORT zlink_config_result_t zlink_set_tls_server (void *handle_,
                           const char *cert_,
                           const char *key_,
                           int require_client_cert_);
```

server socket에 TLS 인증서, 개인 키를 설정하고, client 인증서 요구 여부를
지정한다.

이 함수는 TLS를 지원하는 raw server socket에 적용된다. 지원하지 않는 raw socket type과 다른 핸들은
`ZLINK_CONFIG_NOT_SUPPORTED`, `errno == ENOTSUP`이다.

**반환값:** 성공 시 `ZLINK_CONFIG_OK`, 실패 시 `zlink_config_result_t` 값. `zlink_errno()`는 진단용 내부 errno를 그대로 유지한다.

**참고:** `zlink_set_tls_client`, `zlink_bind`

---

### zlink_set_tls_client

클라이언트 측 TLS를 구성한다.

```c
ZLINK_EXPORT zlink_config_result_t zlink_set_tls_client (void *handle_,
                           const char *ca_cert_,
                           const char *hostname_,
                           int trust_system_);
```

client socket에 CA 인증서, hostname(SNI 및 인증서 검증용), 시스템 CA
저장소 신뢰 여부를 설정한다.

이 함수는 TLS를 지원하는 raw client socket에 적용된다. 지원하지 않는 raw socket type과 다른 핸들은
`ZLINK_CONFIG_NOT_SUPPORTED`, `errno == ENOTSUP`이다.

**반환값:** 성공 시 `ZLINK_CONFIG_OK`, 실패 시 `zlink_config_result_t` 값. `zlink_errno()`는 진단용 내부 errno를 그대로 유지한다.

**참고:** `zlink_set_tls_server`, `zlink_connect`

---

### zlink_bind

socket을 주소에 binding한다.

```c
ZLINK_EXPORT zlink_bind_result_t zlink_bind (void *s_, const char *addr_);
```

socket을 local endpoint에 binding한다. endpoint 문자열은
`transport://address` 형식을 사용하며, 지원되는 `transport`는 다음과 같다:

- `tcp://interface:port` 또는 `tcp://*:port`
- `inproc://name` (process 내 직접 연결, in-process transport)
- `ipc://pathname` (process 간, POSIX 전용)
- `ws://interface:port` (WebSocket)
- `tls://interface:port` (TLS 암호화 TCP)

socket은 여러 endpoint에 binding할 수 있다. TCP의 경우 port 0을 지정하면
시스템이 임시 port를 할당한다. 실제 endpoint를 가져오려면
`ZLINK_OPT_LAST_ENDPOINT`를 사용한다.

**반환값:** 성공 시 `ZLINK_BIND_OK`, 실패 시 `zlink_bind_result_t` 값. `zlink_errno()`는 진단용 내부 errno를 그대로 유지한다.

**에러:** 주소가 이미 사용 중이면 `EADDRINUSE`. interface가 존재하지 않으면
`EADDRNOTAVAIL`. `transport`가 지원되지 않으면 `EPROTONOSUPPORT`.

**참고:** `zlink_connect`, `zlink_unbind`

---

### zlink_connect

socket을 원격 주소에 연결한다.

```c
ZLINK_EXPORT zlink_connect_result_t zlink_connect (void *s_, const char *addr_);
```

socket을 원격 endpoint에 연결한다. endpoint 형식은 `zlink_bind()`와
동일하다. socket은 여러 endpoint에 연결할 수 있으며, peer가 사용 불가능해지면
library가 자동으로 재연결을 처리한다.

**반환값:** 성공 시 `ZLINK_CONNECT_OK`, 실패 시 `zlink_connect_result_t` 값. `zlink_errno()`는 진단용 내부 errno를 그대로 유지한다.

**참고:** `zlink_bind`, `zlink_disconnect`

---

### zlink_unbind

socket의 주소 binding을 해제한다.

```c
ZLINK_EXPORT zlink_connect_result_t zlink_unbind (void *s_, const char *addr_);
```

이전에 설정된 binding을 제거한다.

**반환값:** 성공 시 `ZLINK_CONNECT_OK`, 실패 시 `zlink_connect_result_t` 값. `zlink_errno()`는 진단용 내부 errno를 그대로 유지한다.

**참고:** `zlink_bind`

---

### zlink_disconnect

socket의 원격 주소 연결을 해제한다.

```c
ZLINK_EXPORT zlink_connect_result_t zlink_disconnect (void *s_, const char *addr_);
```

이전에 설정된 연결을 제거한다.

**반환값:** 성공 시 `ZLINK_CONNECT_OK`, 실패 시 `zlink_connect_result_t` 값. `zlink_errno()`는 진단용 내부 errno를 그대로 유지한다.

**참고:** `zlink_connect`

---

### zlink_disconnect_rid

socket에 연결된 peer를 routing id로 찾아 종료한다.

```c
ZLINK_EXPORT zlink_connect_result_t zlink_disconnect_rid (
  void *s_,
  const zlink_routing_id_t *peer_rid_);
```

`peer_rid_`는 비어 있으면 안 된다. 성공하면 해당 peer pipe는 비동기
종료 절차에 들어간다. 성공 반환은 remote peer가 종료 이벤트를 이미
처리했다는 뜻이 아니다.

ROUTER와 STREAM은 routing map을 사용해 대상을 찾는다. STREAM에서는
`peer_rid_`가 반드시 4-byte 연결 routing id여야 한다. 그 외 socket은
현재 연결된 pipe의 source routing id snapshot에서 일치하는 peer를 찾는다.
동일한 routing id가 둘 이상이면 대상을 확정할 수 없으므로 실패한다.

**반환값:** 성공 시 `ZLINK_CONNECT_OK`. 대상 없음은
`ZLINK_CONNECT_NOT_FOUND`, 중복 routing id는 `ZLINK_CONNECT_CONFLICT`,
lifecycle 소유권 충돌은 `ZLINK_CONNECT_BUSY`다. `zlink_errno()`는
진단용 내부 errno를 그대로 유지한다.

**참고:** `zlink_disconnect`, `ZLINK_OPT_RID_DUPLICATE_POLICY`

---

### Part send와 pending admission

PAIR·DEALER처럼 Core가 논리 target을 고르는 socket은 `zlink_send_part()`를 사용한다.
ROUTER·STREAM처럼 caller가 routing ID를 지정하는 socket은 `zlink_send_part_rid()`를 사용한다.
물리 connection ID나, 같은 방향 queue를 다시 만들 때 이전 것과 구분하는
[generation](../glossary.ko.md#generation)은 public target이 아니다. PUB·XPUB의
`zlink_publish_part()`는 completion 대상이 아니다.

```c
ZLINK_EXPORT zlink_submit_result_t zlink_send_part(
  void *s_, zlink_msg_t *part_, zlink_send_flags_t flags_,
  zlink_part_flag_t part_flag_, void *user_context_,
  zlink_completion_id_t *completion_id_out_);

ZLINK_EXPORT zlink_submit_result_t zlink_send_part_rid(
  void *s_, const zlink_routing_id_t *target_rid_, zlink_msg_t *part_,
  zlink_send_flags_t flags_, zlink_part_flag_t part_flag_,
  void *user_context_, zlink_completion_id_t *completion_id_out_);
```

두 함수는 결과와 관계없이 `part_`를 소비해 빈 initialized 상태로 둔다. `MORE`는
socket-local sequence에 part를 staging하고 `FINAL`이 성공해야 record 하나로 admission한다.
같은 sequence의 함수 family, target과 flags는 같아야 한다. 중간 실패는 staging한 prefix와
실패한 part를 모두 폐기한다. 재시도할 caller는 첫 part를 제출하기 전에 전체 record를 따로
보관해야 한다.

`flags_`는 `NONE` 또는 `DONTWAIT`, `part_flag_`는 `MORE` 또는 `FINAL`만 허용한다. 범위 밖
값과 알 수 없는 bit는 sequence 전체를 폐기하고 `ZLINK_SUBMIT_INVALID_ARGUMENT`,
`errno == EINVAL`로 실패한다. `completion_id_out_`은 선택 output이며 non-NULL이면 다른
validation 전에 `0`으로 초기화한다. `user_context_`는 `DONTWAIT FINAL`에서만 non-NULL을
허용한다. `MORE`나 `NONE FINAL`의 non-NULL context는 전체 sequence를 폐기하고
`ZLINK_SUBMIT_INVALID_ARGUMENT`, `errno == EINVAL`이다. Core는 context pointer를 읽거나
해제하지 않으며 caller는 completion을 receive·close하거나 socket을 폐기할 때까지 pointee의
수명을 유지한다.

| 호출 결과 | submit 반환 | 완료 ID | 후속 completion |
|---|---|---:|---|
| `MORE` staging 성공 | `ZLINK_SUBMIT_OK` | 0 | 없음 |
| `NONE FINAL` local send queue admission | `ZLINK_SUBMIT_OK` | 0 | 없음 |
| `DONTWAIT FINAL` 즉시 admission | `ZLINK_SUBMIT_OK` | 0 | 없음 |
| `DONTWAIT FINAL` backpressure 또는 target 준비 전 | `ZLINK_SUBMIT_BACKPRESSURED`, `EAGAIN` | nonzero 대기 토큰 | WRITABLE 한 건 |
| ROUTER·STREAM RID에 route 없음 | `ZLINK_SUBMIT_NOT_CONNECTED`, `EHOSTUNREACH` | 0 | 없음 |
| completion reservation 상한 초과 | `ZLINK_SUBMIT_OUT_OF_MEMORY`, `ENOMEM` | 0 | 없음 |
| validation·target 실패 | 해당 submit result | 0 | 없음 |

`NONE FINAL`은 호출 진입 시 `ZLINK_OPT_SNDTIMEO`를 snapshot하고 local send queue admission까지
기다린다. 기본값은 1,000 ms, `0`은 즉시, `-1`은 무한 대기다. 만료하면
`ZLINK_SUBMIT_BACKPRESSURED`, `errno == EAGAIN`, ID `0`, completion 없음으로 실패한다.
`DONTWAIT FINAL`은 기다리지 않고 admission을 한 번만 시도한다. 즉시 admission되면 ID `0`이고
completion이 없다. HWM·byte credit·flow pause에 의한 backpressure이거나 target이 존재하지만 아직
준비되지 않은 경우(transport pair 미준비, peer weight 0, connect 직후 peer가 0개인 DEALER)에는
`ZLINK_SUBMIT_BACKPRESSURED`, `errno == EAGAIN`과 함께 nonzero 대기 토큰을 `completion_id_out_`에
반환한다. Core는 토큰, target과 `user_context_`만 유지하고 payload는 보관하지 않는다. Part는 다른
결과와 같이 소비·폐기되므로 caller는 자신이 보관한 복사본으로 다시 제출한다. ROUTER·STREAM에서
지정한 RID에 route가 전혀 없으면(ROUTER는 `ZLINK_ROUTER_OPT_MANDATORY`가 양수일 때, 기본값) 즉시
`ZLINK_SUBMIT_NOT_CONNECTED`, `errno == EHOSTUNREACH`, ID `0`이며 토큰을 만들지 않는다. 두 경로의
실패한 `FINAL`은 staging한 prefix와 함께 소비·폐기한다.

REQUEST completion과 대기 토큰은 socket당 65,536개의 unified completion reservation을 공유한다.
SEND는 `DONTWAIT FINAL`이 대기 토큰을 반환할 때만 slot을 예약하고, REQUEST `FINAL`은 admission되어
nonzero REQUEST ID를 반환할 때와 대기 토큰을 반환할 때 예약한다. Slot은 reservation부터
`zlink_completion_recv()`가 record를 queue에서 제거할 때까지 유지한다. Socket close가 unread
record를 정리하면 함께 해제한다. 상한이 차면 Core는 operation을 접수하지 않고 전체 sequence를
소비·폐기한다. 이때 SEND `DONTWAIT FINAL`은
`ZLINK_SUBMIT_OUT_OF_MEMORY`, `errno == ENOMEM`, ID `0`이고 REQUEST `FINAL`은
`ZLINK_SUBMIT_BACKPRESSURED`, `errno == EAGAIN`, ID `0`이다.

대기 토큰의 target에 write credit이 다시 생기면 Core는 `ZLINK_COMPLETION_WRITABLE` record 한 건을
completion queue에 넣는다. Record의 `completion_id`는 토큰, `user_context`는 submit 값,
`send_result`는 `ZLINK_SEND_ADMITTED`, `send_terminal_errno`는 `0`이며 `peer_rid`는 ROUTER·STREAM에서
submit RID, PAIR·DEALER에서 empty다. 이 record를 아직 꺼내지 않은 동안 socket의 `ZLINK_POLLOUT`과
`ZLINK_POLLCOMPLETION`은 모두 level로 true다. Application은 `zlink_completion_recv()`를 `NO_DATA`까지
반복해 queue를 비운 뒤 같은 record를 `DONTWAIT`로 다시 제출한다. 토큰 하나는 정확히 WRITABLE
record 하나를 만든다. 재submit도 admission을 한 번만 시도하며 다시 backpressure이면 새 토큰을
반환한다. `ZLINK_POLLOUT`은 socket 전체의 재시도 가능성을 나타내는 aggregate hint이고, target별
정확한 신호는 WRITABLE record의 토큰·context·RID다.

Target 단위는 PAIR은 socket의 단일 pipe, DEALER는 candidate peer 집합, ROUTER·STREAM은 지정한 RID
하나다. DEALER는 candidate 중 하나가 열리면 WRITABLE 한 건을 만들고 재submit 시 열린 peer를 다시
선택하며 `FINAL`에서 endpoint를 고정하지 않는다. ROUTER·STREAM은 다른 RID의 credit으로 해당 토큰을
발행하지 않는다. WRITABLE을 발행하는 wake edge는 peer drain으로 LWM 아래 도달·credit refill,
pipe attach(connect 완료), peer weight 0 → 양수, ROUTER route 채택·standby 승격, flow RESUME이다.
Core는 SEND·REQUEST payload를 admission 전에 보관하지 않으며 Core 소유의 재시도 FIFO도 없다.
일시적인 transport 종료는 대기 토큰이나 진행 중인 `NONE FINAL` wait의 terminal 결과가 아니다.
`NONE`은 토큰을 만들지 않고 snapshot한 `SNDTIMEO` 안에서 같은 target의 reconnect와 admission을
기다린다.

대기 토큰은 다음 세 경우로만 종료된다. (a) 위의 WRITABLE record. (b) target의 명시적
제거(`zlink_disconnect_rid`, 해당 RID의 endpoint termination)로 `send_result == ZLINK_SEND_TERMINAL`,
`send_terminal_errno == ENOENT`인 WRITABLE record. (c) socket close·context termination으로
`ZLINK_SEND_TERMINAL`과 lifecycle errno(`ESHUTDOWN` 또는 `ETERM`)인 WRITABLE record. Peer weight가
0으로 떨어져도 대기 토큰은 종료되지 않는다. 아직 반환하지 않은 `NONE` wait는 target 제거 시
`ZLINK_SUBMIT_NOT_FOUND`+`ENOENT`, peer-type 거절 시 `ZLINK_SUBMIT_NOT_ADMITTED`+`EPROTOTYPE`, context termination 시
`ZLINK_SUBMIT_TERMINATED`+`ETERM`, socket shutdown 시
`ZLINK_SUBMIT_TERMINATED`+`ESHUTDOWN`으로 동기 종료한다. Admission 전 allocation failure는
`ZLINK_SUBMIT_OUT_OF_MEMORY`+`ENOMEM`, 다른 runtime failure는
`ZLINK_SUBMIT_INTERNAL_ERROR`+`EIO`다. 모두 ID `0`, completion 없음이며 전체 sequence를
소비·폐기한다.

ID `0`으로 admission된 뒤에는 payload가 기존 transport 전달 계약으로 넘어간다.
Core는 application record의 별도 복사본, delivery ACK나 deduplication sequence를 만들지 않으며,
그 뒤 disconnect가 발생해도 새 connection에 같은 application record를 replay하지 않는다.
WRITABLE record의 `ZLINK_SEND_ADMITTED`는 target에 다시 submit할 수 있다는 뜻이지 payload
admission이나 peer 수신 확인이 아니다.

### Request와 reply

DEALER는 Core가 선택한 ROUTER logical route로 request하고, ROUTER는 지정한 ROUTER RID로
request한다. Responder ROUTER는 receive에서 얻은 source RID와 opaque reply token으로 reply한다.

```c
ZLINK_EXPORT zlink_submit_result_t zlink_request_part(
  void *s_, const zlink_routing_id_t *target_router_rid_or_null_,
  zlink_msg_t *part_, zlink_send_flags_t flags_,
  zlink_part_flag_t part_flag_, uint32_t timeout_ms_,
  void *user_context_, zlink_completion_id_t *completion_id_out_);

ZLINK_EXPORT zlink_submit_result_t zlink_reply_part(
  void *router_, const zlink_routing_id_t *source_rid_,
  zlink_reply_token_t reply_token_, zlink_msg_t *part_,
  zlink_part_flag_t part_flag_);
```

DEALER의 target은 반드시 `NULL`, ROUTER의 target은 반드시 non-NULL이다. 다른 socket은
`ZLINK_SUBMIT_NOT_SUPPORTED`, `errno == ENOTSUP`다. ROUTER가 DEALER RID로 typed request를
보내면 `ZLINK_SUBMIT_NOT_ADMITTED`, `errno == EPROTOTYPE`이며 같은 RID의 일반 DATA 송신은
허용한다. RID가 routing map에 없으면 `NONE`은 `ZLINK_SUBMIT_NOT_FOUND`, `errno == ENOENT`이고
`DONTWAIT`은 `ZLINK_SUBMIT_NOT_CONNECTED`, `errno == EHOSTUNREACH`이며 토큰을 만들지 않는다.

Request `MORE`는 `timeout_ms_ == 0`, `user_context_ == NULL`로 호출한다. 이를 어기면 전체
sequence를 폐기하고 `ZLINK_SUBMIT_INVALID_ARGUMENT`, `errno == EINVAL`이다. Optional ID
output은 다른 validation 전에 `0`이 되며 `MORE`와 대기 토큰 없는 submit 실패는 `0`을 유지한다.
Admission된 `FINAL`(`ZLINK_SUBMIT_OK`)은 output 생략 여부와 관계없이 nonzero REQUEST ID를 만들고
정확히 한 REQUEST completion을 queue에 넣는다. Request `FINAL`의 context는 `NONE`과 `DONTWAIT`
모두에서 허용하며 같은 completion에 그대로 들어간다. Core는 pointer를 읽거나 해제하지 않으며
caller는 completion을 receive·close하거나 socket을 폐기할 때까지 pointee 수명을 유지한다. 대기
토큰을 반환한 submit은 WRITABLE record에 같은 context를 돌려주고, 그 밖의 submit 실패에는 context
echo가 없으므로 caller는 반환 직후 자신의 context state를 정리할 수 있다.

Core는 request를 wire에 공개하기 전에 completion ID와 공유 slot을 확보한다. Slot 포화는 flags와
무관하게 즉시 `ZLINK_SUBMIT_BACKPRESSURED`, `errno == EAGAIN`, ID `0`, completion 없음이다.
`NONE FINAL`은 slot과 ID를 임시 예약한 뒤 `SNDTIMEO` 안에서 outbound local admission을 기다린다.
Admission 전 실패는 reservation을 반납하고 [part send](#part-send와-pending-admission)의 동기
result·errno, ID `0`, completion 없음으로 끝난다.

`DONTWAIT FINAL`은 admission을 한 번만 시도하며 admission 전에 Core가 request record를 소유하는
상태는 없다. 즉시 admission되면 `ZLINK_SUBMIT_OK`와 nonzero REQUEST ID를 반환한다. HWM·byte
credit·flow pause에 의한 backpressure이거나 target이 존재하지만 아직 준비되지 않은 경우(transport
pair 미준비, peer weight 0, connect 직후 peer가 0개인 DEALER)에는 REQUEST ID 대신
`ZLINK_SUBMIT_BACKPRESSURED`, `errno == EAGAIN`과 함께 nonzero 대기 토큰을 `completion_id_out_`에
반환한다. 이 토큰은 SEND 대기 토큰과 같은 payload-free 토큰이다. Core는 토큰, target과
`user_context_`만 유지하고 request payload는 보관하지 않으며 reply timeout을 시작하지 않는다.
Part는 소비·폐기되므로 caller는 보관한 복사본으로 같은 request를 다시 제출한다. Target에 write
credit이 생기면 같은 토큰·context와 ROUTER의 submit RID를 담은 `ZLINK_COMPLETION_WRITABLE`
record(`send_result == ZLINK_SEND_ADMITTED`)가 정확히 한 건 뒤따르고, caller는 queue를 `NO_DATA`까지
비운 뒤 같은 request를 `DONTWAIT`로 다시 제출한다. 재제출도 admission을 한 번만 시도하며 다시
거절되면 새 토큰을 받는다. 토큰의 target 단위, wake edge, `ZLINK_POLLOUT`·`ZLINK_POLLCOMPLETION`
level 유지와 종료 조건(WRITABLE record, 명시적 target 제거의 `ZLINK_SEND_TERMINAL`+`ENOENT`, socket
close·context termination의 `ZLINK_SEND_TERMINAL`+lifecycle errno)은
[part send](#part-send와-pending-admission)의 SEND 대기 토큰과 같다. Mandatory ROUTER route가 없는
RID는 즉시 `ZLINK_SUBMIT_NOT_CONNECTED`, `errno == EHOSTUNREACH`, ID `0`이며 토큰을 만들지 않는다.

`timeout_ms_ == 0`은 requester socket의 request timeout을 snapshot하며 기본값은 5,000 ms다.
Reply timeout은 request record가 outbound local send queue에 admission된 시점, 즉
`ZLINK_SUBMIT_OK`를 반환한 시점부터 monotonic하게 흐른다. 대기 토큰이 유지되는 동안은 timeout이
흐르지 않는다. Admission 뒤 disconnect가 발생해도 request
payload를 replay하지 않고 correlation과 이미 시작한 budget만 유지한다. Reply와 timeout 중
pending correlation을 먼저 제거한 하나만 completion을 만들며 늦은 결과는 버린다.

DEALER-ROUTER single connection에서 ROUTER가 먼저 보낸 DATA와 이후 REPLY·error reply는 같은
FIFO를 사용한다. DEALER가 앞선 DATA를 dequeue하지 않거나 local PAUSED가 유지되면 REPLY는
앞지르지 못하며 request timeout이 먼저 terminal completion을 만들 수 있다.

`zlink_reply_part()`는 flags, timeout, context와 completion ID가 없는 synchronous admission
함수다. 모든 호출은 `part_`를 소비한다. 첫 `MORE` 또는 `FINAL`에서 RID·token과 REQUEST
complete 상태를 검증하고 token을 해당 reply sequence에 checkout한다. `MORE`는 staging과 checkout을
유지한다. `FINAL`은 `SNDTIMEO`를 snapshot해 같은 logical source RID의 reply route admission을
기다린다. Source peer가 DEALER이면 현재 ready Application pipe를, ROUTER이면 현재 ready Completion
pipe를 사용한다. Successful `FINAL`만 token을 소비한다.

Reply wait 만료는 `ZLINK_SUBMIT_BACKPRESSURED`+`EAGAIN`, allocation failure는
`ZLINK_SUBMIT_OUT_OF_MEMORY`+`ENOMEM`, 다른 runtime failure는
`ZLINK_SUBMIT_INTERNAL_ERROR`+`EIO`, context termination은
`ZLINK_SUBMIT_TERMINATED`+`ETERM`, socket shutdown은
`ZLINK_SUBMIT_TERMINATED`+`ESHUTDOWN`이다. RID 제거, 없는·소비된·RID 불일치 token은
`ZLINK_SUBMIT_NOT_FOUND`+`ENOENT`, REQUEST `FINAL` 전 reply는
`ZLINK_SUBMIT_INVALID_STATE`+`EBUSY`다. 실패한 sequence는 staging과 checkout을 정리하지만
RID와 socket lifecycle이 유지되는 token은 caller가 보관한 전체 reply로 처음부터 재시도할 수 있다.
같은 token의 두 번째 sequence는 `ZLINK_SUBMIT_INVALID_STATE`+`EBUSY`로 그 call의 part만
소비하고 첫 sequence를 유지한다. 진행 중인 sequence의 후속 part가 다른 RID·token을 사용하면
`ZLINK_SUBMIT_INVALID_ARGUMENT`+`EINVAL`로 original sequence를 폐기하고 checkout을 해제한다.

Reply token은 `(responder ROUTER socket, source logical RID)` 범위의 opaque nonzero capability다.
Application은 값을 해석·생성·변경하지 않는다. Physical disconnect, generation 변경과 requester
timeout은 token을 무효화하지 않는다. Successful reply `FINAL`, logical RID 제거, responder socket
close와 context termination만 token을 무효화한다. Public abandon·cancel API는 없다. Responder
application은 받은 REQUEST를 successful reply `FINAL`로 닫고 payload가 필요 없으면 길이 0
message 하나를 유효한 reply로 보낸다. 첫 `MORE` 뒤 FINAL을 제출하지 않거나 token을 버리면
checkout·staging·slot은 logical RID 제거 또는 responder socket close까지 남는다.
Responder ROUTER의 live token registry는 socket당 65,536개다. 포화하면 새 REQUEST를 application queue로
꺼내지 않고 해당 source pipe의 read·credit을 멈춘다. 다른 pipe의 DATA와 이미 admission된
record는 진행할 수 있지만 같은 pipe에서 REQUEST 뒤의 DATA는 앞지르지 않는다. Slot이 해제되면
paused pipe를 round-robin으로 다시 진행하며 token을 자동 제거하거나 REQUEST를 버리지 않는다.

### Completion pull과 ownership

REQUEST completion과 SEND·REQUEST WRITABLE record는 socket-local completion queue 하나를 사용한다.

이 public completion queue는 transport의 Completion connection과 다른 개념이다.
DEALER-ROUTER reply는 single Application connection의 physical head에 도달한 뒤 이 queue로
이동하고, ROUTER-ROUTER reply는 별도 Completion connection에서 이 queue로 이동한다.

```c
ZLINK_EXPORT zlink_recv_result_t zlink_completion_recv(
  void *s_, zlink_completion_t *completion_out_, zlink_recv_flags_t flags_);

ZLINK_EXPORT void zlink_completion_close(zlink_completion_t *completion_);
```

Caller는 output을 0으로 초기화하고 `struct_size = sizeof(zlink_completion_t)`를 설정한다.
Empty output은 `struct_size`를 제외한 모든 public member가 field별로 0·empty·NULL인 aggregate다.
Padding byte는 비교하지 않는다. 잘못된 `struct_size`나 non-empty output은
`ZLINK_RECV_INVALID_STATE`, `errno == EINVAL`이며 record를 제거하거나 기존 content를 바꾸지
않는다. NULL socket이나 output은 `ZLINK_RECV_INVALID_HANDLE`, `errno == EFAULT`다. `NO_DATA`와
다른 실패는 호출 시 empty였던 output을 empty로 유지한다.

한 successful receive는 REQUEST나 WRITABLE 한 종류만 반환하고 사용하지 않는 field를
0·empty·NULL로 둔다. `peer_rid`는 reservation 시점의 logical peer snapshot이다. PAIR·DEALER
WRITABLE과 DEALER REQUEST에서는 empty이고, ROUTER·STREAM WRITABLE과 ROUTER REQUEST에서는 submit
RID다. Reconnect 뒤 physical connection identity로 바뀌지 않으며 후속 send target capability가
아니다.

| 원인 | WRITABLE completion (SEND·REQUEST 대기 토큰) | REQUEST completion |
|---|---|---|
| target write credit 회복 또는 유효 reply | `ZLINK_SEND_ADMITTED`, errno 0 | `ZLINK_REQUEST_OK` 또는 wire error-reply mapping |
| Request reply timeout | 해당 없음 | `ZLINK_REQUEST_TIMED_OUT` |
| endpoint 또는 logical RID 명시적 제거 | `ZLINK_SEND_TERMINAL`, `ENOENT` | `ZLINK_REQUEST_NOT_FOUND` |
| 영구적인 peer-type 거절 | 해당 없음; 토큰은 target 제거까지 유지 | `ZLINK_REQUEST_REJECTED` |
| malformed protocol | 해당 없음; 토큰은 target 제거까지 유지 | `ZLINK_REQUEST_PROTOCOL_ERROR` |
| accepted 뒤 allocation·runtime failure | 해당 없음; Core가 payload를 보관하지 않음 | `ZLINK_REQUEST_INTERNAL_ERROR` |
| transient physical disconnect | terminal 없음; 토큰 유지, 같은 target의 재연결이 WRITABLE을 발행 | admission 뒤 replay 없이 기존 budget 유지 |
| context termination·socket close | `ZLINK_SEND_TERMINAL`, `ETERM` 또는 `ESHUTDOWN`; 읽지 않은 record는 내부 폐기 | 진행 중 request와 unread record를 내부 폐기하고 새 completion 전달을 보장하지 않음 |

REQUEST reply는 Core가 enqueue 전에 확보한 contiguous `zlink_msg_t[]`에 보관한다. Wire error
reply는 errno part를 닫고 application payload만 새 Core allocation의 index 0부터 정규화한다.
Payload가 없으면 pointer는 `NULL`, count는 `0`이다. Allocation이 실패하면 원래 payload를 닫고
payload 없는 `ZLINK_REQUEST_INTERNAL_ERROR` completion을 만든다. Successful receive는 array와
각 message의 소유권을 caller에게 옮기며 receive 자체는 allocation하지 않는다. Caller는 array를
직접 free하지 않고 `zlink_completion_close()`로 남은 message와 allocator base를 정리한다.

`zlink_completion_close()`는 NULL, WRITABLE과 empty record에도 안전하고 idempotent하다. 모든 field를
0으로 되돌리되 `struct_size`는 보존한다. `struct_size`가 `0` 또는 정확한 구조체 크기가 아니면
pointer를 해제하지 않고 no-op이다. Successful receive 뒤에는 WRITABLE을 포함해 모든 record를 close한다.

Completion queue가 비어 있지 않으면 `ZLINK_POLLCOMPLETION`이 level-trigger된다. 읽지 않은 WRITABLE
record는 `ZLINK_POLLOUT`도 level로 유지한다. Poller wait는
record를 소비하지 않는다. Caller는 DONTWAIT receive를 `NO_DATA`까지 반복한다. 한 socket queue의
drain owner는 하나이며 같은 queue를 두 thread에서 동시에 drain하는 것은 지원하지 않는다.
REQUEST와 WRITABLE 결과는 resolver가 socket-local ready queue에 append한 linearization 순서로
반환한다. 이는 submit 순서나 target별 wire 순서가 아니므로 caller는 ID나 context로 구분한다.

`zlink_completion_recv()`는 PAIR·DEALER·ROUTER·STREAM에서만 지원한다. 다른 socket은
`ZLINK_RECV_NOT_SUPPORTED`+`ENOTSUP`다. `flags_`는 `NONE` 또는 `DONTWAIT`만 허용하며 알 수 없는
bit는 `ZLINK_RECV_INVALID_STATE`+`EINVAL`이다. `DONTWAIT` empty queue와 `NONE`의 timeout은
`ZLINK_RECV_NO_DATA`+`EAGAIN`이다. `NONE`은 진입 시 `RCVTIMEO`를 snapshot하며 기본 1,000 ms,
`0` 즉시, `-1` 무한 대기다. Blocking 중 context termination은
`ZLINK_RECV_TERMINATED`+`ETERM`, socket shutdown은
`ZLINK_RECV_INVALID_STATE`+`ESHUTDOWN`이며 output은 empty다.

### zlink_multipart_close

multipart message 배열의 모든 part를 close한다.

```c
ZLINK_EXPORT void zlink_multipart_close (zlink_msg_t *parts, size_t part_count);
```

각 원소에 대해 `zlink_msg_close()`를 호출하는 편의 함수다.

**참고:** `zlink_msg_close`

---

### zlink_socket_monitor_open

recv 모드로 socket monitor 핸들을 열고 반환한다.

```c
ZLINK_EXPORT void *zlink_socket_monitor_open (void *s_,
                                 const zlink_socket_monitor_open_options_t *options_);
```

socket `s_`에 대한 monitor를 생성하고 핸들을 반환한다. `options_->events`
bitmask로 관찰할 이벤트를 선택한다. `options_->monitor_hwm_bytes`가 `0`이면 Core
기본 byte 예산을, 양수면 그 값을 monitor queue의 byte HWM으로 사용한다 — 예산
규칙은 [Monitoring](../06-monitoring.ko.md)이 소유한다. Event는
`zlink_socket_monitor_recv()`로 직접 수신한다. 반환된 핸들은 더 이상 필요하지 않을 때
`zlink_monitor_close()`로 닫아야 한다.

**반환값:** 성공 시 monitor 핸들, 실패 시 `NULL` (errno가 설정됨).

**참고:** `zlink_socket_monitor_recv`, `zlink_monitor_status`, `zlink_monitor_close`

## 7. 내부 구조

> **이 장의 계약 소유 문서** — option별 공개 계약은 이 문서의 계약 부분과
> [socket 옵션 가이드](../../../guide/12-socket-options.ko.md)가 다룬다. 이 절은 내부 기본값과
> 저장 구조를 설명한다.

`options_t`는 공통 raw-socket과 transport 기본값을 저장한다. typed socket 구현은 pattern별 option을
검증한 뒤 적용한다.

### Queue 계획

`sndhwm`과 `rcvhwm`은 accounted byte를 제한하는 64-bit 값이다. 수동 기본값은
`4,096,000` bytes이며 `0`은 무제한이다. message count HWM을 위한 호환 state는
유지하지 않는다. runtime에서 HWM을 줄여도 이미 queue에 있는 message는 제거하지 않는다.
보관 byte가 새 limit 아래로 감소하는 순간 deferred shrink를 적용한다.

automatic HWM은 context의 Core memory budget, profile 역할 경계와, 두 endpoint가 같은
방향을 관찰해도 한 번만 집계하는 [directional queue](../glossary.ko.md#directional-queue)
registry를 사용한다. registry는 같은 inproc ypipe를 endpoint마다
중복 등록하지 않고 stable queue ID와 generation으로 한 번만 센다. manual reservation을
뺀 budget은 역할별 하한에서 시작해 상한에 도달하지 않은 physical queue에 bounded
[water-filling](../glossary.ko.md#water-filling)으로 나눈다. 나눗셈 remainder는
stable queue ID 순서로 1 byte씩 지급한다.

inproc 양 endpoint 값은 더하지 않는다. 한쪽만 finite manual이면 그 cap, 양쪽이 finite
manual이면 더 작은 cap, 한쪽이 unlimited manual이고 다른 쪽이 auto이면 auto plan을
사용한다. 양쪽이 unlimited면 admission은 unlimited로 유지하되 역할별 상한을 계산용으로
한 번 예약한다.

ROUTER-ROUTER completion progress lane은 terminal reply와 error reply 전용이다. 이
lane에는 auto/manual HWM, LWM, inproc boost, 역할별 경계와 Core budget reservation을
적용하지 않는다. DEALER-ROUTER reply는 Application pipe의 회계와 HWM을 사용한다.
Auto HWM을 비활성화하면 live pipe의 마지막 applied HWM을 유지하고
이후 automatic planning에서 제외한다.

Core pipe low watermark는 `ceil(hwm_bytes / 2)`다. 이 값은 byte credit 반환을
제어하며 Framework의 receive 재개 profile로 변경할 수 없다.

### Application에 보이는 상태

`zlink_monitor_status()` ABI version 4는 계획·적용·보류된 64-bit HWM byte,
pending message count와 pending byte, in-flight byte, minimum message charge와
oversize 단일 message 허용 counter를 제공하고, receive-flow 상태 detail flag와
다섯 개의 flow metric field를 추가한다. context budget snapshot은 physical queue
capacity, provisional·committed queue byte와 completion·monitor queue를 각각
구분한다. ABI 호환용 retained-credit field는 항상 0이다. 이 field는 진단 snapshot이다. application은 내부 값을 직접
바꾸지 않고 public option으로 policy 입력을 설정한다.

### Transport 기본값

reconnect, TCP keepalive, kernel buffer, TOS, handshake interval과 TLS field는 해당 transport가
적용한다. 지원하지 않는 조합은 typed configuration result로 실패한다.

## 8. 구현 및 contract test 검증 요구

공개 표면(socket 생성·연결·옵션·송수신·completion 함수와 반환값·errno)만으로 다음을
확인한다. 각 항목은 unit test 하나로 이어진다.

**생성과 수명**
- `zlink_socket`은 성공 시 non-NULL 핸들을 반환하고, 유효하지 않은 타입은 `EINVAL`, 최대 socket 수 도달은 `EMFILE`, 종료된 context는 `ETERM`이다.
- `zlink_close`는 성공 시 `ZLINK_CLOSE_OK`를 반환한다. 유효하지 않은 pointer는 `EFAULT`, stale opaque value는 `ESTALE`이다.
- 다른 thread가 같은 핸들에서 admitted API를 실행 중일 때 `zlink_close`는 `EBUSY`로 실패하고,
  close가 accepted된 뒤 새 API 진입은 `ESHUTDOWN`이다.
- `zlink_socket_monitor_open`은 성공 시 pull monitor 핸들을, 실패 시 `NULL`과 설정된 errno를 반환한다.

**옵션**
- `ZLINK_OPT_SNDHWM`·`ZLINK_OPT_RCVHWM`은 set·get 모두 정확히 `sizeof(uint64_t)` 크기만 받는다. 4-byte를 포함한 그 밖의 크기는 값을 잘라 쓰거나 일부만 채우지 않고 `ZLINK_CONFIG_INVALID_ARGUMENT`와 `EINVAL`로 실패하며, get 성공 시 `*optvallen_`은 `sizeof(uint64_t)`를 유지한다.
- 제거된 socket option 값 `0x3034`는 `ZLINK_CONFIG_INVALID_ARGUMENT`와 `EINVAL`로 실패한다.
- `ZLINK_OPT_BLOCKY`를 `zlink_set_option()`/`zlink_get_option()`에 주면 `ZLINK_CONFIG_NOT_SUPPORTED`/`ENOTSUP`이다.
- DEALER에서 `ZLINK_OPT_CONFLATE=1`은 `ZLINK_CONFIG_NOT_SUPPORTED`/`ENOTSUP`이고, `0` 설정은
  성공하며 getter는 계속 `0`이다. PUB와 SUB는 `1`을 받아들이고 getter도 `1`을 반환한다.
- 알 수 없는 옵션, 범위 밖 값, 잘못된 byte-count 크기는 `EINVAL`, 종료된 context는 `ETERM`이다.
- `ZLINK_OPT_PENDING_MAX_MSGS/BYTES`는 0x303A/0x303B, 기본 0이며 ABI 호환을 위해 값을 저장·반환할 뿐
  SEND와 REQUEST 어느 동작에도 영향을 주지 않는다. PAIR·DEALER·ROUTER·STREAM은 option 저장을 ABI로
  유지하고 그 외 get/set은 `ZLINK_CONFIG_NOT_SUPPORTED`+`ENOTSUP`다.

**HWM admission** ([Transport/Buffer](#transportbuffer) 참조)
- accounted byte가 HWM에 도달하면 receiver가 byte credit을 반환할 때까지 이후 write가 대기한다.
- DEALER-ROUTER의 REPLY·error reply는 DATA·REQUEST와 같은 Application physical HWM 및 peer
  PAUSED 상태를 적용한다. ROUTER-ROUTER Completion lane의 REPLY·error reply만 이 HWM에서 제외한다.
- 비어 있는 pipe는 admission 시점에 전체 accounted 크기를 아는 complete message 한 건을 HWM보다 크더라도 수락하고, 그 message도 `ZLINK_OPT_MAXMSGSIZE` 검사를 통과해야 하며, 한 건 수락 뒤의 write는 대기한다.
- 최종 크기를 모르는 incremental multipart는 첫 `MORE` frame부터 일반 byte HWM이 적용된다.
- 빈 frame도 charge가 0이 아니므로(payload + `sizeof(zlink_msg_t)`) 빈 frame만 반복 송신해도 HWM에 도달하고, frame이 pipe에서 빠지면 같은 charge가 돌아온다.
- low water mark 기본값은 `ceil(hwm_bytes / 2)`이고, hint는 항상 `1 .. hwm_bytes - 1` 범위로 clamp되며, HWM에 도달한 sender는 receiver가 현재 보이는 입력을 모두 읽으면 LWM 전에도 깨어날 수 있다.

**수신**
- `zlink_recv_part`는 raw `PAIR`·`DEALER`·`STREAM`에서만 성공하고, raw `PUB`·`XPUB`·`SUB`·`XSUB`·`ROUTER`에서는 `ZLINK_RECV_NOT_SUPPORTED`와 `ENOTSUP`이다.
- `ZLINK_RECV_FLAGS_DONTWAIT`에 수신할 part가 없으면 `ZLINK_RECV_NO_DATA`와 `EAGAIN`이다.
- 성공한 수신은 part 소유권을 호출자에게 이전하고(정확히 한 번 close), 실패한 수신은 이전하지 않는다. `source_rid_out_`은 `STREAM`에서 Core-owned view, `PAIR`·`DEALER`에서 `NULL`이다.
- 같은 socket의 다음 data recv 진입은 이전 borrowed RID를 무효화하지만 다른 socket의 data recv,
  poller wait, completion recv와 monitor recv는 무효화하지 않는다.
- `NONE` recv는 호출 진입 시 `RCVTIMEO` 0/positive/-1을 snapshot한다. Timeout은
  `ZLINK_RECV_NO_DATA`+`EAGAIN`, context 종료는 `ZLINK_RECV_TERMINATED`+`ETERM`, socket
  shutdown은 `ZLINK_RECV_INVALID_STATE`+`ESHUTDOWN`이며 output은 변하지 않는다.
- SUB·XPUB의 nonempty topic buffer가 0이거나 너무 작으면 필요한 길이만 바꾸고
  `ZLINK_RECV_BUFFER_TOO_SMALL`+`ENOBUFS`로 record를 보존한다. 충분한 buffer 재호출은 같은
  record를 한 번 성공한 뒤 `NO_DATA`가 된다.
- Empty topic은 capacity 0·NULL buffer로 성공·소비하고, positive capacity와 NULL buffer는 topic
  길이에 관계없이 `ZLINK_RECV_INVALID_HANDLE`+`EFAULT`이며 비소비다.

**routing ID와 연결 종료**
- routing ID를 설정하지 않으면 socket 생성 시 RFC 4122 UUID v4 bit layout의 16-byte binary routing ID가 자동 발급되고 `zlink_get_routing_id`로 조회된다.
- `zlink_set_routing_id`는 1..255 byte binary-safe 값을 받고, raw socket이 아닌 핸들 종류는 `ZLINK_CONFIG_NOT_SUPPORTED`와 `ENOTSUP`이다.
- TLS setter는 TLS를 지원하는 raw socket에서만 성공하고, 지원하지 않는 type과 다른 핸들은 `ZLINK_CONFIG_NOT_SUPPORTED`와 `ENOTSUP`이다.
- `zlink_bind`는 사용 중인 주소에 `EADDRINUSE`, 없는 interface에 `EADDRNOTAVAIL`, 지원하지 않는 transport에 `EPROTONOSUPPORT`이며, TCP port 0으로 bind하면 `ZLINK_OPT_LAST_ENDPOINT`로 실제 endpoint를 조회할 수 있다.
- `zlink_disconnect_rid`는 대상 없음에 `ZLINK_CONNECT_NOT_FOUND`, 중복 routing id에 `ZLINK_CONNECT_CONFLICT`, lifecycle 소유권 충돌에 `ZLINK_CONNECT_BUSY`다.

**Part send와 completion**
- `DONTWAIT FINAL`은 admission을 한 번만 시도한다. 즉시 admission되면 ID `0`과 completion 없음이고,
  backpressure이거나 target이 준비되지 않았으면 `ZLINK_SUBMIT_BACKPRESSURED`+`EAGAIN`과 nonzero 대기
  토큰을 반환하며 payload는 caller가 보관한다. 토큰의 target에 write credit이 생기면
  `ZLINK_COMPLETION_WRITABLE` record를 정확히 한 번 반환하고, caller는 queue를 `NO_DATA`까지 비운 뒤
  같은 record를 다시 제출한다. `NONE FINAL`은 snapshot한 `SNDTIMEO`
  안에서 같은 logical target admission을 기다리며 ID `0`과 completion 없음으로 끝난다.
- 모든 part 호출은 성공·실패와 관계없이 입력을 소비하며 실패한 FINAL과 staging prefix를 함께
  폐기한다. ROUTER·STREAM RID에 route가 없으면 `ZLINK_SUBMIT_NOT_CONNECTED`+`EHOSTUNREACH`, ID `0`이고
  completion reservation 상한 초과는 `ZLINK_SUBMIT_OUT_OF_MEMORY`+`ENOMEM`, ID `0`이다.
- Core는 SEND·REQUEST payload를 admission 전에 보관하거나 Core 소유의 재시도 FIFO를 두지 않는다. 대기
  토큰은 target 단위(PAIR pipe, DEALER candidate peer 집합, ROUTER·STREAM의 해당 RID)로 예약하며
  admission 전 transient disconnect는 토큰을 종료하지 않는다. ID `0` 뒤에는 application payload를
  replay하지 않는다.
- 대기 토큰은 WRITABLE record, target 명시적 제거(`ZLINK_SEND_TERMINAL`+`ENOENT`), socket close·context
  termination(`ZLINK_SEND_TERMINAL`+lifecycle errno)으로만 종료되며 peer weight 0은 대기 토큰을
  종료하지 않는다.
- SEND 대기 토큰과 REQUEST completion을 섞어 65,536개 slot을 채우면 다음 SEND `DONTWAIT FINAL`은
  `ZLINK_SUBMIT_OUT_OF_MEMORY`+`ENOMEM`, 다음 REQUEST FINAL은 `ZLINK_SUBMIT_BACKPRESSURED`+`EAGAIN`,
  모두 ID `0`이고, 한 record를 receive하면 다음 submit이 다시 접수된다.

**Request와 reply**
- DEALER는 NULL target으로 known positive-weight ROUTER route에, ROUTER는 non-NULL ROUTER RID에
  request한다. ROUTER가 DEALER RID를 지정하면 `ZLINK_SUBMIT_NOT_ADMITTED`+`EPROTOTYPE`이고 같은
  RID의 DATA send는 허용된다.
- Admission된 request FINAL은 nonzero REQUEST ID와 정확히 한 REQUEST completion을 만들고 reply
  timeout은 그 admission부터 시작한다. 대기 토큰 없는 submit 실패는 ID `0`, completion과 context echo
  없음으로 끝난다.
- DONTWAIT request FINAL은 admission을 한 번만 시도한다. Backpressure나 준비되지 않은 target(transport
  pair 미준비, weight 0, connect 직후 peer 0개인 DEALER)은 `ZLINK_SUBMIT_BACKPRESSURED`+`EAGAIN`과
  nonzero 대기 토큰을 반환하고, Core는 payload를 보관하지 않으며 같은 토큰·context·RID의 WRITABLE 뒤
  caller가 같은 request를 다시 제출한다. Mandatory ROUTER route가 없으면
  `ZLINK_SUBMIT_NOT_CONNECTED`+`EHOSTUNREACH`, ID `0`, 토큰 없음이다.
- `zlink_reply_part()`의 successful FINAL만 `(responder ROUTER, source RID)` 범위 token을 소비한다.
  Physical disconnect·generation 변경·requester timeout은 token을 무효화하지 않으며 RID 제거,
  responder close와 context termination은 무효화한다.
- Responder ROUTER의 live token 65,536개가 차면 새 REQUEST를 drop·eviction하지 않고 source read를
  멈추며 slot 해제 뒤 round-robin으로 redrive한다.
- Non-NULL request ID output은 다른 validation 전에 `0`이 되고 MORE와 대기 토큰 없는 submit 실패는
  `0`을 유지한다. Output을 생략한 admission된 FINAL도 internal nonzero ID와 context를 정확히 한
  completion에 넣는다.
- Reply allocation·runtime·context·socket 실패는 각각 `OUT_OF_MEMORY`+`ENOMEM`,
  `INTERNAL_ERROR`+`EIO`, `TERMINATED`+`ETERM`, `TERMINATED`+`ESHUTDOWN`이며 모든 call이 part를
  소비하고 live token은 처음부터 재시도할 수 있다.
- Reply하지 않은 token은 자동 소비되지 않는다. Empty-message reply, logical RID 제거 또는 socket
  close가 slot을 해제한다.
- DEALER-ROUTER에서 앞선 DATA의 `FINAL` part를 dequeue하지 않거나 local PAUSED를 유지하면 뒤의
  REPLY가 physical head에 도달하지 못해 request timeout이 먼저 완료될 수 있다. 늦은 REPLY는 두 번째
  completion을 만들지 않는다.
- DEALER peer로 보내는 reply는 Application HWM·PAUSED와 `SNDTIMEO` admission을 적용하여
  `ZLINK_SUBMIT_BACKPRESSURED`+`EAGAIN`으로 끝날 수 있다. ROUTER peer로 보내는 reply는 별도
  Completion lane의 HWM-free admission을 유지한다.

**Completion receive와 ownership**
- Completion이 있으면 `ZLINK_POLLCOMPLETION`이 level-trigger되고 poller wait만으로 queue가 줄지
  않는다. 읽지 않은 WRITABLE record는 `ZLINK_POLLOUT`도 level로 유지한다. DONTWAIT receive로 마지막
  record를 꺼내면 readiness가 해제된다.
- 잘못된 `struct_size`와 non-empty output은 record를 dequeue·overwrite하지 않는다.
  `zlink_completion_close(NULL)`, WRITABLE·empty close는 안전하고 idempotent하며 `struct_size`를
  보존한다.
- REQUEST success와 유효 error reply payload는 contiguous array의 base index 0부터 caller에게
  이동하고 `zlink_completion_close()`가 남은 message와 array를 정리한다. Malformed errno part는
  payload 없는 `ZLINK_REQUEST_PROTOCOL_ERROR`, 정규화 allocation 실패는 payload 없는
  `ZLINK_REQUEST_INTERNAL_ERROR`다.
- Socket close와 context termination은 live SEND·REQUEST 대기 토큰을 `ZLINK_SEND_TERMINAL`과
  lifecycle errno의 WRITABLE로 retire하고, 진행 중 request와 unread record를 내부 정리하며 새
  terminal completion 전달을 보장하지 않는다.
- Completion recv `NONE`은 진입 시 `RCVTIMEO` 0/positive/-1을 snapshot한다. Timeout·unknown
  flags·NULL input과 blocking 중 context/socket 종료는 정해진 result·errno로 queue와 empty output을
  보존한다.
- WRITABLE·REQUEST completion을 섞어 enqueue하면 각 nonzero ID와 context가 socket-local append
  linearization 순서로 한 번씩 반환되며 event array 크기 때문에 유실·병합되지 않는다.
- Completion의 `peer_rid`는 PAIR·DEALER WRITABLE과 DEALER REQUEST에서 empty, ROUTER·STREAM WRITABLE과
  ROUTER REQUEST에서 submit RID snapshot이며 reconnect 뒤 physical identity로 바뀌지 않는다.

**Pull-only 표면**
- Socket DATA, STREAM packet, REQUEST·WRITABLE completion과 monitor event는 각각 정해진 pull 함수로
  소비하고 `zlink_poller_event_t`에는 operation payload가 아니라 readiness bit만 들어간다.

**receive-flow 상태**
- 현재 상태를 다시 설정하는 `zlink_socket_set_receive_flow_state`는 성공하고 새로 보내는 것이 없다.
- Count `1`인 DEALER-DEALER·DEALER-ROUTER에서는 single Application connection의 Core control
  경로로, count `2`인 ROUTER-ROUTER에서는 Completion connection으로 PAUSED·RUNNING을 전달하며
  reconnect 뒤 추가 setter 호출 없이 현재 절대 상태를 다시 보낸다.
- DEALER·ROUTER가 아닌 socket 유형은 `ZLINK_CONFIG_NOT_SUPPORTED`를 반환하며 기존 byte HWM과 transport backpressure를 유지한다.

<!-- zlink-nav:start -->
[Core 스펙 목차](../README.ko.md) | [이전: Runtime 경계](../08-runtime-boundary.ko.md) | [다음: PAIR](01-pair.ko.md)
<!-- zlink-nav:end -->
