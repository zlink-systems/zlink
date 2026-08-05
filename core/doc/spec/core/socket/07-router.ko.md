---
title: "소켓 — ROUTER"
---

[English](07-router.en.md) | 한국어

<!-- zlink-nav:start -->
[소켓 목차](README.ko.md) | [이전: DEALER](06-dealer.ko.md) | [다음: STREAM](08-stream.ko.md)
<!-- zlink-nav:end -->

# 소켓 — ROUTER

> **이 장이 정의하는 것** — ROUTER 소켓의 routing id 기반 응답 라우팅과
> [result/errno](../04-errno-map.ko.md) 공개 계약.

ROUTER는 하나의 socket에서 여러 peer pipe를 관리하고 routing ID로 송신 대상을 선택하는 비동기 raw
socket이다. 일반 directed message와 request/reply record를 처리한다.

## 1. 공개 타입

다음 숫자는 공개 ABI 값이다.

```c
typedef enum zlink_router_option_t {
  ZLINK_ROUTER_OPT_MANDATORY          = 0x3101,
  ZLINK_ROUTER_OPT_PROBE              = 0x3103,
  ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID = 0x3104,
  ZLINK_ROUTER_OPT_REQUEST_TIMEOUT_MS = 0x3105,
  ZLINK_ROUTER_OPT_WEIGHT             = 0x3106
} zlink_router_option_t;

typedef enum zlink_part_flag_t {
  ZLINK_PART_FINAL = 0,
  ZLINK_PART_MORE  = 1
} zlink_part_flag_t;

typedef void (*zlink_reply_handler_fn)(
  zlink_request_result_t result_,
  zlink_msg_t *parts_,
  size_t part_count_,
  void *userdata_);

typedef void (*zlink_completion_control_handler_fn)(
  const zlink_routing_id_t *source_rid_,
  zlink_msg_t *parts_,
  size_t part_count_,
  void *userdata_);
```

`ZLINK_PART_MORE`는 같은 multipart record에 뒤따르는 part가 있음을 뜻한다.
`ZLINK_PART_FINAL`은 현재 part가 마지막임을 뜻한다. receive API의 `has_more_out_`도 같은 값을
사용한다.

## 2. ROUTER option

```c
ZLINK_EXPORT zlink_config_result_t zlink_set_router_option(
  void *handle_,
  zlink_router_option_t option_,
  const void *optval_,
  size_t optvallen_);

ZLINK_EXPORT zlink_config_result_t zlink_get_router_option(
  void *handle_,
  zlink_router_option_t option_,
  void *optval_,
  size_t *optvallen_);
```

| 상수 | 값 형식 | 의미 |
|---|---|---|
| `ZLINK_ROUTER_OPT_MANDATORY` | `int`, `0` 또는 `1` | `1`이면 연결된 pipe가 없는 routing ID의 directed submit을 `ZLINK_SUBMIT_NOT_CONNECTED`로 실패시킨다. 기본값은 `1`이다 |
| `ZLINK_ROUTER_OPT_PROBE` | `int`, `0` 또는 `1` | 연결을 설정할 때 빈 raw message를 보내 peer가 연결과 routing ID를 관찰할 수 있게 한다. 기본값은 `0`이다 |
| `ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID` | 가변 길이 byte string, set 전용 | 다음 `zlink_connect()`로 만든 pipe를 식별할 local alias를 설정한다. 각 connect 전에 설정한다 |
| `ZLINK_ROUTER_OPT_REQUEST_TIMEOUT_MS` | 0 이상인 `int`, millisecond | request API에서 `timeout_ms_ == 0`일 때 사용할 기본 timeout을 정한다. 기본값은 `5000`이다 |
| `ZLINK_ROUTER_OPT_WEIGHT` | `int`, `0..10000` | 연결된 peer에 알리는 이 ROUTER의 가중치다. 기본값은 `100`이다 |

`zlink_get_router_option()`을 호출할 때 `*optvallen_`은 `optval_`의 입력 용량이다. 성공하면 실제로
쓴 byte 수로 갱신된다. ROUTER 전용이 아닌 HWM, reconnect와 timeout option은
`zlink_set_option()`과 `zlink_get_option()`을 사용한다.

## 3. Raw receive record 구분

ROUTER receive API는 `zlink_dealer_message_type_t`를 반환하지 않는다. 다음 output 조합으로 일반 raw
record와 reply가 필요한 request record를 구분한다.

