---
title: "소켓 — DEALER"
---

[English](https://zlink-systems.github.io/zlink/spec/core/socket/06-dealer/) | 한국어

<!-- zlink-nav:start -->
[소켓 목차](README.ko.md) | [이전: XSUB](05-xsub.ko.md) | [다음: ROUTER](07-router.ko.md)
<!-- zlink-nav:end -->

# 소켓 — DEALER

> **이 장이 정의하는 것** — DEALER 소켓의 request 라우팅과 [result/errno](../04-errno-map.ko.md)
> 공개 계약.

DEALER는 여러 peer에서 공정 큐잉으로 수신하고, 연결된 peer에 순환 또는 가중치 기반으로 송신하는
비동기 raw socket이다. 일반 raw message와 request/reply record를 같은 socket에서 처리할 수 있다.

## 1. 공개 타입

다음 숫자는 공개 ABI 값이다.

```c
typedef enum zlink_dealer_option_t {
  ZLINK_DEALER_OPT_PROBE              = 0x3201,
  ZLINK_DEALER_OPT_REQUEST_TIMEOUT_MS = 0x3202,
  ZLINK_DEALER_OPT_WEIGHT             = 0x3203
} zlink_dealer_option_t;

typedef enum zlink_dealer_message_type_t {
  ZLINK_DEALER_MESSAGE_RAW         = 0,
  ZLINK_DEALER_MESSAGE_REQUEST     = 1,
  ZLINK_DEALER_MESSAGE_REPLY       = 2,
  ZLINK_DEALER_MESSAGE_ERROR_REPLY = 3
} zlink_dealer_message_type_t;

typedef enum zlink_part_flag_t {
  ZLINK_PART_FINAL = 0,
  ZLINK_PART_MORE  = 1
} zlink_part_flag_t;

typedef void (*zlink_reply_handler_fn)(
  zlink_request_result_t result_,
  zlink_msg_t *parts_,
  size_t part_count_,
  void *userdata_);
```

`ZLINK_PART_MORE`는 같은 multipart record에 뒤따르는 part가 있음을 뜻한다.
`ZLINK_PART_FINAL`은 현재 part가 그 record의 마지막임을 뜻한다. receive API의
`has_more_out_`도 같은 두 값을 사용한다.

## 2. DEALER option

```c
ZLINK_EXPORT zlink_config_result_t zlink_set_dealer_option(
  void *handle_,
  zlink_dealer_option_t option_,
  const void *optval_,
  size_t optvallen_);

ZLINK_EXPORT zlink_config_result_t zlink_get_dealer_option(
  void *handle_,
  zlink_dealer_option_t option_,
  void *optval_,
  size_t *optvallen_);
```

| 상수 | 값 형식 | 의미 |
|---|---|---|
| `ZLINK_DEALER_OPT_PROBE` | `int`, `0` 또는 `1` | 연결을 설정할 때 빈 raw message를 보내 peer가 연결과 routing ID를 관찰할 수 있게 한다. 기본값은 `0`이다 |
| `ZLINK_DEALER_OPT_REQUEST_TIMEOUT_MS` | 0 이상인 `int`, millisecond | request API에서 `timeout_ms_ == 0`일 때 사용할 기본 timeout을 정한다. 기본값은 `5000`이다 |
| `ZLINK_DEALER_OPT_WEIGHT` | `int`, `0..10000` | 연결된 peer에 알리는 이 DEALER의 가중치다. 기본값은 `100`이다 |

`zlink_get_dealer_option()`을 호출할 때 `*optvallen_`은 `optval_`의 입력 용량이다. 성공하면 실제로
쓴 byte 수로 갱신된다. DEALER 전용이 아닌 HWM, reconnect와 timeout option은
`zlink_set_option()`과 `zlink_get_option()`을 사용한다.

`0..10000` 밖의 가중치는 거부하며 clamp하지 않는다. `0..100` 값의 의미는 범위를 넓히기 전과
같다.

### 2.1 Outbound peer 선택

후보는 양수 가중치를 알린 연결된 outbound peer다. 가중치가 `0`인 peer는 후보에서 제외한다.
알려진 peer의 가중치가 모두 `0`이면 submit은 `ZLINK_SUBMIT_NOT_ADMITTED`로 실패할 수 있다.

각 후보는 `0`에서 시작하는 누적값을 갖는다. message 하나를 보낼 때 다음 선택 절차를 한 번
수행한다.

1. 모든 후보의 누적값에 자기 가중치를 더한다.
2. 누적값이 가장 큰 후보를 고른다. 누적값이 같은 후보가 여럿이면 식별자가 가장 작은 후보를
   고른다.
3. 고른 후보의 누적값에서 후보 전체의 가중치 합을 뺀다.

가중치가 같은 경우도 별도 규칙이 아니다. 같은 가중치를 가진 후보도 같은 세 단계를 따르므로
번갈아 선택된다. 후보가 하나뿐인 경우도 같은 절차가 적용된다. 같은 값을 더하고 빼므로
누적값이 그대로이기 때문이다.

이 절차는 한 후보의 몫을 한 구간에 몰아주지 않고 연속 선택을 흩뿌린다. 가중치가 `100`과
`300`이면 반복되는 순서는 `두 번째, 첫 번째, 두 번째, 두 번째`이며, 무거운 peer에 세 번
연속 보낸 뒤 가벼운 peer에 한 번 보내는 순서가 아니다. message가 충분히 쌓이면 선택 빈도가
설정한 비율과 일치한다.

선택 절차는 peer가 실제로 받은 message에만 적용한다. 고른 후보가 쓰기 여유가 없어 받지
못하면 그 message에 한해 후보에서 빠지고, 절차는 대신 받아들인 peer에 적용한다. 이 실패는
설정한 가중치를 바꾸지 않으며, 그 peer는 쓰기 여유를 다시 알리면 후보로 돌아온다. 크기 제한을
넘어 거부된 message는 다른 후보로 다시 시도하지 않는다. 어느 후보든 같은 이유로 거부하기
때문이다.

2단계의 식별자는 peer routing ID이며 byte 열로 비교한다. routing ID가 없으면 빈 byte 열이므로
비어 있지 않은 모든 식별자보다 앞선다. 식별자가 같은 peer는, routing ID가 모두 없는 경우를
포함해, 연결이 성립한 endpoint 순으로 정렬하고, endpoint까지 같으면 로컬에서 연결이 붙은
순서로 정렬한다. 재연결은 누적값이 `0`에서 시작하는 새 연결을 만든다. 식별자는 그대로이므로
정렬 위치는 이전과 같다.

같은 peer와 같은 가중치로 설정한 두 process는 같은 선택 순서를 낸다. 후보 식별자가 서로 다른
한 application은 이 순서에 의존할 수 있다. 정렬이 로컬 연결 순서까지 내려가는 경우에는 한
process 안에서는 결정적이지만 process 사이에서는 재현되지 않는다.

후보 목록이 바뀌면 남은 후보는 누적값을 그대로 유지하므로 설정한 비율이 보존된다. 새 연결은
`0`에서 시작하고, 연결이 끊긴 peer는 연결과 함께 누적값을 버린다. backpressure나 가중치 `0`
때문에 일시적으로만 빠진 peer는 누적값을 유지하며, 다시 후보가 되면 그 값에서 이어간다.

## 3. Message record 구분

`zlink_dealer_recv_part()`는 payload part와 함께 record 종류와 request sequence를 반환한다.

| `message_type_out_` 값 | `request_seq_out_` | 의미 |
|---|---:|---|
| `ZLINK_DEALER_MESSAGE_RAW` (`0`) | `0` | request/reply envelope가 없는 일반 raw multipart message |
| `ZLINK_DEALER_MESSAGE_REQUEST` (`1`) | 0이 아닌 값 | 이 DEALER가 수신한 request다. 반환된 값은 `zlink_dealer_reply_part()`에 넘길 reply token이다 |
| `ZLINK_DEALER_MESSAGE_REPLY` (`2`) | 0이 아닌 값 | 성공 reply record |
| `ZLINK_DEALER_MESSAGE_ERROR_REPLY` (`3`) | 0이 아닌 값 | 실패 reply record |

한 multipart record의 모든 part에는 같은 message type과 request sequence가 반환된다. request API로
시작한 작업의 reply와 terminal failure는 `zlink_reply_handler_fn` completion으로 전달된다. 일반 raw
message를 보낼 때는 다음 API를 사용하며 request sequence를 만들지 않는다.

```c
ZLINK_EXPORT zlink_submit_result_t zlink_send_part(
  void *s_,
  zlink_msg_t *part_,
  zlink_send_flags_t flags_,
  zlink_part_flag_t part_flag_);
```

## 4. Part sequence와 소유권

`*_part` send 호출은 `ZLINK_PART_MORE`부터 `ZLINK_PART_FINAL`까지 하나의 multipart sequence를
구성한다. 열린 sequence가 있는 동안 같은 handle에서 다른 send helper family를 섞을 수 없다.

초기화된 유효한 `part_`를 send API에 넘기면 함수는 성공과 실패 모두에서 그 message 내용을
소비한다. 따라서 호출 결과와 관계없이 호출자가 전송 전 payload를 다시 읽거나 같은 내용을 다시
보낼 수 없다. 다시 보내야 하는 payload는 호출 전에 별도 message로 보관해야 한다.

각 send helper family는 성공한 중간 파트를 `ZLINK_PART_FINAL`이 성공할 때까지 하나의 record로
staging한다. 열린 sequence의 중간 또는 마지막 submit이 실패하면 Core는 이전에 staging한 파트와
실패한 파트를 원자적으로 폐기하고 sequence를 닫는다. peer에는 그 record의 어떤 파트도 보이지
않는다. 실패한 호출의 `part_`도 소비되며 다음 submit은 새 record의 첫 파트로 시작한다. 실패한
request submit은 request sequence를 만들지 않고 handler도 호출하지 않는다. reply sequence가 실패하면
reply token은 성공한 `ZLINK_PART_FINAL`이나 request lifecycle 종료 전까지 유효하므로 caller가 보관한 전체 reply를
첫 파트부터 다시 제출할 수 있다.

receive API의 `part_out_`은 호출 전에 초기화된 `zlink_msg_t`여야 한다. 성공하면 수신 part의
소유권이 호출자에게 이동하며 호출자는 `zlink_msg_close()`로 정확히 한 번 해제한다. 실패하면
수신 part 소유권은 이동하지 않는다.

### 4.1 Exact target raw submit

```c
ZLINK_EXPORT zlink_submit_result_t zlink_dealer_send_transport_pair_part(
  void *dealer_,
  const zlink_routed_submit_target_t *target_,
  zlink_msg_t *part_,
  zlink_send_flags_t flags_,
  zlink_part_flag_t part_flag_);
```

`target_`은 같은 DEALER에서 `zlink_select_routed_submit_target()`으로 얻은 값이다. Core는
RID·transport pair ID·generation이 현재 연결된 같은 application pipe를 가리키는지 한 번
검증하고 그 pipe에만 제출한다. HWM이면 `ZLINK_SUBMIT_BACKPRESSURED`, detach나 stale generation이면
`ZLINK_SUBMIT_NOT_CONNECTED`이며 다른 pipe로 재선택하지 않는다. 첫 파트가 성공하면 그 exact pipe
fence를 FINAL까지 유지한다. 중간 또는 마지막 파트 실패는 앞서 staging한 전체 record를 rollback하고
sequence를 닫으므로 peer에는 부분 record가 보이지 않는다.
Part 호출마다 public API scope가 따로이므로 binding은 한 번의 nonblocking multipart
시도 동안만 자기 socket-local attempt gate를 유지해 다른 binding submit의 interleave를
막는다. `BACKPRESSURED` 뒤 readiness를 기다릴 때는 gate를 해제한다. 이는 새 Core
multipart API나 공개 FIFO 계약을 만들지 않는다.

## 5. Raw request submit

```c
ZLINK_EXPORT zlink_submit_result_t zlink_dealer_request_part(
  void *dealer_,
  zlink_msg_t *part_,
  zlink_send_flags_t flags_,
  zlink_part_flag_t part_flag_,
  uint32_t timeout_ms_,
  zlink_reply_handler_fn handler_,
  void *userdata_);
```

하나의 비동기 request payload를 part 단위로 제출한다. 중간 part는 `part_flag_`를
`ZLINK_PART_MORE`로 두고 `timeout_ms_ == 0`, `handler_ == NULL`, `userdata_ == NULL`로 호출한다.
마지막 part는 `ZLINK_PART_FINAL`과 0이 아닌 `handler_`를 사용한다. 마지막 호출의
`timeout_ms_ == 0`은 `ZLINK_DEALER_OPT_REQUEST_TIMEOUT_MS` 기본값을 사용한다. `flags_`는
`ZLINK_SEND_FLAGS_NONE` 또는 `ZLINK_SEND_FLAGS_DONTWAIT`다.

마지막 submit이 `ZLINK_SUBMIT_OK`이면 completion은 정확히 한 번 `handler_`로 전달된다. submit이
실패하면 handler를 호출하지 않는다. callback의 `parts_`와 각 message의 소유권은 callback으로
이동하며 callback은 이를 정확히 한 번 해제한다. timeout과 다른 terminal result에서는
`zlink_request_result_t`가 결과를 나타낸다.

Exact target request는 다음 API를 사용한다.

```c
ZLINK_EXPORT zlink_submit_result_t zlink_dealer_request_transport_pair_part(
  void *dealer_,
  const zlink_routed_submit_target_t *target_,
  zlink_msg_t *part_,
  zlink_send_flags_t flags_,
  zlink_part_flag_t part_flag_,
  uint32_t timeout_ms_,
  zlink_reply_handler_fn handler_,
  void *userdata_);
```

Target 검증·multipart fence·실패 rollback은 exact raw submit과 같다. Core는 request envelope가
wire에 보이기 전에 pending correlation과 timeout lifecycle을 등록한다. Final submit이 실패하면
그 pending entry와 completion reservation을 제거하고 handler를 호출하지 않는다. Submit이 성공한
뒤 빠른 reply가 도착해도 correlation 등록보다 앞설 수 없다. Binding은 raw send와 같은
짧은 socket-local attempt gate 아래에서 첫 request part부터 FINAL까지 한 번만 시도하고,
대기 전에는 gate를 해제한다.

## 6. Raw record receive

```c
ZLINK_EXPORT zlink_recv_result_t zlink_dealer_recv_part(
  void *dealer_,
  uint8_t *message_type_out_,
  uint64_t *request_seq_out_,
  zlink_msg_t *part_out_,
  zlink_part_flag_t *has_more_out_,
  zlink_recv_flags_t flags_);
```

완전한 record에서 part 하나를 반환한다. 모든 output pointer는 필수다. `message_type_out_`의 C 타입은
`uint8_t`이며 값은 `zlink_dealer_message_type_t`에 정의된 숫자 중 하나다. `flags_`는
`ZLINK_RECV_FLAGS_NONE` 또는 `ZLINK_RECV_FLAGS_DONTWAIT`다. non-blocking 호출에 받을 record가 없으면
`ZLINK_RECV_NO_DATA`와 `EAGAIN`을 반환한다.

`has_more_out_ == ZLINK_PART_MORE`이면 다음 호출로 같은 record의 다음 part를 받아야 한다.
`ZLINK_PART_FINAL`이면 record 수신이 끝난다.

## 7. Raw reply submit

```c
ZLINK_EXPORT zlink_submit_result_t zlink_dealer_reply_part(
  void *dealer_,
  uint64_t request_seq_,
  zlink_msg_t *part_,
  zlink_part_flag_t part_flag_);
```

`ZLINK_DEALER_MESSAGE_REQUEST` record에 reply part를 보낸다. `request_seq_`는 같은 socket의
`zlink_dealer_recv_part()`가 그 request에 대해 반환한 0이 아닌 reply token이어야 한다. 여러 part로
reply할 때 모든 호출에서 같은 token을 사용한다. `ZLINK_PART_FINAL`이 성공하면 그 token의 reply가
완료되며 다시 사용할 수 없다.

Raw reply와 error reply는 completion progress lane에 한 번만 submit한다. 이 lane은 application
byte HWM, manual HWM, LWM과 Core budget reservation의 대상이 아니므로 이 함수는 그 capacity를
이유로 `ZLINK_SUBMIT_BACKPRESSURED`를 반환하지 않으며 `ZLINK_POLLOUT` 또는 send-ready callback을
기다려 재시도하지 않는다. 연결, lifecycle, argument, state와 allocation failure는 호출 시점의
해당 `zlink_submit_result_t`로 즉시 끝난다.

## 8. Result와 readiness

submit은 `zlink_submit_result_t`, receive는 `zlink_recv_result_t`, option은
`zlink_config_result_t`를 반환한다. 각 result와 `zlink_errno()`의 대응은
[errno map](../04-errno-map.ko.md)을 따른다.

DEALER의 `ZLINK_POLLIN`은 raw 또는 request/reply record를 수신할 수 있음을 뜻한다. Ordinary send와
request에서 `ZLINK_POLLOUT`과 `zlink_send_ready_handler()`는 backpressure 뒤 submit을 다시 시도할
가치가 있음을 나타내지만 다음 submit 성공을 보장하지 않는다. 이 readiness 계약은 raw reply에
적용하지 않는다.

## 9. Receive flow state

Completion lane으로 ROUTER와 pair를 이룬 DEALER는 자신에게 보내는 peer에게 전송을 멈추고
다시 시작하라고 요청할 수 있다. `zlink_socket_set_receive_flow_state()`는 socket 전체에
적용되는 상태 하나를 저장하고 이 socket의 ready pair마다 completion lane으로 보낸다. 함수
선언은 [Socket 공통](README.ko.md)이, 결과 표는 [Errors](../03-errors.ko.md)가 소유한다.

이 상태는 counter가 아니라 절대값이다. `ZLINK_RECEIVE_FLOW_PAUSED`를 두 번 설정해도 pause는
하나이며 두 번째 호출은 아무것도 보내지 않고 성공한다. 중첩 count도 없고 그만큼 resume을
해야 하는 규칙도 없다. Socket은 상태를 정확히 하나만 유지하므로 한 peer만 pause하고 다른
peer는 running으로 둘 수 없다.

각 상태 변경은 한 connection generation 안에서 증가하는 flow epoch를 함께 보내고, frame은
자신이 기록된 connection의 pair id와 generation도 담는다. 수신 socket은 frame이 현재 pair와
generation을 지칭하고 epoch가 그 generation에서 마지막으로 수락한 epoch보다 클 때만
적용한다. 그 밖의 frame은 stale로 무시하며 Core는 적용 대신
`ZLINK_EVENT_FLOW_STATE_STALE`로 보고한다. 따라서 이전 generation의 frame이 그것을 대체한
connection에 적용되는 일은 없다.

Pair가 ready가 되면 Core는 socket의 현재 상태를 새 completion lane으로 보낸다. 이 socket이
pause한 동안 연결하거나 재연결한 peer는 추가 호출 없이 pause를 알게 된다. 상태를 한 번도
설정하지 않은 socket은 아무것도 보내지 않는다. 새 pair는 이미 RUNNING을 가정하기 때문이다.

Remote PAUSE는 그 peer로 보내는 전송을 막는다. 이는 기존 차단 요인과 합성되는 독립적인
차단 요인이다. Byte HWM, transport wait와 termination도 각각 그대로 전송을 막으며, 어느
것도 해당하지 않을 때만 send가 수락된다. 따라서 remote pause를 해제해도 그것만으로 다음
send가 성공하지는 않는다. Send 결과와 readiness는 그대로다. 차단된 non-blocking send는
계속 `errno == EAGAIN`과 함께 `ZLINK_SUBMIT_BACKPRESSURED`를 보고하고, `ZLINK_POLLOUT`과
`zlink_send_ready_handler()`는 8절이 정의한 의미를 유지한다.

Remote PAUSE는 다음 message 경계에서 적용되며 message를 쪼개지 않는다. 첫 byte가 이미
pipe에 도달한 message와 socket이 첫 part를 이미 수락한 message는 남은 part를 끝까지 보내고,
pause는 그다음 message부터 적용한다.

[Monitoring](../07-monitoring.ko.md)의 status snapshot은 이 socket이 현재 pause 상태로 보는
peer 수, 적용한 pause와 resume 전이 수, stale로 거부한 frame 수, 가장 최근에 끝난 pause의
길이를 제공한다.
