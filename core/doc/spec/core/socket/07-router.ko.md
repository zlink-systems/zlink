---
title: "Socket — ROUTER"
---

[English](https://zlink-systems.github.io/zlink/spec/core/socket/07-router/) | 한국어

<!-- zlink-nav:start -->
[소켓 목차](README.ko.md) | [이전: DEALER](06-dealer.ko.md) | [다음: STREAM](08-stream.ko.md)
<!-- zlink-nav:end -->

# Socket — ROUTER

> **이 장이 정의하는 것** — ROUTER 소켓의 routing id 기반 응답 라우팅과
> [result/errno](../03-errors.ko.md#result와-errno-대응) 공개 계약.

## 1. ROUTER 개요

ROUTER는 하나의 [socket](../glossary.ko.md#socket)에서 여러 peer와의 연결(pipe)을 관리하고,
peer를 식별하는 byte 열인 routing ID로 송신 대상을 선택하는 비동기 raw socket이다. 일반
directed message와 수신 request record를 처리한다. routing ID를 전달하는 타입
`zlink_routing_id_t`의 계약은 [Message](../02-message.ko.md#zlink_routing_id_t)가 소유한다.

관련 계약의 소유 문서는 다음과 같다.

| 관련 계약 | 정의하는 문서 |
|---|---|
| socket 생성·공통 옵션·`zlink_socket_set_receive_flow_state` 함수 선언 | [Socket 공통](README.ko.md) |
| routing ID 타입(`zlink_routing_id_t`) | [Message](../02-message.ko.md#zlink_routing_id_t) |
| Request-reply kind, sequence와 ZMP header byte 배치·검증 | [ZMP](../protocol/01-zmp.ko.md) |
| 각 result와 errno 대응, receive flow state 결과 표 | [Errors](../03-errors.ko.md) |
| ROUTER와 pair를 이루는 상대 socket | [DEALER](06-dealer.ko.md) |
| socket status snapshot | [Monitoring](../06-monitoring.ko.md) |

## 2. DATA와 REQUEST receive

`zlink_router_recv_part()`는 source logical RID와 reply token으로 DATA와 REQUEST를 구분한다.

| record | `source_rid_out_` | `reply_token_out_` |
|---|---|---:|
| DATA multipart | 송신 peer의 logical RID | `0` |
| REQUEST | 송신 peer의 logical RID | Core가 만든 nonzero opaque token |

Multipart REQUEST의 모든 part에는 같은 RID와 token을 반환한다. Token은 wire request sequence가
아니며 application은 값을 해석·생성·변경하지 않는다. REQUEST 전체를 `FINAL`까지 받은 뒤에만
reply sequence를 시작한다. ROUTER가 제출한 REQUEST의 reply·timeout·terminal 결과는 일반 receive에
나타나지 않고 REQUEST completion으로 반환된다.

반환한 RID는 socket-owned borrowed view다. 같은 socket의 다음 data recv API에 진입하거나 socket을
close할 때까지 유효하다. Poller wait, completion recv, monitor recv와 다른 socket의 data recv는
무효화하지 않는다. 더 오래 보관할 caller와 binding은 receive 직후 owned RID로 복사한다.

## 3. Part sequence와 소유권

`*_part` send 호출은 `ZLINK_PART_MORE`부터 `ZLINK_PART_FINAL`까지 하나의 multipart sequence를
구성한다. 열린 sequence가 있는 동안 같은 handle에서 다른 send helper family나 다른 routing ID를
섞을 수 없다.

초기화된 유효한 `part_`를 send API에 넘기면 함수는 성공과 실패 모두에서 그 message 내용을
소비하고 길이 0인 초기화 상태로 둔다. 따라서 호출 결과와 관계없이 호출자가 전송 전 payload를
다시 읽거나 같은 내용을 다시 보낼 수 없다. 다시 보내야 하는 payload는 호출 전에 별도 message로
보관해야 한다.

각 send helper family는 성공한 중간 part를 `ZLINK_PART_FINAL`이 성공할 때까지 하나의 record로
staging한다. 열린 sequence의 중간 또는 마지막 submit이 실패하면 Core는 이전에 staging한 part와
실패한 part를 원자적으로 폐기하고 sequence를 닫는다. peer에는 그 record의 어떤 part도 보이지
않는다. 실패한 호출의 `part_`도 소비되며 다음 submit은 새 record의 첫 part로 시작한다. 실패한
request submit은 ID `0`이고 completion과 context echo를 만들지 않는다. Reply sequence가 실패해도
logical RID와 token이 유지되는 동안 caller가 보관한 전체 reply를 처음부터 다시 제출할 수 있다.

receive API의 `part_out_`은 호출 전에 초기화된 `zlink_msg_t`여야 한다. 성공하면 수신 part의
소유권이 호출자에게 이동하며 호출자는 `zlink_msg_close()`로 정확히 한 번 해제한다. 실패하면
수신 part 소유권은 이동하지 않는다.

## 4. 공개 타입

다음 숫자는 공개 ABI 값이다.

```c
typedef enum zlink_router_option_t {
  ZLINK_ROUTER_OPT_MANDATORY          = 0x3101,  // int, 0=off, 양수=on, getter는 0/1 반환, 기본 1. 미연결 routing ID의 directed submit 실패 여부
  ZLINK_ROUTER_OPT_PROBE              = 0x3103,  // int, 0=off, 양수=on, getter는 0/1 반환, 기본 0. 연결 설정 시 빈 raw message로 peer가 연결·routing ID 관찰
  ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID = 0x3104,  // 가변 길이 byte string, set 전용. 다음 zlink_connect() pipe의 local alias
  ZLINK_ROUTER_OPT_REQUEST_TIMEOUT_MS = 0x3105,  // 0 이상인 int (millisecond), 기본 5000. request의 timeout_ms_ == 0일 때 기본 timeout
  ZLINK_ROUTER_OPT_WEIGHT             = 0x3106   // int, 0..10000, 기본 100. 연결된 peer에 알리는 이 ROUTER의 가중치
} zlink_router_option_t;

typedef enum zlink_part_flag_t {
  ZLINK_PART_FINAL = 0,  // 현재 part가 record의 마지막
  ZLINK_PART_MORE  = 1   // 같은 multipart record에 뒤따르는 part가 있음
} zlink_part_flag_t;

typedef uint64_t zlink_reply_token_t;  // DATA는 0, REQUEST는 nonzero opaque capability
```

`ZLINK_PART_MORE`는 같은 multipart record에 뒤따르는 part가 있음을 뜻한다.
`ZLINK_PART_FINAL`은 현재 part가 마지막임을 뜻한다. receive API의 `has_more_out_`도 같은 값을
사용한다.

## 5. ROUTER option

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

각 option의 값 형식·범위·기본값은 [§4](#4-공개-타입)의 인라인 주석이 정의한다. 주석에
담기지 않는 계약은 다음과 같다.

- `ZLINK_ROUTER_OPT_MANDATORY`가 양수이면 연결된 pipe가 없는 routing ID의 directed submit을
  `ZLINK_SUBMIT_NOT_CONNECTED`로 실패시킨다. 그때 route가 전혀 없는 routing ID의 `DONTWAIT FINAL`은
  즉시 `ZLINK_SUBMIT_NOT_CONNECTED`이며 wait token을 만들지 않는다. option이 `0`이면 기존대로 record를
  조용히 버리고 `ZLINK_SUBMIT_OK`, ID `0`이다.
- `ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID`는 다음 `zlink_connect()`로 만든 pipe를 식별할 local
  alias를 설정하며, 각 connect 전에 설정한다.

`zlink_get_router_option()`을 호출할 때 `*optvallen_`은 `optval_`의 입력 용량이다. 성공하면 실제로
쓴 byte 수로 갱신된다. ROUTER 전용이 아닌 [HWM](../glossary.ko.md#hwm)(queue 보관 byte 상한),
reconnect와 timeout option은 `zlink_set_option()`과 `zlink_get_option()`을 사용한다.

`ZLINK_ROUTER_OPT_WEIGHT`는 이 ROUTER를 peer가 outbound 후보로 선택할 때 사용할 절대값이다.
ROUTER와 DEALER는 자기 값을 독립적으로 알리므로 각 방향은 상대 socket이 알린 값을 사용한다.

공개 weight 결과는 다음 순서를 따른다.

1. Bind·connect 전에 설정한 값은 paired Application pipe가 ready 된 뒤 적용된다.
2. Dynamic 변경은 `0`을 포함한 새 절대값을 peer scheduler에 적용한다.
3. 실제 값이 바뀌면 `PEER_WEIGHT_CHANGED`가 값과 Application lane·connection ID를 제공한다.
   같은 값을 반복 설정하면 event를 추가로 만들지 않는다.
4. Reconnect 뒤에는 현재 설정값을 새 connection에 적용한다.

Network wire, inproc 전달, CONTROL 크기 경계, multipart defer와 선택한 그 pipe의 lifetime·stale
전달 소유권은 [ZMP request-reply lane](../protocol/01-zmp.ko.md#41-request-reply-lane),
[decode](../protocol/01-zmp.ko.md#7-decode-유효성-검사),
[peer-weight owner](../protocol/01-zmp.ko.md#peer-weight-control) 계약이 정의한다. 어느 transport
경로도 public receive나 Completion lane에 weight record를 만들지 않는다.

Multipart가 pipe를 선택한 뒤 적용값이 `0`이 되어도 그 message는 같은 pipe에서 FINAL까지
완료한다. 다음 message 선택부터 그 pipe를 제외한다.

Remote weight가 실제로 바뀌면 wait token이 있는 DONTWAIT send와 request를 다시 평가한다. Wait
token은 weight가 `0`이 되어도 끝나지 않는다. `0`에서 양수로 바뀌면 그 RID의 SEND·REQUEST wait
token에 `ZLINK_COMPLETION_WRITABLE` record를 발행한다.

Active duplicate는 standby 동안 자기 최신 값을 보관하고 나중에 같은 pipe가 선택되면 사용한다.
Application 최대값을 10 byte보다 작게 설정해도 pair readiness·FLOWSTATE·WEIGHT 전달은 막히지
않으며, 잘못된 CONTROL의 동작은 ZMP가 소유한다.

## 6. Directed raw send

```c
ZLINK_EXPORT zlink_submit_result_t zlink_send_part_rid(
  void *s_,
  const zlink_routing_id_t *target_rid_,
  zlink_msg_t *part_,
  zlink_send_flags_t flags_,
  zlink_part_flag_t part_flag_,
  void *user_context_,
  zlink_completion_id_t *completion_id_out_);
```

`target_rid_`의 peer에 일반 raw multipart part를 보낸다. 모든 part에 같은 target을 사용해야 한다.
`flags_`는 `ZLINK_SEND_FLAGS_NONE` 또는 `ZLINK_SEND_FLAGS_DONTWAIT`다. `NONE FINAL`은
`SNDTIMEO`를 snapshot해 같은 logical RID의 admission과 reconnect를 기다리고 ID `0`으로 끝난다.
`DONTWAIT FINAL`은 admission을 한 번만 시도한다. 즉시 admission되면 ID `0`과 completion
없음이다. HWM·byte credit 때문에 admission하지 못하거나 route는 있지만 아직 준비되지
않았으면(transport pair 미준비, weight `0`) `ZLINK_SUBMIT_BACKPRESSURED`+`EAGAIN`과 그 RID에
묶인 nonzero wait token을 반환하며 payload는 유지하지 않는다. `target_rid_`에 route가 전혀
없으면 `ZLINK_ROUTER_OPT_MANDATORY`가 양수일 때(기본값) 즉시 `ZLINK_SUBMIT_NOT_CONNECTED`이고 token을
만들지 않는다(option이 `0`이면 기존대로 조용히 버리고 ID `0`). 같은 RID에 write credit이 생기면(peer drain, reconnect·route 채택·standby 승격,
weight `0`→양수) Core는 그 token의 `ZLINK_COMPLETION_WRITABLE` record를 정확히 하나 만들며
`send_result == ZLINK_SEND_ADMITTED`, `peer_rid`는 제출한 RID다. 다른 RID의 credit은 이 token을
깨우지 않는다. 호출자는 보관한 record를 같은 RID에 `DONTWAIT`로 다시 제출한다.
`zlink_disconnect_rid()`로 그 RID를 명시적으로 제거하면 token은
`ZLINK_SEND_TERMINAL`+`ENOENT`인 WRITABLE record로 끝나고, socket close·context 종료는 token을
내부에서 끝내며 record를 전달하지 않는다. ID `0` 뒤에는 payload를 replay하지 않는다.
Ownership과 exact result·errno는 [Socket 공통](README.ko.md#part-send와-pending-admission)을
따른다.

## 7. Raw request submit

```c
ZLINK_EXPORT zlink_submit_result_t zlink_request_part(
  void *s_,
  const zlink_routing_id_t *target_router_rid_or_null_,
  zlink_msg_t *part_,
  zlink_send_flags_t flags_,
  zlink_part_flag_t part_flag_,
  uint32_t timeout_ms_,
  void *user_context_,
  zlink_completion_id_t *completion_id_out_);
```

`target_router_rid_or_null_`은 target ROUTER의 non-NULL logical RID여야 한다. DEALER RID를
지정하면 `ZLINK_SUBMIT_NOT_ADMITTED`+`EPROTOTYPE`이며 같은 RID의 DATA send는 허용한다.
Routing map에 RID가 없으면 `NONE`은 `ZLINK_SUBMIT_NOT_FOUND`+`ENOENT`, `DONTWAIT`은
`ZLINK_SUBMIT_NOT_CONNECTED`+`EHOSTUNREACH`이며 wait token을 만들지 않는다.

`MORE`는 `timeout_ms_ == 0`, `user_context_ == NULL`이어야 한다. Optional ID output은 다른
validation 전에 `0`이 되며 admission된 `FINAL`은 nonzero REQUEST ID를 반환한다. Core는 request를
wire에 공개하기 전에 SEND·REQUEST 공유 completion slot과 ID를 확보한다. Slot 포화는 flags와
관계없이 즉시 `ZLINK_SUBMIT_BACKPRESSURED`+`EAGAIN`, ID `0`, completion 없음이다.

`NONE FINAL`은 임시 reservation 뒤 `SNDTIMEO` 안에서 same-RID local admission을 기다린다.
Admission 전 실패는 reservation을 반납하고 ID `0`, completion 없음으로 동기 종료한다.

`DONTWAIT FINAL`은 same-RID admission을 한 번만 시도하며 admission 전에 Core가 record를 소유하는
상태는 없다. 즉시 admission되면 nonzero REQUEST ID를 반환한다. HWM·byte credit·flow pause에 의한
backpressure이거나 RID의 route가 있으나 아직 준비되지 않은 경우(transport pair 미준비, weight `0`)에는
`ZLINK_SUBMIT_BACKPRESSURED`+`EAGAIN`과 그 RID를 target으로 하는 nonzero wait token을 반환하며 Core는
request payload를 보관하지 않는다. 그 RID에 write credit이 생기면 같은 token·context·`peer_rid`의
`ZLINK_COMPLETION_WRITABLE` record 한 건이 뒤따르고 caller는 같은 request를 다시 제출한다. 다른
RID의 credit은 이 token을 발행하지 않는다. Mandatory route가 없는 RID는
`ZLINK_SUBMIT_NOT_CONNECTED`+`EHOSTUNREACH`, ID `0`, token 없음이다.

`timeout_ms_ == 0`은 `ZLINK_ROUTER_OPT_REQUEST_TIMEOUT_MS`의 기본 5,000 ms를 snapshot한다.
Reply timeout은 outbound local admission, 즉 `ZLINK_SUBMIT_OK` 반환부터 시작하며 wait token이
유지되는 동안은 시작하지 않는다. Admission 뒤 submit 시점 transport pair가 종료되면
[Socket 공통 §6의 completion 표](README.ko.md#request와-reply)대로 즉시 `ZLINK_REQUEST_NOT_CONNECTED`로
한 번 종결하며 payload를 replay하지 않는다. Reply·timeout·terminal 중 하나만 REQUEST
completion을 만든다.

```mermaid
sequenceDiagram
    participant App as Application
    participant R as ROUTER (Core)
    participant P as Peer
    App->>R: 중간 part 제출 (ZLINK_PART_MORE)
    App->>R: 마지막 part 제출 (ZLINK_PART_FINAL, context)
    Note over R: wire 공개 전에 completion ID·slot과 correlation 등록
    R->>P: request record 전달
    P-->>R: reply 또는 terminal failure
    R->>R: REQUEST completion enqueue
    R-->>App: POLLCOMPLETION readiness
    App->>R: completion_recv(DONTWAIT)
    R-->>App: result와 reply multipart
```

위 diagram은 마지막 submit이 성공한 경로다. Submit 실패는 [§3](#3-part-sequence와-소유권)의
폐기 규칙을 따르고 completion을 만들지 않는다.

## 8. Raw request와 message receive

```c
ZLINK_EXPORT zlink_recv_result_t zlink_router_recv_part(
  void *router_,
  const zlink_routing_id_t **source_rid_out_,
  zlink_reply_token_t *reply_token_out_,
  zlink_msg_t *part_out_,
  zlink_part_flag_t *has_more_out_,
  zlink_recv_flags_t flags_);
```

완전한 DATA 또는 REQUEST record에서 part 하나를 반환한다. 모든 output pointer는 필수다. `flags_`는
`ZLINK_RECV_FLAGS_NONE` 또는 `ZLINK_RECV_FLAGS_DONTWAIT`다. non-blocking 호출에 받을 record가 없으면
`ZLINK_RECV_NO_DATA`와 `EAGAIN`을 반환한다.

`has_more_out_ == ZLINK_PART_MORE`이면 다음 호출로 같은 record의 다음 part를 받아야 한다.
`ZLINK_PART_FINAL`이면 record 수신이 끝난다. Reply가 필요한지는
[§2](#2-data와-request-receive)의 output 조합으로 판단한다.

반환한 payload에는 internal request metadata가 없다. Multipart request의 첫 part에서 얻은
routing ID와 opaque reply token은 같은 record의 나머지 part에 반복해서 반환하되,
message 자체에는 보존하지 않는다.

Part output ownership, `NONE`의 `RCVTIMEO`, output 불변과 socket-owned borrowed RID 수명은
[Socket 공통](README.ko.md#zlink_recv_part)의 data recv 공통 계약을 따른다.

## 9. Raw reply submit

```c
ZLINK_EXPORT zlink_submit_result_t zlink_reply_part(
  void *router_,
  const zlink_routing_id_t *source_rid_,
  zlink_reply_token_t reply_token_,
  zlink_msg_t *part_,
  zlink_part_flag_t part_flag_);
```

`zlink_router_recv_part()`로 받은 REQUEST의 source RID와 nonzero opaque reply token을 그대로
사용한다. Wire request sequence는 Core 내부 metadata이며 reply token과 값이 같다는 보장은 없다.
모든 호출은 결과와 관계없이 initialized `part_`를 소비해 empty initialized 상태로 만든다.

첫 `MORE` 또는 `FINAL`에서 RID·token·REQUEST-complete 상태를 검증하고 token을 reply sequence에
원자적으로 checkout한다. Successful `MORE`는 part를 local staging하고 checkout을 유지하지만
token을 소비하지 않는다. `FINAL`은 `ZLINK_OPT_SNDTIMEO`를 진입 시 snapshot해 같은 logical source
RID의 reply route가 local admission될 때까지 기다린다. Source peer가 DEALER이면 현재 ready
Application pipe를, ROUTER이면 현재 ready
[completion progress lane](../glossary.ko.md#completion-progress-lane)의 Completion pipe를 고른다. 기본값은
1,000 ms, `0`은 즉시,
`-1`은 무한 대기다. Successful `FINAL`만 registry의 token을 소비하며 requester application의
수신이나 acceptance를 보장하지 않는다. Reply는 completion ID나 completion record를 만들지 않는다.

대기 만료는 `ZLINK_SUBMIT_BACKPRESSURED`+`EAGAIN`, allocation 실패는
`ZLINK_SUBMIT_OUT_OF_MEMORY`+`ENOMEM`, 그 밖의 runtime failure는
`ZLINK_SUBMIT_INTERNAL_ERROR`+`EIO`다. Context termination과 socket shutdown은 각각
`ZLINK_SUBMIT_TERMINATED`+`ETERM`, `ZLINK_SUBMIT_TERMINATED`+`ESHUTDOWN`이다. Logical RID가
명시적으로 제거됐거나 token이 없거나 이미 소비됐거나 RID와 맞지 않으면
`ZLINK_SUBMIT_NOT_FOUND`+`ENOENT`, REQUEST의 `FINAL`을 받기 전 reply는
`ZLINK_SUBMIT_INVALID_STATE`+`EBUSY`다.

첫 part의 checkout 전 validation 실패는 그 call의 part만 소비한다. Checkout한 sequence의 call이
실패하면 실패한 part와 staging prefix를 모두 폐기하고 checkout을 해제한다. Token이 아직 live이면
timeout·allocation·runtime·early-reply·후속 part mismatch 뒤에도 보관한 전체 reply를 처음부터
재시도할 수 있다. 같은 token으로 second sequence를 시작하면
`ZLINK_SUBMIT_INVALID_STATE`+`EBUSY`로 second part만 소비하고 기존 staging과 checkout은 유지한다.
열린 sequence의 후속 part가 다른 RID·token을 쓰면 `ZLINK_SUBMIT_INVALID_ARGUMENT`+`EINVAL`로
그 sequence를 폐기하고 original checkout을 해제한다.

Token은 `(responder ROUTER socket, source logical RID)` 범위의 opaque nonzero capability다.
Application은 이를 생성·숫자 변환·연산하지 않는다. Physical disconnect, connection generation
변경과 requester timeout은 token을 무효화하지 않는다. Successful reply `FINAL`, 명시적 logical
RID 제거, responder socket close와 context termination만 token을 무효화한다. Requester timeout은
취소 신호가 아니므로 이후 reply의 local admission은 성공할 수 있으며 requester Core는 이미
correlation이 없으면 늦은 reply를 폐기한다.

Token ID는 responder socket에서 단조 증가하며 close 전 재사용하지 않는다. 다음 nonzero ID를
만들 수 없으면 새 REQUEST를 application queue에 넣지 않고 internal error reply로 requester를
`ZLINK_REQUEST_INTERNAL_ERROR` 완료하며 token과 slot은 만들지 않는다. Live registry는 ROUTER당
65,536개다. 포화 시 token이 필요한 REQUEST가 ingress head인 source pipe의 read/credit을 멈추되,
다른 pipe의 DATA와 이미 admission된 record는 fair queue로 진행한다. 같은 pipe의 후속 DATA는
REQUEST를 앞지르지 않는다. Slot 해제 시 paused pipe를 round-robin으로 redrive한다. Token을
자동 eviction하거나 REQUEST를 조용히 drop하지 않으며 public abandon·cancel API도 없다.

## 10. Result와 readiness

submit은 `zlink_submit_result_t`, receive는 `zlink_recv_result_t`, option은
`zlink_config_result_t`를 반환한다. 각 result와 `zlink_errno()`의 대응은
[errno map](../03-errors.ko.md#result와-errno-대응)을 따른다.

ROUTER의 `ZLINK_POLLIN`은 application queue에 admission된 DATA·REQUEST 또는 token slot을 예약할
수 있는 완전한 REQUEST가 있음을 뜻한다. 모든 readable head가 token-blocked REQUEST이면 준비되지
않는다. Ordinary send와 request에서
`ZLINK_POLLOUT`은 [backpressure](../glossary.ko.md#backpressure)(수신 측이 따라오지 못해 추가
제출이 제한되는 상태) 뒤 submit을 다시 시도할 가치가 있음을 나타내지만 다음 submit
성공을 보장하지 않는다. 읽지 않은 `ZLINK_COMPLETION_WRITABLE` record가 있는 동안
`ZLINK_POLLOUT`과 `ZLINK_POLLCOMPLETION`은 level로 유지되며, RID별 정확한 신호는 그 record의
token·`peer_rid`다. Core가 접수한 SEND·REQUEST 결과는 `ZLINK_POLLCOMPLETION`과
`zlink_completion_recv()`로 받는다. Reply submit은 completion을 만들지 않는다.

## 11. Receive flow state

DEALER 또는 ROUTER peer와 연결된 ROUTER는 그 peer들에게 자신에게
보내는 전송을 멈추고 다시 시작하라고 요청할 수 있다. `zlink_socket_set_receive_flow_state()`는
socket 전체에 적용되는 상태 하나를 저장한다. 함수 선언은 [Socket 공통](README.ko.md)이, 결과 표는
[Errors](../03-errors.ko.md)가 소유한다.

이 상태는 routing ID가 아니라 socket에 속한다. Peer별 flow-state 호출은 없다. 한 번 호출하면
이 ROUTER의 ready peer마다 상태를 전달하므로 모든 peer가 같은 상태를 받는다. DEALER peer에는
single Application connection의 Core control 경로를, ROUTER peer에는 Completion connection을
사용한다. 나중에 ready가 된 peer도 peer type에 따른 경로로 socket의 현재 상태를 받는다.
Routing ID는 send의 목적지를 고르는 값이며 receive-flow 상태를 고르지 않는다.

이 상태는 counter가 아니라 절대값이다. 현재 상태를 다시 설정하면 성공하고 아무것도 보내지
않는다.

Flow-state frame은 자신이 기록된 connection 범위의 flow epoch를 담는다. Public pair ID·generation
field와 `Zlink-Pair-Id`·`Zlink-Pair-Generation` wire property는 없다. Core는 내부 connection
identity로 frame이 기록된 connection에만 frame을 적용한다. 대체된 connection의 frame을 포함해
identity가 일치하지 않는 frame은 public event 없이 내부에서 소비하고
`flow_state_stale_total` counter에만 반영한다. 같은 connection에서 epoch가 중복·역행하면 frame을
적용하지 않고 `ZLINK_EVENT_FLOW_STATE_STALE`로 보고하며
`ZLINK_MONITOR_EVENT_FLAG_FLOW_STATE_STALE_EPOCH`을 설정한다. Routing ID는 재연결 후에도 유지될 수
있지만, peer가 재연결 전에 알린 상태가 그것을 대체한 connection에 적용되는 일은 없다. 새
connection은 pair가 ready가 될 때 socket이 보내는 상태에서 시작한다.

Remote PAUSE는 pause한 peer로 보내는 전송만 막고 다른 peer로 가는 route에는 영향이 없다.
이는 byte HWM, transport wait, termination과 합성되는 독립적인 차단 요인이므로 해제만으로
다음 send가 수락되지는 않는다. Send 결과와 readiness는 그대로다. 차단된 non-blocking send는
계속 `errno == EAGAIN`과 함께 `ZLINK_SUBMIT_BACKPRESSURED`를 보고하고, mandatory routing은
[§6](#6-directed-raw-send)이 정의한 동작을 유지한다.

Remote PAUSE는 다음 message 경계부터 적용하며 multipart record를 쪼개지 않는다. 이 ROUTER가
routing ID part를 이미 수락한 record는 남은 part를 끝까지 보낸 뒤에 pause가 적용된다.

[Monitoring](../06-monitoring.ko.md)의 status snapshot은 현재 pause 상태인 peer 수와 함께
socket 전체의 적용된 전이 수, stale 수, pause 길이를 제공한다.

## 12. 내부 구조

> **이 절의 계약 소유** — directed submit과 request submit의 공개 계약은
> [§6](#6-directed-raw-send)과 [§7](#7-raw-request-submit)이 소유한다. 이 절은 binding이 그
> 계약 위에서 multipart 시도를 어떻게 직렬화하는지 설명한다.

Part 호출마다 public API scope가 따로이므로 binding은 첫 part부터 FINAL까지 한 번의
`DONTWAIT` 시도 동안만 socket-local attempt gate를 유지한다. 실패하면 Core sequence
rollback 뒤 gate를 즉시 해제하고, `BACKPRESSURED` readiness 대기 중에는 보유하지 않는다.
Request submit도 raw send와 같은 짧은 socket-local attempt gate 아래에서 첫 request part부터
FINAL까지 한 번 시도한다. 이 gate는 새 Core multipart API나 공개 FIFO 계약이 아니다.

## 13. 구현 및 contract test 검증 요구

공개 표면(ROUTER send·request·receive·reply 함수, completion pull, ROUTER option set·get,
반환값·errno, event·status snapshot)만으로 다음을 확인한다. 각 항목은
test 하나로 이어진다.

**Option**
- `zlink_get_router_option()` 호출 시 `*optvallen_`은 입력 용량이며, 성공하면 실제로 쓴 byte 수로 갱신된다.
- 각 option의 기본값이 조회된다 — `MANDATORY` `1`, `PROBE` `0`, `REQUEST_TIMEOUT_MS` `5000`, `WEIGHT` `100`.
- `ZLINK_ROUTER_OPT_MANDATORY`가 양수이면 연결된 pipe가 없는 routing ID의 directed submit이 `ZLINK_SUBMIT_NOT_CONNECTED`로 실패하고, getter는 `0` 또는 `1`을 반환한다.
- `ZLINK_ROUTER_OPT_PROBE`가 양수이면 연결을 설정할 때 빈 raw message가 전송되어 peer가 연결과 routing ID를 관찰할 수 있고, getter는 `0` 또는 `1`을 반환한다.
- `ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID`를 connect 전에 설정하면 다음 `zlink_connect()`로 만든 pipe를 그 local alias로 식별한다.

**Peer weight 전달**
- Bind·connect 전에 ROUTER와 상대 peer의 weight를 서로 다른 값으로 설정하면 logical route가 ready 된
  뒤 양쪽 scheduler가 상대의 정확한 값을 사용하며, `0`인 peer는 outbound 후보에서 제외된다.
- Network와 inproc connection이 ready 된 뒤 양쪽 weight를 동적으로 바꾸면 monitor의
  `PEER_WEIGHT_CHANGED`가 새 값을 `value`로 제공하고 Application lane과 진단용 `connection_id`로
  값을 적용한 connection을 식별한다.
- Weight를 설정하거나 동기화해도 public receive와 Completion lane에는 application record가
  추가되지 않으며, 같은 값을 다시 설정해도 monitor event가 중복 발생하지 않는다.
- Application multipart가 열린 동안 weight를 여러 번 바꿔도 peer에는 multipart가 atomic record
  하나로 보이며, FINAL 또는 rollback 뒤에는 가장 최근 값만 반영된다.
- Application multipart의 첫 part를 받은 뒤 pipe의 remote weight가 `0`이 되어도 같은 pipe가
  FINAL까지 남은 part를 전달하고, 다음 message 선택부터 제외된다.
- Remote weight 변경은 같은 logical RID에 wait token이 있는 DONTWAIT SEND와 REQUEST를 다시
  평가한다. Weight가 `0`이 되어도 wait token은 끝나지 않는다. `0`에서 양수로 바뀌면 그 RID의
  SEND·REQUEST wait token에 WRITABLE record가 발행된다.
- Application 최대값을 10 byte보다 작게 설정해도 pair readiness·FLOWSTATE와 peer 선택·monitor로
  관찰하는 weight 변경은 막히지 않는다.
- Reconnect 뒤 peer 선택과 monitor는 새 connection의 현재 weight를 반영한다. Active standby를
  승격하면 그 standby가 마지막으로 받은 값을 사용한다.

**Record 구분과 receive**
- 일반 DATA record는 reply token `0`, 수신 REQUEST는 nonzero opaque reply token을 반환하며 wire sequence와 token 값의 일치는 계약이 아니다.
- 한 multipart record의 모든 part에는 같은 source routing ID와 reply token이 반복해 반환된다.
- `zlink_request_part()`로 시작한 request의 reply와 terminal failure는 data receive record가 아니라 `ZLINK_COMPLETION_REQUEST`로 반환된다.
- non-blocking receive에 받을 record가 없으면 `ZLINK_RECV_NO_DATA`와 `EAGAIN`이다.
- receive 성공 시 part 소유권이 호출자에게 이동해 `zlink_msg_close()` 한 번으로 해제하고, 실패 시 소유권은 이동하지 않는다.
- `has_more_out_ == ZLINK_PART_MORE`이면 다음 호출이 같은 record의 다음 part를 반환하고, `ZLINK_PART_FINAL`이면 record 수신이 끝난다.
- `zlink_router_recv_part()`에 reply나 error reply가 도착하면 payload를 반환하지 않고 `EPROTO`로 connection을 종료한다.
- `zlink_router_recv_part()`가 반환한 DATA 또는 REQUEST payload를 raw send에 다시 사용해도 request-reply 의미가 나타나지 않는다.
- 공통 `zlink_recv_part()`에 ROUTER를 넘기면 지원하지 않는 receive surface로 거부한다.
- 같은 RID의 서로 다른 physical source가 같은 live wire sequence REQUEST를 보내면 서로 다른 opaque reply token을 반환하고, reverse·out-of-order reply도 각 token이 가리키는 원래 request 하나만 완료한다.
- 같은 physical source가 live wire sequence를 중복해 보내면 `EPROTO`로 그 connection을 종료하고 duplicate REQUEST를 application receive에 전달하지 않는다.

**Part sequence**
- send API는 성공·실패 모두에서 `part_` 내용을 소비하고 길이 0인 초기화 상태로 둔다 — 실패 후에도 같은 `part_`로 재전송할 수 없다.
- 열린 sequence의 중간 또는 마지막 submit이 실패하면 peer에는 그 record의 어떤 part도 보이지 않고, 다음 submit은 새 record의 첫 part로 시작한다.
- 실패한 request submit은 completion ID `0`이고 completion과 context echo를 만들지 않는다.
- reply sequence가 실패해도 reply token과 source RID 조합은 successful `ZLINK_PART_FINAL`, logical
  RID 제거, responder socket close 또는 context termination 전까지 유효하며, 보관한 전체 reply를
  첫 part부터 다시 제출할 수 있다.
- Request나 reply의 첫 part에 non-empty group이 있으면 `ZLINK_SUBMIT_INVALID_ARGUMENT`와 `EINVAL`이며 입력 part는 소비되고 peer에는 그 record의 part가 전달되지 않는다. 실패한 request는 completion이 없으며 실패한 reply는 같은 source RID와 token으로 group이 없는 payload를 다시 제출할 수 있다.

**Directed submit**
- `zlink_send_part_rid()`의 `NONE FINAL`은 `SNDTIMEO` 안에서 같은 logical RID의 local admission을 기다리고 ID `0`과 completion 없음으로 끝난다.
- `DONTWAIT FINAL`이 즉시 admission되면 ID `0`과 completion 없음이다. HWM·credit 또는 준비되지 않은 route 때문에 거절되면 `ZLINK_SUBMIT_BACKPRESSURED`+`EAGAIN`과 그 RID의 nonzero wait token이며 payload는 유지되지 않는다.
- route가 없는 RID의 `DONTWAIT FINAL`은 `ZLINK_ROUTER_OPT_MANDATORY`가 양수일 때 즉시 `ZLINK_SUBMIT_NOT_CONNECTED`, ID `0`이고 token이 없다. option이 `0`이면 기존대로 조용히 버리고 `ZLINK_SUBMIT_OK`, ID `0`이다.
- 같은 RID에 write credit이 생기면 그 token의 `ZLINK_COMPLETION_WRITABLE` record(`ZLINK_SEND_ADMITTED`, `peer_rid`는 제출한 RID)를 정확히 한 번 반환하고 다른 RID의 credit은 이 token을 깨우지 않는다. 읽기 전까지 `ZLINK_POLLOUT`이 level로 유지된다.
- `zlink_disconnect_rid()`로 RID를 제거하면 그 RID의 token은 `ZLINK_SEND_TERMINAL`+`ENOENT`인 WRITABLE record로 끝난다.
- Wait token은 같은 logical RID에만 묶이고 reconnect 뒤 그 RID의 pipe attach가 WRITABLE record를 발행하며, ID `0` 뒤에는 payload를 replay하지 않는다.
- Multipart 첫 part가 수용된 뒤 실패하면 전체 record가 rollback되어 peer에 부분 record가 보이지 않는다.

**Request completion**
- Admission된 FINAL은 nonzero REQUEST ID를 반환하고 reply·timeout·terminal 중 하나를 정확히 한 REQUEST completion으로 반환하며 wait token 없는 submit 실패는 ID `0`과 completion 없음으로 끝난다.
- DONTWAIT FINAL은 admission을 한 번만 시도한다. Backpressure나 준비되지 않은 route(transport pair 미준비, weight `0`)는 `ZLINK_SUBMIT_BACKPRESSURED`+`EAGAIN`과 그 RID의 nonzero wait token을 반환하고, 같은 token·context·`peer_rid`의 WRITABLE record 뒤 caller가 같은 request를 다시 제출한다. Mandatory route가 없는 RID는 `ZLINK_SUBMIT_NOT_CONNECTED`+`EHOSTUNREACH`, ID `0`, token 없음이다.
- 마지막 호출의 `timeout_ms_ == 0`은 `ZLINK_ROUTER_OPT_REQUEST_TIMEOUT_MS` 기본값을 사용한다.
- 유효한 error reply는 errno를 매핑한 non-OK `zlink_request_result_t`와 errno part 뒤의 payload를 completion에 보존하며, malformed errno part는 `ZLINK_REQUEST_PROTOCOL_ERROR`와 payload 없음으로 완료한다.
- Request timeout은 local admission부터 시작하고 wait token이 유지되는 동안은 시작하지 않으며, admission 뒤 submit 시점 pair가 종료되면 timeout을 기다리지 않고 즉시 `ZLINK_REQUEST_NOT_CONNECTED` completion 하나를 받는다.
- 공유 completion slot 포화는 flags와 관계없이 즉시 `ZLINK_SUBMIT_BACKPRESSURED`+`EAGAIN`, ID `0`, completion 없음으로 실패한다.

**Reply**
- `zlink_reply_part()`는 수신 record가 반환한 source RID와 opaque reply token을 사용하며 successful FINAL만 token을 소비한다.
- Reply FINAL은 `SNDTIMEO`를 snapshot해 logical reply route의 local admission을 기다리고, timeout이면 `ZLINK_SUBMIT_BACKPRESSURED`+`EAGAIN`이면서 live token으로 전체 reply를 처음부터 재시도할 수 있다.
- 같은 token의 concurrent second sequence는 `ZLINK_SUBMIT_INVALID_STATE`+`EBUSY`로 second part만 소비하고 기존 checkout과 staging을 유지한다.
- Physical disconnect·connection generation 변경·requester timeout은 token을 무효화하지 않고, logical RID 제거·responder close·context termination은 무효화한다.
- Live token이 65,536개면 새 REQUEST를 조용히 drop하지 않고 해당 source의 read를 멈추며, slot 해제 뒤 round-robin으로 다시 진행한다.
- Application queue가 비었고 모든 readable head가 token-blocked REQUEST이면 `ZLINK_POLLIN`이
  내려가며, slot 해제 뒤 source들을 round-robin으로 redrive해 starvation이 없다.
- Reply submit은 completion ID나 completion record를 만들지 않는다.
- DEALER peer에 대한 reply는 현재 ready Application pipe를 사용하고 HWM·PAUSED와 `SNDTIMEO`
  admission을 적용하므로 `ZLINK_SUBMIT_BACKPRESSURED`+`EAGAIN`이 될 수 있다. ROUTER peer에 대한
  reply는 현재 ready Completion pipe를 사용하며 HWM-free admission을 적용한다.

**Readiness**
- `ZLINK_POLLIN`은 완전한 raw record를 수신할 수 있을 때 서고, `ZLINK_POLLOUT`은 backpressure 뒤 재시도 가치를 나타낼 뿐 다음 submit 성공을 보장하지 않는다.
- 읽지 않은 `ZLINK_COMPLETION_WRITABLE` record가 있는 동안 `ZLINK_POLLOUT`과 `ZLINK_POLLCOMPLETION`이 level로 유지되고, `NO_DATA`까지 drain하면 내려간다.
- readiness 계약은 raw reply에 적용되지 않는다.

**Receive flow state**
- 현재 상태를 다시 설정하면 성공하고 아무것도 보내지 않는다.
- 한 번의 호출로 모든 ready peer가 같은 상태를 받는다. DEALER peer에는 Application connection,
  ROUTER peer에는 Completion connection을 사용하며 나중에 ready가 된 peer도 socket의 현재 상태를 받는다.
- Flow-state frame에는 자신이 기록된 connection 범위의 flow epoch만 있고 public pair ID·generation field와 `Zlink-Pair-Id`·`Zlink-Pair-Generation` wire property가 없다. Core는 내부 connection identity가 일치하는 기록 connection에만 frame을 적용한다.
- 대체된 connection의 frame을 포함해 identity가 일치하지 않는 frame은 public event 없이 내부에서 소비되고 `flow_state_stale_total`에만 반영된다. 같은 connection의 중복·역행 epoch는 적용되지 않고 `ZLINK_EVENT_FLOW_STATE_STALE`과 `ZLINK_MONITOR_EVENT_FLAG_FLOW_STATE_STALE_EPOCH`으로 보고된다.
- peer가 재연결 전에 알린 상태는 그것을 대체한 connection에 적용되지 않는다.
- Remote PAUSE는 pause한 peer로 보내는 전송만 막는다 — 다른 peer로 가는 route는 영향이 없고, 차단된 non-blocking send는 `errno == EAGAIN`과 함께 `ZLINK_SUBMIT_BACKPRESSURED`를 보고하며, PAUSE 해제만으로 다음 send가 수락되지는 않는다.
- Remote PAUSE는 다음 message 경계부터 적용된다 — routing ID part를 이미 수락한 record는 남은 part를 끝까지 보낸 뒤 pause가 적용된다.
- [Monitoring](../06-monitoring.ko.md) status snapshot이 현재 pause 상태인 peer 수, socket 전체의 적용된 전이 수, stale 수, pause 길이를 제공한다.

<!-- zlink-nav:start -->
[소켓 목차](README.ko.md) | [이전: DEALER](06-dealer.ko.md) | [다음: STREAM](08-stream.ko.md)
<!-- zlink-nav:end -->