| record | `source_node_rid_out_` | `request_seq_out_` |
|---|---|---:|
| 일반 raw multipart | 송신 peer의 routing ID | `0` |
| 수신 request | 송신 peer의 routing ID | 0이 아닌 reply sequence |

`zlink_router_request_part()`로 시작한 request의 reply와 terminal failure는
`zlink_reply_handler_fn` completion으로 전달되며 일반 receive record로 반환되지 않는다.

`source_node_rid_out_`은 Core가 소유한 thread-local view다. 호출자는 이를 해제하지 않으며, 다음 raw
receive 호출 뒤에도 보관해야 하면 값을 복사한다. 같은 thread에서 다음 receive 호출을 시작하면 앞서
반환된 view는 더 이상 유효하지 않다. 한 multipart record의 모든 part에는 같은 routing ID와 request
sequence가 반환된다.

## 4. Part sequence와 소유권

`*_part` send 호출은 `ZLINK_PART_MORE`부터 `ZLINK_PART_FINAL`까지 하나의 multipart sequence를
구성한다. 열린 sequence가 있는 동안 같은 handle에서 다른 send helper family나 다른 routing ID를
섞을 수 없다.

초기화된 유효한 `part_`를 send API에 넘기면 함수는 성공과 실패 모두에서 그 message 내용을
소비한다. 따라서 호출 결과와 관계없이 호출자가 전송 전 payload를 다시 읽거나 같은 내용을 다시
보낼 수 없다. 다시 보내야 하는 payload는 호출 전에 별도 message로 보관해야 한다.

각 send helper family는 성공한 중간 파트를 `ZLINK_PART_FINAL`이 성공할 때까지 하나의 record로
staging한다. 열린 sequence의 중간 또는 마지막 submit이 실패하면 Core는 이전에 staging한 파트와
실패한 파트를 원자적으로 폐기하고 sequence를 닫는다. peer에는 그 record의 어떤 파트도 보이지
않는다. 실패한 호출의 `part_`도 소비되며 다음 submit은 새 record의 첫 파트로 시작한다. 실패한
request submit은 request sequence를 만들지 않고 handler도 호출하지 않는다. reply sequence가 실패하면
reply token과 peer RID 조합은 성공한 `ZLINK_PART_FINAL`이나 request lifecycle 종료 전까지 유효하므로 caller가
보관한 전체 reply를 첫 파트부터 다시 제출할 수 있다.

receive API의 `part_out_`은 호출 전에 초기화된 `zlink_msg_t`여야 한다. 성공하면 수신 part의
소유권이 호출자에게 이동하며 호출자는 `zlink_msg_close()`로 정확히 한 번 해제한다. 실패하면
수신 part 소유권은 이동하지 않는다.

## 5. Directed raw send

```c
ZLINK_EXPORT zlink_submit_result_t zlink_send_part_rid(
  void *s_,
  const zlink_routing_id_t *target_rid_,
  zlink_msg_t *part_,
  zlink_send_flags_t flags_,
  zlink_part_flag_t part_flag_);
```

`target_rid_`의 peer에 일반 raw multipart part를 보낸다. 모든 part에 같은 target을 사용해야 한다.
`flags_`는 `ZLINK_SEND_FLAGS_NONE` 또는 `ZLINK_SEND_FLAGS_DONTWAIT`다. 이 API는 request sequence나
completion handler를 만들지 않는다.

## 6. Raw request submit

```c
ZLINK_EXPORT zlink_submit_result_t zlink_router_request_part(
  void *router_,
  const zlink_routing_id_t *peer_rid_,
  zlink_msg_t *part_,
  zlink_send_flags_t flags_,
  zlink_part_flag_t part_flag_,
  uint32_t timeout_ms_,
  zlink_reply_handler_fn handler_,
  void *userdata_);
```

`peer_rid_`에 비동기 request payload를 part 단위로 제출한다. 중간 part는
`ZLINK_PART_MORE`, `timeout_ms_ == 0`, `handler_ == NULL`, `userdata_ == NULL`로 호출한다. 마지막
part는 `ZLINK_PART_FINAL`과 0이 아닌 `handler_`를 사용한다. 마지막 호출의 `timeout_ms_ == 0`은
`ZLINK_ROUTER_OPT_REQUEST_TIMEOUT_MS` 기본값을 사용한다.

마지막 submit이 `ZLINK_SUBMIT_OK`이면 completion은 정확히 한 번 `handler_`로 전달된다. submit이
실패하면 handler를 호출하지 않는다. callback의 `parts_`와 각 message의 소유권은 callback으로
이동하며 callback은 이를 정확히 한 번 해제한다.

## 7. Raw request와 message receive

```c
ZLINK_EXPORT zlink_recv_result_t zlink_router_recv_part(
  void *router_,
  const zlink_routing_id_t **source_node_rid_out_,
  uint64_t *request_seq_out_,
  zlink_msg_t *part_out_,
  zlink_part_flag_t *has_more_out_,
  zlink_recv_flags_t flags_);
```

완전한 raw record에서 part 하나를 반환한다. 모든 output pointer는 필수다. `flags_`는
`ZLINK_RECV_FLAGS_NONE` 또는 `ZLINK_RECV_FLAGS_DONTWAIT`다. non-blocking 호출에 받을 record가 없으면
`ZLINK_RECV_NO_DATA`와 `EAGAIN`을 반환한다.

`has_more_out_ == ZLINK_PART_MORE`이면 다음 호출로 같은 record의 다음 part를 받아야 한다.
`ZLINK_PART_FINAL`이면 record 수신이 끝난다. reply가 필요한지는 §3의 output 조합으로 판단한다.

## 8. Raw reply submit

```c
ZLINK_EXPORT zlink_submit_result_t zlink_router_reply_part(
  void *router_,
  const zlink_routing_id_t *peer_rid_,
  uint64_t request_seq_,
  zlink_msg_t *part_,
  zlink_part_flag_t part_flag_);
```

`zlink_router_recv_part()`로 받은 request에 reply part를 보낸다. `peer_rid_`와 0이 아닌
`request_seq_`는 수신 record가 반환한 값을 그대로 사용한다. 여러 part로 reply할 때 모든 호출에서
같은 두 값을 사용한다. `ZLINK_PART_FINAL`이 성공하면 reply가 완료된다.

## 9. Raw completion control

```c
ZLINK_EXPORT zlink_handler_result_t
zlink_router_completion_control_handler(
  void *router_,
  zlink_completion_control_handler_fn handler_,
  void *userdata_);

ZLINK_EXPORT zlink_submit_result_t
zlink_router_completion_control_part(
  void *router_,
  const zlink_routing_id_t *peer_rid_,
  zlink_msg_t *part_,
  zlink_part_flag_t part_flag_);
```

Completion control은 Core가 내용을 해석하지 않는 bounded raw multipart record다. 기존
Application·Completion connection pair의 Completion connection을 사용한다. 새 socket이나 connection을
만들지 않으며 일반 directed message와 request는 Application connection에 그대로 둔다.

Handler는 socket마다 하나이며 다시 등록하면 교체한다. `NULL` handler는
`ZLINK_HANDLER_INVALID_ARGUMENT`, ROUTER가 아닌 socket은 `ZLINK_HANDLER_NOT_SUPPORTED`다. Handler가
등록되지 않은 상태에서 받은 record는 폐기한다.

Callback이 실행 중이면 같은 socket의 close는 `ZLINK_CLOSE_BUSY`와 `EBUSY`를 반환한다. Callback이
끝난 뒤 close를 다시 호출할 수 있다.

Handler는 completion owner가 해당 connection을 처리할 때 실행된다. Application receive API를 호출하지
않아도 `ZLINK_POLLCOMPLETION` poller가 계속 동작하면 control을 받을 수 있다. `source_rid_`는 callback이
끝날 때까지만 유효하다. Payload part의 소유권은 callback으로 이동하며 callback은 각 part를 정확히 한 번
해제하거나 소비한다.

Part sequence와 실패 시 소유권은 §4를 따른다. Completion connection은 유한한 byte HWM을 사용한다.
각 submit 호출은 결과와 관계없이 전달받은 `part_`를 소비한다. Submit이
`ZLINK_SUBMIT_BACKPRESSURED`이면 caller는 독립적으로 보관해 둔 전체 record를 send-ready 뒤 처음
part부터 다시 제출한다. Core는 payload의 command 종류, 허용 목록과 업무 의미를 정의하지 않는다.

## 10. Result와 readiness

submit은 `zlink_submit_result_t`, receive는 `zlink_recv_result_t`, option은
`zlink_config_result_t`를 반환한다. 각 result와 `zlink_errno()`의 대응은
[errno map](../04-errno-map.ko.md)을 따른다.

ROUTER의 `ZLINK_POLLIN`은 완전한 raw record를 수신할 수 있음을 뜻한다. `ZLINK_POLLOUT`과
`zlink_send_ready_handler()`는 backpressure 뒤 submit을 다시 시도할 가치가 있음을 나타내지만 다음
submit 성공을 보장하지 않는다.
