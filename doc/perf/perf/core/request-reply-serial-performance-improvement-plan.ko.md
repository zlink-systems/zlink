# core request/reply serial 성능 개선 계획

> 이 문서는 `zlink` C API request/reply의 serial 패턴에서 보이는 낮은 처리량을
> 분석하고 개선하기 위한 계획이다.
>
> 이 문서는 공개 API 계약 문서가 아니다. 아직 확인되지 않은 동작을 spec으로
> 보장하지 않으며, 먼저 측정으로 병목 위치를 분리한 뒤 필요한 계층에서 수정한다.

## 1. 문제 요약

`bindings/c/bench/with_grpc`의 최신 local 비교 결과에서 `zlink` request/reply는
window와 saturation 패턴에서는 gRPC보다 높게 나오지만, serial 패턴에서만 약
`0.92 KOPS` 수준으로 낮다.

기준 리포트:

- 디렉터리: `bindings/c/bench/with_grpc/log/with_grpc_c_20260706_194816_c_with_grpc_patterns/`
- 파일: `with_grpc_c_20260706_194816_c_with_grpc_patterns.txt`

핵심 결과:

| 패턴 | 크기 | 처리량 | 평균 latency | blocked | max outstanding |
|------|------|--------|--------------|---------|-----------------|
| zlink request serial | 1024 | `0.923 KOPS` | `1.082 ms` | `0` | `1` |
| zlink request window | 1024 | `85.876 KOPS` | `1.144 ms` | `0` | `100` |
| zlink request saturation | 1024 | `176.825 KOPS` | `3.183 ms` | `76787` | `646` |
| zlink request serial | 4096 | `0.921 KOPS` | `1.085 ms` | `0` | `1` |
| zlink request window | 4096 | `84.675 KOPS` | `1.168 ms` | `0` | `100` |
| zlink request saturation | 4096 | `116.490 KOPS` | `5.841 ms` | `91167` | `754` |

이 결과에서 중요한 점은 아래와 같다.

- serial 처리량의 역수와 평균 latency가 거의 같다. 요청 하나가 완료될 때까지
  약 `1.08 ms`가 걸린다.
- 1024바이트와 4096바이트 serial 결과가 거의 같으므로 payload copy나 전송량
  자체가 주된 병목이라고 보기 어렵다.
- serial과 window에서는 `blocked`가 `0`이다. backpressure는 saturation에서
  자연스럽게 나타나는 동작이며, 이번 문제의 주 원인으로 보지 않는다.
- 1024바이트 serial의 `submit_wait_ms`는 전체 `24.763 ms`이고 submit 수는
  `2770`이다. submit 한 번당 약 `9 us` 수준이므로 `zlink_dealer_request_part()`
  호출 자체가 `1 ms`를 대부분 소비한다고 보기는 어렵다.
- 같은 실행에서 gRPC request serial은 약 `13 KOPS` 수준이다. 따라서 순수한 OS
  scheduler wakeup 한계만으로 zlink serial의 `0.92 KOPS`를 설명하기 어렵다.

따라서 우선 의심할 경로는 request submit 이후 reply completion을 기다리는 경로다.
특히 core socket command 처리에는 약 `1 ms` 수준의 throttle 정책이 있으므로,
reply가 completion queue에 들어가기 전 activate/read command가 이 정책에 묶이는지
먼저 확인한다.

이 문서는 한 가지 원인을 미리 확정하지 않는다. 확인 대상은 크게 네 구간이다.

1. server가 request를 받고 reply를 다시 보내기까지의 blocking wakeup 구간
2. client I/O thread가 reply frame을 받아 request/reply dispatch를 실행하는 구간
3. completion queue가 internal PAIR socket으로 다시 signal하는 구간
4. application thread의 `zlink_poller_wait()`가 signal을 보고 callback을 drain하는 구간

## 2. 현재 serial 벤치 의미

with_grpc 벤치의 serial request는 한 번에 request 하나만 outstanding 상태로 둔다.
request가 완료되기 전까지 다음 request를 보내지 않는다.

관련 코드:

```text
bindings/c/bench/with_grpc/zlink/bench_zlink_client.cpp
```

- `submit_request_once()`는 payload를 만들고 `zlink_dealer_request_part()`를 호출한다.
- `run_request_serial()`은 request 하나를 보낸 뒤 `outstanding == 0`이 될 때까지
  `poll_once(poller, 50)`을 반복한다.
- `poll_once()`는 `zlink_poller_wait()`를 호출한다.

`50 ms`는 최대 대기 시간이다. completion signal이 정상적으로 poller를 깨우면
즉시 return해야 한다. 그러므로 serial에서 약 `1 ms`가 반복된다면, 단순히 timeout
값이 `50 ms`라서 느린 것은 아니다. completion이 poller를 즉시 깨우지 못하거나,
poller가 깨우기 전에 reply completion이 만들어지는 경로가 지연되는지 확인해야 한다.

기존 `bindings/c/perf/single`의 reqrep 구현은 이 serial 의미와 다르다. single perf의
reqrep requester는 가능한 만큼 request를 계속 넣고, 중간중간 completion을 poll한다.
따라서 `1 ms` 수준의 completion 지연이 있어도 여러 request에 나뉘어 보이며,
with_grpc serial처럼 outstanding 1개에서 병목이 그대로 드러나지 않는다.

## 3. 우선 병목 후보

### 3.1 command polling throttle 때문에 activate_read 처리가 늦는 경로

core socket에는 command 처리 빈도를 제한하는 정책이 있다. 이 정책은 처리량을 위해
command polling을 매번 하지 않고, 일정 시간 안에는 command 확인을 건너뛸 수 있게 한다.

관련 코드:

```text
core/src/runtime/utils/config.hpp
core/src/runtime/sockets/common/socket_base_lifecycle.cpp
core/src/runtime/sockets/common/socket_base_msg.cpp
core/src/runtime/core/pipe.cpp
```

현재 코드에서 `max_command_delay`는 CPU tick 기준이며, 주석은 이 값이 현재 CPU에서
약 `1 - 2 ms`가 될 수 있다고 설명한다. public send hot path는
`send_direct_with_retry()`에서 `process_commands(0, true)`를 호출한다. 여기서 두 번째
인자가 `true`이므로 command poll이 throttle 대상이 된다.

request/reply serial에서는 request 하나를 보낸 뒤 reply completion 하나를 기다린다.
이때 pipe flush가 sleeping reader를 깨워야 하는 상황이면 `activate_read` command가
전달된다. 이 command 처리가 `max_command_delay`에 묶이면 outstanding 1개인 serial에서는
그 지연이 request마다 그대로 latency가 된다. window와 saturation에서는 여러 request가
같은 지연 구간 안에 겹치므로 throughput이 회복될 수 있다.

확인할 질문:

- serial request마다 `process_commands(0, true)`가 command poll을 건너뛰는가?
- reply를 만들거나 completion signal을 전달하는 `activate_read` command가 약 `1 ms`
  뒤에 처리되는가?
- `max_command_delay`를 진단 빌드에서 0으로 줄이면 serial 처리량이 즉시 회복되는가?

수정 계층 후보:

`core/src/runtime/sockets/common`, `core/src/runtime/core/pipe`

#### 3.1.1 코드 추적 결과 — 우선순위 하향

실제로 `activate_read`를 만들고 전달하는 경로를 끝까지 따라가면 이 throttle과는
분리된다는 근거가 나온다.

- `process_commands(0, true)`(`throttle_=true`)를 호출하는 지점은 코드 전체에서
  `core/src/runtime/sockets/common/socket_base_msg.cpp:137`
  (`send_direct_with_retry()` 내부) 단 한 곳뿐이다. 나머지 모든 `process_commands`
  호출(recv/poll 경로 포함, 같은 파일 183/239/315/330/346/385/400/416/455/471/487행)은
  전부 `throttle_=false`다. 즉 이 throttle은 "보내는 소켓이 자신의 관리용
  mailbox(bind/term 등)를 매번 체크할지"만 결정하며, poll/recv 쪽 readiness 확인에는
  적용되지 않는다.
- reply 도착을 상대에게 알리는 `send_activate_read()`는
  `core/src/runtime/core/pipe.cpp:1021-1030`(`pipe_t::flush_unlocked()`)에서
  **매 `write_and_flush()`마다 무조건, 동기적으로** 호출된다(1027-1029행:
  `_out_pipe`가 flush 결과 sleeping이면 즉시 `send_activate_read(_peer)`). 이 호출은
  `process_commands(0, true)` throttle과 무관한 별도 경로다.
- `send_activate_read` → `object_t::send_command`(`core/src/runtime/core/object.cpp:260-267`)
  → `ctx_t::send_command`(`core/src/runtime/core/ctx.cpp:762-767`)는
  `mailbox(tid)->send(command_)`로 상대 mailbox에 즉시 push+signal하는 구조이며,
  이 경로에서도 지연을 만드는 지점은 보이지 않는다.

따라서 "activate_read 처리가 `max_command_delay`에 묶인다"는 가설은 코드 추적만으로는
근거가 약하다. 4.1 실험은 여전히 빠르게 배제하기 위해 돌려볼 가치가 있지만, 결과가
"변화 없음"으로 나올 가능성을 염두에 두고, 3.2/3.3/3.5(특히 아래 3.7)를 먼저 검증하는
순서를 권장한다.

### 3.2 reply 수신 뒤 internal dispatch가 늦게 실행되는 경로

reply completion은 request/reply internal dispatch가 reply frame을 처리한 뒤
completion queue에 넣는다.

관련 코드:

```text
core/src/api/socket/socket_request_reply_dispatch.cpp
```

- pending request 조회
- timeout task cancel
- reply completion decode
- `queue_reply_completion()` 호출

이 경로가 즉시 실행되지 않으면 poller는 completion signal을 받을 수 없다. serial에서는
이 지연이 request마다 그대로 처리량 제한이 된다. window나 saturation에서는 여러 request가
같은 지연 구간 안에 겹치므로 throughput이 회복될 수 있다.

확인할 질문:

- server가 reply를 보낸 뒤 client socket의 internal dispatch가 언제 실행되는가?
- reply frame이 socket에 도착했는데 dispatch가 다음 runtime poll tick까지 밀리는가?
- dispatch 실행 간격에 `1 ms` 수준의 고정 granularity가 있는가?

이 후보는 3.1과 연결해서 본다. reply frame이 도착했는데 socket message dispatch가
늦게 실행된다면, 그 원인이 runtime I/O event 지연인지, socket command progress 지연인지
분리해야 한다.

수정 계층 후보:

`core/src/runtime/sockets`, `core/src/api/socket`

### 3.3 completion queue signal 또는 poller wakeup이 즉시 전달되지 않는 경로

completion queue는 pending queue가 비어 있던 상태에서 새 completion이 들어오면
signal socket에 byte를 쓴다. `ZLINK_POLLCOMPLETION`으로 등록한 poller는 이 hidden
completion signal을 감지하고 completion을 drain한다.

관련 코드:

```text
core/src/api/socket/request_completion_queue_internal.cpp
core/src/api/monitoring/poller_api.cpp
core/src/api/core/zlink.cpp
core/src/api/socket/internal_pair_queue_internal.cpp
```

현재 코드 의도는 completion enqueue 시 poller가 즉시 깨어나는 것이다. 다만 completion
signal은 OS eventfd에 직접 byte를 쓰는 구조가 아니라 internal PAIR socket을 통해 전달된다.
`request_completion::enqueue()`는 `internal_pair_queue::send_buffer_frame()`을 호출하고,
그 내부에서는 다시 `socket_->send()`가 실행된다. 따라서 completion signal 자체도 core
socket send, pipe flush, activate/read wakeup 경로의 영향을 받을 수 있다.

이 말은 completion 전달에 re-signal hop이 하나 더 있다는 뜻이다. 실제 reply readiness가
application poller를 직접 깨우는 것이 아니라, reply dispatch가 completion queue에 결과를
넣고 internal PAIR socket에 1바이트를 보낸 뒤 그 PAIR socket의 readiness를 poller가 본다.
serial에서는 이 추가 hop의 wakeup 비용이 request마다 그대로 latency가 될 수 있다.

반대로 poller는 blocking wait에 들어가기 전 socket readiness를 직접 확인한다. signal byte가
이미 internal PAIR pipe에 들어와 있다면 `has_in()`으로 즉시 감지할 수 있다. 그러므로
completion signal 후보는 "enqueue 이후 poller가 무조건 1 ms 늦는다"가 아니라,
"signal byte가 internal PAIR socket 경로를 지나 poller가 볼 수 있는 상태가 되기까지
지연되는가"로 좁혀서 확인한다.

`zlink_poller_wait()`에는 hidden completion을 drain한 뒤 public event가 없어도 caller에게
return해야 한다는 주석이 있다. 이 주석의 의도는 다음 timeout까지 다시 wait에 들어가
처리량이 `1 / timeout`으로 제한되는 상황을 막는 것이다.

따라서 여기서 확인할 것은 "의도한 wakeup이 실제로 즉시 일어나는가"이다.

확인할 질문:

- `queue_reply_completion()`에서 signal byte를 쓴 시각과 `zlink_poller_wait()`가
  return한 시각 사이가 얼마인가?
- hidden completion event가 관측되었는데 callback drain 뒤 caller로 바로 돌아오지
  않는 경로가 있는가?
- signal socket에 byte가 남거나 `signal_pending` 상태가 꼬여 다음 completion이
  signal을 생략하는 경우가 있는가?

수정 계층 후보:

`core/src/api/socket/request_completion_queue_internal.cpp`,
`core/src/api/socket/internal_pair_queue_internal.cpp`,
`core/src/api/monitoring/poller_api.cpp`

### 3.4 서버 blocking recv wakeup 비용

with_grpc zlink server의 request loop는 `zlink_router_recv_part()`를 blocking으로 호출하고,
request를 받으면 즉시 `zlink_router_reply_part()`로 응답한다. serial 패턴에서는 다음 request가
이전 completion 이후에야 들어오므로 server thread도 매 request마다 완전히 잠들었다가 깨어날
수 있다.

관련 코드:

```text
bindings/c/bench/with_grpc/zlink/bench_zlink_server.cpp
core/src/runtime/core/recv_internal.cpp
```

이 후보는 zlink 고유 completion path만의 문제라기보다 serial 패턴의 구조적 비용일 수 있다.
다만 같은 머신에서 gRPC serial이 훨씬 빠르므로, 서버 blocking wakeup만으로 전체 차이를
설명할 수 있는지는 측정으로 분리한다.

확인할 질문:

- server `zlink_router_recv_part()` return까지의 시간이 `1 ms` 대부분을 차지하는가?
- server reply 완료 뒤 client dispatch 진입까지가 짧은가, 긴가?
- server를 busy-poll 진단 모드로 바꾸면 serial latency가 크게 줄어드는가?

수정 계층 후보:

core recv/wakeup 경로 또는 bench 진단 조건. 단, busy polling은 최종 수정안으로 남기지 않는다.

### 3.5 I/O thread 배치와 inproc re-signal 경계

기본 context의 I/O thread 수는 `ZLINK_IO_THREADS_DFLT=4`다. TCP reply 수신과 socket message
dispatch는 I/O thread와 관련된다. 반면 completion signal에 쓰는 internal PAIR socket은
`inproc://`로 만들어지며, inproc bind/connect 자체는 일반 TCP transport처럼 `choose_io_thread()`
로 session을 만들지 않고 pipepair를 직접 붙인다.

따라서 "internal PAIR rx/tx가 기본 I/O thread 수 때문에 반드시 서로 다른 I/O thread에 배정된다"는
식으로 단정하지 않는다. 대신 아래 가능성을 나누어 확인한다.

- TCP reply를 받은 I/O thread에서 request/reply dispatch가 언제 실행되는가?
- internal PAIR signal send가 application poller가 볼 수 있는 pipe state까지 언제 반영되는가?
- `ZLINK_IO_THREADS=1`과 기본값 4에서 serial latency가 달라지는가?
- socket affinity를 고정했을 때 serial latency가 달라지는가?

수정 계층 후보:

core I/O scheduling, socket dispatch, internal inproc signal 경로

### 3.6 request timeout schedule/cancel 비용

request는 submit 시 timeout task를 등록하고, reply가 도착하면 dispatch 경로에서 cancel한다.
모든 request가 이 경로를 지나므로 비용은 확인해야 한다.

관련 코드:

```text
core/src/api/socket/socket_request_reply_pending_api.cpp
core/src/api/socket/socket_request_reply_dispatch.cpp
core/src/api/socket/request_timeout_scheduler_internal.cpp
```

다만 현재 리포트 기준으로는 이 후보의 우선순위가 낮다. submit 전체 시간이 요청당 약
`9 us` 수준이므로 timeout schedule이 serial의 `1 ms` 대부분을 설명한다고 보기는 어렵다.
그래도 cancel 경로가 scheduler thread와 경합하거나 특정 상황에서 blocking되는지
별도 측정으로 확인한다.

수정 계층 후보:

`core/src/api/socket`, request timeout scheduler

### 3.7 `send_activate_read`의 same-thread inline 최적화 누락

`object.cpp`에서 pipe 활성화 커맨드 두 종류의 처리 방식이 비대칭이다.

```text
core/src/runtime/core/object.cpp
```

```cpp
void zlink::object_t::send_activate_write (pipe_t *destination_, uint64_t msgs_read_)
{
    ...
    if (destination_->get_tid () == _tid)
        destination_->process_command (cmd);   // 같은 스레드면 mailbox 안 거치고 즉시 처리
    else
        send_command (cmd);
}

void zlink::object_t::send_activate_read (pipe_t *destination_)
{
    ...
    send_command (cmd);   // 같은 스레드여도 항상 mailbox 경유
}
```

`send_activate_write`(269-280행)는 송신자·수신자가 같은 `tid`면 mailbox를 거치지
않고 `process_command()`를 바로 호출하는 fast path가 있는데, reply 전달에 직접
쓰이는 `send_activate_read`(260-267행)에는 이 fast path가 없다. request/reply
serial 벤치 토폴로지에서 관련 pipe들이 같은 io_thread(tid)에 배정된다면, 매
reply마다 필요 없는 mailbox 왕복(큐 push + signal + 이후 dequeue)을 타고 있을
수 있다.

확인할 질문:

- 이 벤치 조건에서 reply를 전달하는 pipe의 `destination_->get_tid() == _tid`가
  실제로 참인가?
- 참이라면, `send_activate_write`와 동일한 same-thread inline 처리를
  `send_activate_read`에도 적용했을 때 serial latency가 줄어드는가?

수정 계층 후보:

`core/src/runtime/core/object.cpp`

## 4. 확인 실험

수정 전에 아래 순서로 병목 위치를 분리한다. 실험용 계측은 성능 개선 패치와 분리하고,
계측 결과를 확인한 뒤 제거한다.

### 4.1 command throttle 영향 제거

진단 빌드에서 `max_command_delay`를 0으로 낮추거나, request serial 경로에서
`process_commands(0, true)`가 command poll을 건너뛰지 않도록 임시로 바꾼 뒤
1024바이트 serial만 재측정한다.

판단 기준:

- serial 처리량이 크게 회복되면 `activate_read` 또는 completion signal progress가
  command polling throttle에 묶인 것이다.
- serial 처리량이 그대로 약 `0.92 KOPS`이면 command throttle은 1차 원인이 아니므로
  reply dispatch 이전 또는 poller wakeup 이후 경로를 계속 본다.

이 변경은 진단용이다. 처리량을 위해 command throttle 정책을 무조건 제거하는 방식은
최종 수정안으로 보지 않는다. 실제 수정은 request/reply completion progress에 필요한
command만 지연되지 않게 할 수 있는지, 또는 low-latency 경로를 좁게 둘 수 있는지 별도로
설계한다.

### 4.2 serial completion timeline 계측

with_grpc zlink client에 임시 timestamp를 넣어 아래 시각을 기록한다.

1. `zlink_dealer_request_part()` 호출 직전
2. `zlink_dealer_request_part()` return 직후
3. `zlink_poller_wait()` 진입 직전
4. `zlink_poller_wait()` return 직후
5. reply callback 진입 시각

판단 기준:

- 2번에서 5번까지가 약 `1 ms`이면 completion wait 경로가 병목이다.
- 1번에서 2번까지가 커지면 submit/send 경로를 다시 본다.
- 4번과 5번이 거의 같고 3번에서 4번이 약 `1 ms`이면 poller wakeup 전 단계가 병목이다.

### 4.3 core dispatch와 completion enqueue 계측

core에 임시 trace를 넣어 아래 시각을 비교한다.

1. client request submit 완료
2. server `zlink_router_recv_part()` return
3. server `zlink_router_reply_part()` 완료
4. client `socket_request_reply_dispatch()` reply path 진입
5. `queue_reply_completion()` 호출 직전
6. `request_completion::enqueue()` signal write 직후
7. `pipe_t::flush_unlocked()`에서 completion 내부 PAIR tx에 대해
   `send_activate_read()`를 호출한 시각(`core/src/runtime/core/pipe.cpp:1027-1029`)
8. 그 커맨드가 상대 pipe에서 `process_activate_read()`로 소비된 시각
   (`core/src/runtime/core/pipe.cpp:558-570`)
9. `zlink_poller_wait()` return

판단 기준:

- 1번에서 2번까지가 약 `1 ms`이면 server blocking recv wakeup 또는 request delivery
  경로를 본다.
- 3번에서 4번까지가 약 `1 ms`이면 runtime socket dispatch 또는 transport event
  progress 경로를 본다.
- 6번에서 9번까지가 약 `1 ms`이면 completion signal 또는 poller wakeup 경로를 본다.
  이때 7번-8번 구간이 크면 mailbox 크로스스레드 전달 자체의 비용(3.5, 3.7)이고,
  8번-9번 구간이 크면 completion queue drain 이후 poller 쪽 비용(3.3)이다.
- 4번에서 6번까지가 크면 pending lookup, timeout cancel, reply decode, enqueue 비용을 본다.

추가로 `process_commands(0, true)`가 command poll을 skip한 횟수와 시각을 함께 기록한다.
다만 3.1.1에서 확인했듯 이 throttle은 `send_activate_read` 자체를 게이팅하지 않으므로,
skip 횟수가 반복돼도 그것만으로 3.1을 우선 수정 대상으로 올리지 않는다 — 7번-8번
구간의 실측 지연과 함께 봐야 한다.

### 4.4 poll timeout 영향 제거

진단 빌드에서 serial loop의 `poll_once(poller, 50)`을 `poll_once(poller, 0)` busy poll
형태로 바꿔 같은 조건을 한 번 측정한다.

판단 기준:

- throughput이 크게 오르면 signal wakeup이나 wait blocking 경로가 문제다.
- throughput이 그대로 약 `0.92 KOPS`이면 poll timeout이 아니라 reply dispatch나
  completion 생성 이전 경로가 문제일 가능성이 높다.

이 변경은 진단용이다. hot path에 busy polling을 남기지 않는다.

### 4.5 timeout schedule/cancel 제거 실험

진단 빌드에서 timeout task schedule/cancel을 우회하거나, timeout이 필요 없는 local
실험 경로를 만들어 serial request만 재측정한다.

판단 기준:

- serial latency가 의미 있게 줄면 request timeout 관리 비용을 줄이는 설계를 검토한다.
- 변화가 없으면 timeout 후보를 제외한다.

이 실험은 public timeout 계약을 바꾸기 위한 것이 아니다. 병목 후보를 제거하기 위한
진단이다.

### 4.6 I/O thread 수와 affinity 비교

같은 진단 빌드와 같은 실행 조건에서 `ZLINK_IO_THREADS`를 1과 기본값 4로 바꿔 serial만
비교한다. 가능하면 socket affinity도 고정해 TCP I/O dispatch와 internal signal 경로의
배치 영향을 확인한다.

같은 실험에서 `object_t::send_activate_read()` 호출 시 `destination_->get_tid () == _tid`
여부도 함께 로깅한다(3.7). `IO_THREADS=1`과 4 모두에서 이 조건이 참으로 나오는지,
거짓이면 실제로 mailbox 크로스스레드 경로를 타는지 구분한다.

판단 기준:

- `ZLINK_IO_THREADS=1`에서 serial latency가 크게 줄면 thread 배치나 cross-thread wakeup
  비용을 더 본다.
- 값이 거의 같으면 internal PAIR가 다른 I/O thread에 배정된다는 가설은 약해지고,
  command throttle 또는 inproc pipe signal 자체의 비용을 우선 본다.
- `destination_->get_tid () == _tid`가 참인데도 `send_activate_read`가 매번 mailbox를
  거친다면, 3.7의 same-thread inline 누락이 두 설정 모두에서 공통 비용일 수 있다.

### 4.7 completion signal 직접화 진단

진단 빌드에서 completion queue enqueue 이후 internal PAIR socket 대신 가벼운 직접 wakeup
수단을 임시로 사용해 본다. 예를 들어 eventfd/pipe 기반 raw fd signal이나 condition
variable 기반 진단 경로를 좁게 만들어 serial만 비교한다.

판단 기준:

- 직접 wakeup에서 serial latency가 크게 줄면 internal PAIR re-signal hop이 주요 병목이다.
- 변화가 작으면 completion enqueue 이전 dispatch 또는 server wakeup 경로가 더 유력하다.

이 실험은 public API나 최종 구조를 바로 바꾸기 위한 것이 아니다. internal PAIR signal hop의
비용 상한을 재기 위한 진단이다.

## 5. 수정 원칙

- backpressure는 의도된 flow-control 동작이다. saturation에서 blocked가 많다는 이유로
  문제로 보지 않는다.
- request/reply completion을 빠르게 만들기 위해 public API 의미를 바꾸지 않는다.
- 성능 개선을 위한 최종 수정 대상은 `core/`다. request/reply serial 병목을 줄이는 코드는
  `core/src` 또는 `core/include` 안에서 고친다.
- zlink bindings 라이브러리와 `framework/` 구현을 빠르게 만들어 수치를 맞추는 방식은 이번
  작업 범위가 아니다. 예를 들어 `bindings/c`의 public binding 구현, 다른 언어 binding,
  `framework/languages` 구현은 성능 개선 대상으로 수정하지 않는다.
- `bindings/c/bench/with_grpc`와 `bindings/c/perf`는 측정 도구다. 측정 의미가 틀렸거나
  runner/bench 코드의 버그가 확인된 경우에는 해당 측정 도구를 수정할 수 있다. 단, 이 수정은
  core 성능 개선과 분리해서 "측정 코드 수정"으로 근거를 남긴다.
- perf 전용 shortcut을 만들지 않는다. 수정은 실제 core runtime 사용자에게도 같은 의미를
  가져야 한다.
- `max_command_delay`나 command polling throttle을 전역으로 제거해 처리량 최적화를
  잃는 방식은 최종 수정안으로 보지 않는다. request/reply completion progress에 필요한
  wakeup만 지연되지 않게 하는 좁은 수정이 가능한지 먼저 검토한다.
- `ZLINK_POLLCOMPLETION`은 public completion progress 경로다. completion을 별도 timer,
  sleep, busy loop로 진행시키는 방식은 perf 정책에 맞지 않는다.
- request timeout 계약을 제거하거나 약하게 만들지 않는다. timeout 관리가 병목으로
  확인되면 같은 계약을 유지하면서 자료구조, lock 범위, cancel fast path를 개선한다.

## 6. 테스트 및 개선 판단 기준

이번 개선의 1차 판정은 `bindings/c/bench/with_grpc`의 request serial 결과로 한다.
`bindings/c/perf`의 request/reply 패턴은 pipeline 성격이 강해 serial completion 지연이
묻힐 수 있으므로, 개선 여부의 주 지표가 아니라 회귀 확인 지표로 사용한다.

### 6.1 1차 성공 기준

`zlink-c-request-serial`은 같은 실행의 `grpc-c-request-serial`보다 충분히 높아야 한다.
현재 zlink window/saturation 결과가 이미 gRPC request window/saturation보다 높기 때문에,
serial 개선 목표를 gRPC와 비슷한 수준으로 잡지 않는다.

성공 기준:

- 1024바이트 `zlink-c-request-serial` 처리량이 같은 크기의 `grpc-c-request-serial` 대비
  `3x` 이상이어야 한다.
- 4096바이트 `zlink-c-request-serial` 처리량이 같은 크기의 `grpc-c-request-serial` 대비
  `3x` 이상이어야 한다.
- 현재 리포트 기준으로는 1024바이트와 4096바이트 모두 대략 `40 KOPS` 이상을 1차 성공선으로
  본다.
- 평균 latency는 현재 약 `1.08 ms`에서 명확히 내려가야 한다. 처리량 `40 KOPS` 기준으로는
  request 하나의 평균 완료 시간이 약 `25 us` 수준이어야 한다.

판정:

| 상태 | 기준 |
|------|------|
| 실패 | zlink serial이 `0.9 KOPS` 근처에 머물거나 평균 latency가 `1 ms` 수준으로 유지된다. |
| 부분 개선 | zlink serial이 gRPC serial은 넘지만 `3x`에는 못 미친다. |
| 성공 | 1024바이트와 4096바이트 모두 zlink serial이 gRPC serial 대비 `3x` 이상이다. |

### 6.2 부작용 기준

serial만 좋아지고 다른 request/reply 또는 send 경로가 나빠지면 성공으로 보지 않는다.

반드시 같이 확인할 항목:

- `with_grpc`의 `zlink-c-request-window`
- `with_grpc`의 `zlink-c-request-saturation`
- `with_grpc`의 `zlink-c-send-blocking`
- `with_grpc`의 `zlink-c-send-saturation`
- `bindings/c/perf`의 request/reply single 패턴
- `bindings/c/perf`의 request/reply multi 패턴
- `bindings/c/perf`의 기존 send/send echo 계열

현재 기준값 대비 `5%` 이내 차이는 측정 흔들림으로 보고, 같은 조건에서 반복해도 `10%` 이상
떨어지면 회귀로 본다. 특히 command progress, pipe activation, poller wakeup 경로를 건드린
경우 send/send echo 계열이 떨어지면 수정 범위가 너무 넓은 것으로 판단한다.

### 6.3 with_grpc 실행 방법

원인 분리와 최종 성능 판정은 같은 조건으로 실행한다.

```bash
PAYLOAD_SIZES=1024,4096 \
DURATION_SECONDS=3 \
WINDOW_SIZE=100 \
MAX_OUTSTANDING=4096 \
DRAIN_TIMEOUT_MS=5000 \
bindings/c/bench/with_grpc/run_local.sh
```

core 수정 뒤에는 실제 benchmark가 사용하는 runtime을 먼저 다시 빌드한다.

```bash
cmake --build core/build -j$(nproc)
```

확인할 핵심 값:

- `grpc-c-request-serial` 1024/4096 throughput
- `zlink-c-request-serial` 1024/4096 throughput, latency, submit_wait_ms
- `zlink-c-request-window` 1024/4096 throughput, latency
- `zlink-c-request-saturation` 1024/4096 throughput, latency, blocked
- `zlink-c-send-blocking` 1024/4096 throughput
- `zlink-c-send-saturation` 1024/4096 throughput

성공 판정은 같은 report 안에서 아래 식으로 확인한다.

```text
zlink-c-request-serial throughput >= grpc-c-request-serial throughput * 3
```

이 조건을 1024바이트와 4096바이트 모두 만족해야 한다.

### 6.4 c/perf 회귀 확인 방법

`bindings/c/perf`는 개선 효과의 1차 판정이 아니라 회귀 확인에 사용한다. 실행은 동시에
여러 perf를 돌리지 않고 하나씩 진행한다.
이 확인에서 문제가 나오더라도 성능 개선을 위해 zlink bindings 라이브러리를 수정하지 않는다.
측정 코드 자체의 버그가 확인된 경우만 bench/perf 수정으로 분리한다.

확인 순서:

1. request/reply single 패턴
2. request/reply multi 패턴
3. 기존 send/send echo 계열

판정:

- request/reply single/multi가 기존 대비 `10%` 이상 반복 하락하면 회귀다.
- send/send echo가 기존 대비 `10%` 이상 반복 하락하면 command progress 또는 wakeup 수정이
  너무 넓게 영향을 준 것으로 본다.
- c/perf reqrep 수치가 크게 오르지 않아도 `with_grpc` serial이 성공 기준을 만족하면 이번
  serial 병목은 개선된 것으로 본다. 다만 c/perf가 하락하면 부작용이므로 실패다.

## 7. 완료 기준

이 작업은 아래 조건을 만족해야 완료로 본다.

1. serial request/reply의 `1 ms` 지연이 어느 구간에서 생기는지 timestamp 근거로 확인한다.
2. command polling throttle, socket message dispatch, completion signal internal PAIR 경로,
   poller wakeup, server blocking recv wakeup 중 어느 구간이 병목인지 분리한다.
3. 원인이 core 계층이면 `core/src` 또는 `core/include` 수정으로 개선한다. bench 계층이면
   `bindings/c/bench/with_grpc` 또는 `bindings/c/perf`의 측정 의미 오류와 runner 버그만
   수정할 수 있다. zlink bindings 라이브러리와 `framework/` 구현은 수정하지 않는다.
4. `ZLINK_IO_THREADS=1`과 기본값 4에서 병목 구간이 달라지는지 확인한다.
5. gRPC serial 결과와 비교할 때 zlink 고유 completion path 비용인지, serial 구조의 일반
   blocking wakeup 비용인지 구분한다.
6. `with_grpc`에서 1024바이트와 4096바이트 serial이 모두 gRPC serial 대비 `3x` 이상인지
   확인한다.
7. window와 saturation 수치가 의미 있게 나빠지지 않았는지 확인한다.
8. `bindings/c/perf` request/reply와 send/send echo 계열에 회귀가 없는지 확인한다.
9. backpressure 동작은 기존 의미를 유지한다.
10. 계측 코드는 최종 패치에 남기지 않는다. 필요한 장기 진단 기능은 별도 debug option으로
   설계한다.

## 8. 현재까지 확인한 결과

이번 확인에서 `1 ms` 고정 지연의 1차 원인은 server blocking recv 대기 경로로 좁혀졌다.
`core/src/runtime/core/recv_internal.cpp`의 `wait_socket_events_internal()`은 readiness가
없을 때 `sleep_ms(1)`로 반복 대기했다. `zlink_router_recv_part()`가 blocking recv로
request를 기다리는 serial 패턴에서는 이 1 ms sleep이 request마다 그대로 latency에
반영됐다.

이 경로를 `ZLINK_POLLIN` 단일 대기일 때 `socket_poller_t::wait()`로 바꾼 뒤, 임시 계측을
제거한 상태에서 다시 잰 결과는 아래와 같다.

| 조건 | 1024 serial | 4096 serial | 해석 |
|------|-------------|-------------|------|
| 기준 리포트 | `0.923 KOPS` | `0.921 KOPS` | 약 `1.08 ms` 고정 지연 |
| `POLLIN` sleep 제거 뒤 | `6.538 KOPS` | `6.250 KOPS` | 1 ms granularity는 제거됐지만 성공 기준 미달 |
| bench 측정 오염 제거 뒤 | `8.586 KOPS` | `9.318 KOPS` | heap userdata와 callback mutex 제거 뒤 최신 clean 결과 |

실행 리포트:

```text
bindings/c/bench/with_grpc/log/with_grpc_c_waitfix_clean_final_20260706_212537/with_grpc_c_waitfix_clean_final_20260706_212537.txt
bindings/c/bench/with_grpc/log/with_grpc_c_goal_final_20260706_224759/with_grpc_c_goal_final_20260706_224759.txt
```

같은 실행에서 gRPC serial은 1024바이트 `12.902 KOPS`, 4096바이트 `10.878 KOPS`였다.
따라서 현재 수정은 "0.92 KOPS 병목을 제거한 부분 개선"이지, 목표인 gRPC 대비 `3x`
성공은 아니다. 목표 기준으로 보면 1024바이트는 약 `38.7 KOPS`, 4096바이트는 약
`32.6 KOPS` 이상이어야 한다.

bench 측정 코드에서 request마다 heap userdata를 만들고 callback에서 latency 기록 mutex를
잡는 비용은 제거했다. 이 변경은 zlink bindings 라이브러리나 framework 수정이 아니라
`bindings/c/bench/with_grpc`의 측정 오염 제거다. callback userdata는 요청별 상태를 들고
있지 않으므로 `callback_state_t`를 직접 넘기고, completion callback은 같은 poller thread에서
drain되므로 별도 mutex를 잡지 않는다. 이 수정 뒤 3초 조건으로 다시 잰 최신 결과는 아래와
같다.

```text
bindings/c/bench/with_grpc/log/with_grpc_c_goal_final_20260706_224759/with_grpc_c_goal_final_20260706_224759.txt
bindings/c/bench/with_grpc/log/with_grpc_c_goal_recheck_20260706_230050/with_grpc_c_goal_recheck_20260706_230050.txt
bindings/c/bench/with_grpc/log/with_grpc_c_goal_recheck_scope_20260706_231734/with_grpc_c_goal_recheck_scope_20260706_231734.txt
bindings/c/bench/with_grpc/log/with_grpc_c_timeout_notify_gate_serial_only_20260707_065130/with_grpc_c_timeout_notify_gate_serial_only_20260707_065130.txt
bindings/c/bench/with_grpc/log/with_grpc_c_core_timeout_notify_gate_full_20260707_065021/with_grpc_c_core_timeout_notify_gate_full_20260707_065021.txt
```

| 조건 | grpc serial | zlink serial | zlink/gRPC | 판정 |
|------|-------------|--------------|------------|------|
| 1024바이트 | `12.265 KOPS` | `8.586 KOPS` | `0.70x` | 실패 |
| 4096바이트 | `11.799 KOPS` | `9.318 KOPS` | `0.79x` | 실패 |
| 1024바이트 recheck | `12.352 KOPS` | `7.077 KOPS` | `0.57x` | 실패 |
| 4096바이트 recheck | `8.221 KOPS` | `6.896 KOPS` | `0.84x` | 실패 |
| 1024바이트 scope recheck | `11.612 KOPS` | `7.083 KOPS` | `0.61x` | 실패 |
| 4096바이트 scope recheck | `11.300 KOPS` | `7.140 KOPS` | `0.63x` | 실패 |
| 1024바이트 timeout notify gate, serial-only | `13.687 KOPS` | `9.837 KOPS` | `0.72x` | 실패 |
| 4096바이트 timeout notify gate, serial-only | `14.030 KOPS` | `9.770 KOPS` | `0.70x` | 실패 |
| 1024바이트 timeout notify gate, all scenarios | `12.595 KOPS` | `6.988 KOPS` | `0.55x` | 실패 |
| 4096바이트 timeout notify gate, all scenarios | `13.537 KOPS` | `9.534 KOPS` | `0.70x` | 실패 |

같은 실행에서 zlink window/saturation은 여전히 gRPC보다 높다.

| 조건 | zlink window | zlink saturation | send blocking | send saturation |
|------|--------------|------------------|---------------|-----------------|
| 1024바이트 | `172.043 KOPS` | `170.233 KOPS` | `1411.190 KMSG/s` | `327.082 KMSG/s` |
| 4096바이트 | `122.415 KOPS` | `126.588 KOPS` | `708.923 KMSG/s` | `328.533 KMSG/s` |
| 1024바이트 scope recheck | `184.362 KOPS` | `182.276 KOPS` | `1453.859 KMSG/s` | `330.077 KMSG/s` |
| 4096바이트 scope recheck | `122.074 KOPS` | `127.139 KOPS` | `561.148 KMSG/s` | `326.761 KMSG/s` |
| 1024바이트 timeout notify gate | `206.248 KOPS` | `206.835 KOPS` | `1281.521 KMSG/s` | `307.200 KMSG/s` |
| 4096바이트 timeout notify gate | `157.851 KOPS` | `163.344 KOPS` | `657.862 KMSG/s` | `308.223 KMSG/s` |

따라서 현재 상태의 결론은 명확하다. `1 ms` 고정 지연은 제거됐지만, serial request/reply는
아직 성공 기준에 도달하지 못했다. timeout scheduler wakeup 비용 일부를 줄인 뒤 serial-only
3초 측정에서 1024바이트는 `9.837 KOPS`, 4096바이트는 `9.770 KOPS`까지 올라왔지만,
같은 실행의 gRPC serial 대비 각각 `0.72x`, `0.70x`다. 목표는 gRPC의 `3x`이므로 성공
판정과 c/perf 회귀 검증 단계로 넘어가면 안 된다.

함께 확인한 진단 결과:

- `ZLINK_IO_THREADS=1` 진단은 1024바이트 serial을 크게 올리지 못했다. 기본 I/O thread
  수 4 자체가 남은 지연의 주 원인이라고 보기는 어렵다.
- request/reply control frame을 4개에서 1개 packed frame으로 줄이는 실험은 serial을
  개선하지 못했고, 이번 문제의 1차 수정 후보에서 제외한다.
- completion hidden poller를 internal PAIR socket 대신 direct fd signal로 붙이는 실험도
  serial을 개선하지 못했다. 현재 구조에서 internal PAIR re-signal hop 하나만 제거해도
  목표치에 도달한다고 보기 어렵다.
- completion-only poller에 owner socket notify를 함께 등록해서 reply 도착 command를
  application thread가 직접 진행하게 만드는 진단도 1024바이트 1초에서 `8.956 KOPS`였다.
  기존 `8~9 KOPS` 범위에서 벗어나지 못했으므로, client-side hidden signal wakeup 하나만
  남은 병목이라고 보기 어렵다.
- `wait_socket_events_internal()`에서 `socket_poller_t` 대신 notify fd를 직접 `poll()`하는
  실험은 오히려 나빠졌다. 단일 fd 직접 poll은 최종 수정 후보로 남기지 않는다.
- `send_activate_read()`에 `send_activate_write()`와 같은 same-thread inline 처리를 넣는
  실험은 1024바이트 1초 측정에서 `8.181 KOPS` 수준으로, 기존 `POLLIN` sleep 제거 효과를
  넘어서지 못했다. 이 변경은 최종 수정 후보로 남기지 않는다.
- `wait_socket_events_internal()`에서 매번 `socket_poller_t`를 새로 만들지 않고 thread-local
  cache로 재사용하는 실험은 1024바이트 1초 측정에서 `8.472 KOPS` 수준이었다. 개선 폭이 작고,
  socket 수명 재사용을 잘못 다루면 오래된 notify fd를 볼 위험이 있으므로 최종 수정 후보로
  남기지 않는다.
- `wait_socket_events_internal()`에 50us bounded spin을 넣어 command wakeup 전에 짧게
  busy wait하는 실험은 1024바이트 1초에서 `6.309 KOPS`로 오히려 나빠졌다. window/saturation도
  같이 떨어졌으므로 최종 수정 후보로 남기지 않는다.
- TCP Nagle 지연은 현재 1차 후보가 아니다. `core/src/runtime/core/options.cpp`의 기본값은
  `tcp_nodelay (1)`이고, TCP listener/connecter는 이 값을 `tune_tcp_socket()`에 전달한다.
  따라서 기본 `tcp://127.0.0.1` 경로는 `TCP_NODELAY`가 켜진 상태로 봐야 한다.
- TCP transport의 nonblocking sync write를 `ZLINK_ASIO_TCP_SYNC_WRITE=1`로 켠 1024바이트
  1초 진단은 request serial을 `7.590 KOPS`로 낮췄다. non-STREAM TCP를 단순히 sync write
  쪽으로 돌리는 방식은 serial 개선 후보로 보지 않는다.
- TCP transport의 `ZLINK_ASIO_TCP_ASYNC_WRITE_SOME=1` 진단은 1024바이트 1초 request serial을
  `7.091 KOPS` 수준으로 만들었다. `boost::asio::async_write()` 대신 `async_write_some()`을
  쓰는 방식도 개선 후보로 보지 않는다.
- TCP transport의 `ZLINK_ASIO_WRITEV_SINGLE_SHOT=1` 진단은 1024바이트 1초 request serial을
  `6.306 KOPS`로 낮췄고, `ZLINK_ASIO_WRITEV_USE_ASIO=1` 진단도 `6.061 KOPS`였다. writev
  전송 정책 변경은 serial 병목을 줄이지 못했다.
- `ZLINK_ASIO_TCP_STATS=1`와 `ZLINK_BENCH_SCENARIOS=request-serial`로 1024바이트 serial만
  실행했을 때 zlink client 쪽 TCP 통계는 완료 `7032`건에 async read `7036`회, async write
  `7034`회였다. request/reply 한 건이 여러 TCP write로 과도하게 쪼개지는 문제가 주 원인이라는
  가설은 낮다.
- zlink endpoint만 `ipc://`로 바꾼 1024바이트 1초 진단에서도 serial은 `8.518 KOPS`였다.
  TCP loopback 자체가 남은 병목의 주 원인이라고 보기는 어렵다.
- `zlink_router_recv_part()`에서 raw router receive 때 router spot state를 새로 만들지 않고
  이미 있는 state만 확인하도록 바꾼 뒤에도 1024바이트 3초 serial은 `7.083 KOPS`였다. 일반
  request/reply 서버가 spot completion signal을 불필요하게 wait 대상에 넣는 비용은 줄일 수
  있지만, 목표인 `40 KOPS`급 차이를 설명하지 못한다.
- `with_grpc` zlink server의 `zlink_router_recv_part()`를 임시로 `DONTWAIT` spin loop로 바꾼
  진단은 1024바이트 1초에서 `10.195 KOPS`였다. server thread를 잠들지 않게 만들어도
  목표인 `40 KOPS+`에 도달하지 못하므로, server blocking recv wakeup 하나만 남은 병목으로
  볼 수 없다. 이 변경은 CPU 사용 정책을 바꾸는 bench 진단이므로 최종 코드에 남기지 않는다.
- ASIO read drain을 STREAM이 아닌 TCP DEALER/ROUTER까지 단순히 넓히는 진단은
  `with_grpc` 실행이 비정상적으로 길어지는 부작용을 만들었다. STREAM용 bounded drain을
  그대로 일반 socket에 확장하는 방식은 최종 수정 후보에서 제외한다.
- public C API의 `zlink_recv_handler()`는 현재 STREAM 전용이다. ROUTER request/reply server를
  bench에서 callback 서버로 바꾸려면 public API가 아닌 내부 socket message dispatch 경로를
  써야 하므로, 이번 작업의 bench 수정 허용 범위에 맞지 않는다. 이 방향은 bench 버그 수정이
  아니라 public API/contract 설계 문제로 분리해야 한다.
- `with_grpc` bench에는 `ZLINK_BENCH_SCENARIOS`와 `GRPC_BENCH_SCENARIOS` 필터를 추가했다.
  기본값은 `all`이라 기존 전체 벤치 의미는 유지된다. 이 필터는 serial-only 진단에서 TCP 통계와
  context switch 수를 다른 scenario와 섞지 않기 위한 측정 코드 변경이다.
- `request_timeout_scheduler_internal.cpp`에서 정상 reply cancel 때 scheduler condition variable을
  매번 깨우던 경로를 줄였다. cancel은 schedule map에서 task를 제거하고 task 상태를 canceled로
  바꾸면 충분하며, scheduler를 즉시 깨우지 않아도 timeout correctness는 유지된다. 또 scheduler가
  이미 더 이른 deadline 또는 idle wake 시각까지 기다리고 있으면 새 task deadline이 그보다 늦을
  때 schedule notify를 생략하도록 했다. 이 변경은 1024바이트 1초 serial-only 진단에서
  `9.284 KOPS`, 3초 serial-only 진단에서 `9.837 KOPS`까지 올렸지만, 목표치에는 부족하다.
- `ZLINK_SERIAL_POLL_TIMEOUT_MS=0`로 client completion wait를 busy-poll하는 bench 진단은
  1024바이트 1초에서 `7.393 KOPS`였고 기본 wait보다 의미 있게 좋아지지 않았다. client poller의
  blocking wait 하나만 주 병목으로 보지 않는다. 이 env는 진단 후 최종 bench 코드에 남기지 않았다.
- completion queue를 건너뛰고 I/O thread에서 reply callback을 직접 호출하는 임시 core 진단은
  public `POLLCOMPLETION` 진행 의미를 깨뜨려 신뢰 가능한 성능값으로 쓰지 않았다. perf 정책은
  raw request/reply completion을 public poller가 소유한다고 정하고 있으므로, 이 방향은 core
  최적화가 아니라 public contract 설계 변경 후보로 분리해야 한다.
- serial-only로 client process를 `/usr/bin/time -v`로 잰 결과, zlink는 1024바이트 3초에서
  완료 `22315`건, voluntary context switch `111627`회였다. gRPC는 완료 `35898`건, voluntary
  context switch `36052`회였다. zlink는 client 기준으로 요청당 약 `5.0`회, gRPC는 약 `1.0`회
  수준이므로, 남은 병목은 payload copy나 TCP write 분할보다 serial 한 건마다 여러 application
  thread/I/O thread handoff를 타는 구조와 더 잘 맞는다.
- timeout scheduler notify gate 적용 뒤 같은 방식으로 잰 zlink client는 1024바이트 3초
  serial-only에서 완료 `29420`건, voluntary context switch `88768`회였다. 요청당 약 `3.0`회로
  줄었으므로 timeout scheduler wakeup은 실제 handoff 비용의 일부였다. 하지만 gRPC의 약
  `1.0`회/request와는 아직 차이가 크다.
- 임시 raw ROUTER/ROUTER echo serial 진단은 1024바이트 1초에서 `8.533 KOPS`,
  평균 latency `0.117 ms`였다. 같은 실행의 request/reply serial은 `6.888 KOPS`,
  평균 latency `0.144 ms`였다. request/reply envelope와 completion 비용은 raw echo 대비
  약 `20~25%` 수준의 추가 비용으로 보이지만, raw echo 자체도 목표인 `40 KOPS+`와는 거리가
  크다. 따라서 남은 큰 병목은 request/reply completion만이 아니라 zlink ROUTER 기반
  one-at-a-time round-trip wakeup 구조에도 있다.
- `bindings/c/perf/single/src/perf_dealer_router.cpp`의 ROUTER raw recv 판정과 맞춘
  `with_grpc` DEALER/ROUTER raw send/send echo serial 비교도 같은 결론이다. 이 진단은
  client가 `zlink_send_part()`로 한 건을 보내고 같은 DEALER에서 blocking `zlink_recv_part()`로
  echo를 받은 뒤 다음 요청을 보내며, server는 `zlink_router_recv_part()`에서 `seq == 0`이고
  spot routing id가 비어 있는 raw 메시지만 `zlink_send_part_rid()`로 되돌린다. 3초 측정에서
  1024바이트는 request/reply `9.640 KOPS`, send/send echo `9.861 KOPS`였고, 4096바이트는
  request/reply `9.796 KOPS`, send/send echo `10.295 KOPS`였다. raw echo가 request/reply보다
  약간 빠르지만 차이는 1024바이트 약 `2.3%`, 4096바이트 약 `5.1%` 수준이다. 따라서 현재
  남은 큰 차이는 request/reply completion 재신호만으로 설명하기 어렵고, serial 한 건마다
  client send, server blocking recv wakeup, routed reply, client blocking recv wakeup을 모두
  거치는 공통 왕복 비용을 우선 더 나누어 봐야 한다.
- 같은 구조를 libzmq DEALER/ROUTER raw send/send echo로도 비교했다. 이 진단은
  `bindings/c/bench/with_zmq/libzmq/libzmq_dist/linux-x64`의 libzmq 배포본을 사용하고,
  client가 DEALER에서 `zmq_msg_send()`로 한 건을 보낸 뒤 blocking `zmq_msg_recv()`로 echo를
  받고 다음 메시지를 보낸다. server는 ROUTER에서 routing id frame과 payload frame을 받은 뒤
  같은 routing id로 payload를 되돌린다. 3초 측정에서 1024바이트는 `9.607 KOPS`, 4096바이트는
  `9.935 KOPS`였다. 같은 실행의 zlink send/send echo는 1024바이트 `10.376 KOPS`,
  4096바이트 `10.358 KOPS`였으므로 zlink raw echo가 libzmq raw echo보다 약간 빠르다. 이 결과는
  zlink request/reply만 특이하게 느린 상태가 아니라, DEALER/ROUTER one-at-a-time blocking
  echo 토폴로지 자체가 이 머신과 조건에서 약 `10 KOPS` 근처에 묶인다는 해석을 강하게 뒷받침한다.
- `max_command_delay`를 `0`으로 낮춘 진단 빌드는 1024바이트 1초에서 request/reply serial을
  `8.289 KOPS`, 평균 latency `0.120 ms`로 올렸다. 하지만 목표치와는 거리가 멀고, 이 값은
  모든 socket command polling 정책에 영향을 주는 전역 상수다. 전역 throttle 제거는 최종
  수정 후보로 보지 않고, serial wakeup 공통 비용을 일부 줄이는 상한으로만 사용한다.
- `recv_router_message_direct()`에서 request/reply multipart를 매번 `std::vector::reserve(8)`로
  받는 비용을 stack small buffer로 바꾸는 실험도 개선 근거가 없었다. 코드 복잡도만 커지고
  짧은 벤치에서 window/saturation 흔들림이 보여 최종 수정 후보로 남기지 않는다.

임시 timeline 계측으로 본 대략적인 분해는 아래와 같다. 이 계측은 `ZLINK_REQREP_TRACE=1`과
`ZLINK_ONLY_REQUEST_SERIAL=1`을 켜고 1024바이트 serial만 1초 실행한 값이다. 계측 자체가
약간의 부하를 만들 수 있으므로 절대 시간보다 구간 비중을 우선 본다.

계측 리포트:

```text
bindings/c/bench/with_grpc/log/with_grpc_c_goal_trace_serial_20260706_224218/with_grpc_c_goal_trace_serial_20260706_224218.txt
bindings/c/bench/with_grpc/log/with_grpc_c_goal_trace_serial_20260706_224218/zlink-server.log
```

| 구간 | 평균 시간 | 해석 |
|------|-----------|------|
| submit 호출 내부 | `6.395 us` | submit 자체는 여전히 작다. |
| request stamp 뒤 server recv return | `62.101 us` | request 전달과 server blocking wakeup 비용이 가장 크다. |
| server reply 호출 내부 | `4.730 us` | reply API 자체는 작다. |
| request stamp 뒤 server reply 완료 | `66.870 us` | server가 reply를 내보낼 때까지 이미 전체 latency의 절반 이상을 쓴다. |
| request stamp 뒤 client dispatch 진입 | `101.448 us` | reply 전달과 client I/O dispatch 비용이 두 번째 큰 구간이다. |
| request stamp 뒤 completion enqueue | `104.248 us` | dispatch에서 enqueue까지는 약 `2.8 us`로 작다. |
| request stamp 뒤 completion drain/callback 직전 | `120.842 us` | completion signal/drain 비용은 약 `16.6 us`다. |
| request stamp 뒤 client callback | `120.948 us` | callback bookkeeping은 별도 큰 병목으로 보이지 않는다. |
| `activate_read` send 뒤 process | `16.712 us` | command 전달 비용은 있지만 단독으로 `40 KOPS+` 목표 차이를 설명하지 못한다. |

남은 문제는 `1 ms` 고정 sleep이 아니라 request 하나당 약 `100~150 us` 안팎의 serial
round-trip 비용이다. 특히 request 전달 후 server가 깨어나는 구간과 server reply 후 client
dispatch가 실행되는 구간이 크다. timeout scheduler의 불필요한 wakeup은 일부 줄였지만,
그 효과만으로는 `40 KOPS+` 목표에 도달하지 못한다. request/reply multipart 처리, pending
request map 처리, completion callback drain, bench의 callback/latency bookkeeping도 포함되지만,
현재 계측에서는 pending map과 timeout scheduler만으로 목표 미달을 설명하기 어렵다.

현재까지의 해석은 다음과 같다. `with_grpc`의 zlink server는 public blocking
`zlink_router_recv_part()`로 요청을 하나씩 받고, request/reply protocol은 control frame 4개와
payload frame을 합친 multipart를 사용한다. client completion은 I/O thread에서 reply를 매칭한
뒤 application thread의 poller로 다시 전달된다. 이 구조는 window/saturation에서는 잘 숨겨지지만,
serial에서는 request마다 server wakeup, multipart receive, reply send, client dispatch,
completion drain 비용을 모두 한 번씩 드러낸다. 단순한 fd wakeup 교체나 thread 수 조정으로
목표치에 도달하지 못한 이유도 이 비용이 여러 구간에 나누어져 있기 때문이다.

## 9. 다음 작업 순서

3.1.1과 8장의 확인 결과에 따라, 이제 1 ms 고정 지연이 아니라 남은 `150 us` 안팎의
serial round-trip 비용을 나누어 본다. 이미 효과가 없던 packed control, direct completion
fd, owner socket 보조 wake, notify fd 직접 poll, `send_activate_read()` same-thread inline,
단순 poller cache, TCP sync write 실험은 같은 방식으로 반복하지 않는다.

1. with_grpc serial client/server와 core에 임시 timeline 계측을 넣고, submit부터
   callback까지 전 구간을 한 trace로 비교한다. 특히 server recv 완료 전 비용,
   server reply 비용, client dispatch 비용, completion drain 비용을 별도 숫자로 남긴다.
2. request 전달 뒤 server recv가 깨어나는 구간과 server reply 뒤 client dispatch가 실행되는
   구간을 더 세분화한다. transport event, mailbox command, socket message dispatch 중 어느
   단계에서 시간을 쓰는지 구분한다. 동시에 `/usr/bin/time -v` 또는 같은 수준의 도구로
   context switch 수를 함께 남겨, 수정이 handoff 횟수를 실제로 줄였는지 확인한다.
3. raw ROUTER/ROUTER echo와 DEALER/ROUTER send/send echo serial 기준선은 request/reply보다
   약간 빠른 정도로 확인됐다. 다음에는 raw echo와 request/reply 양쪽에 같은 timeline 계측을
   넣어 request/reply에만 추가되는 비용과 공통 wakeup 비용을 분리한다.
4. gRPC처럼 server application callback이 runtime worker thread에서 실행되는 구조와 zlink의
   public blocking recv 구조가 serial에서 어떤 차이를 만드는지 분리한다. 단, public ROUTER
   callback API가 없는 상태에서 내부 dispatch를 bench에 직접 쓰는 방식은 bench 수정 범위를
   넘으므로 최종 코드로 남기지 않는다.
5. request/reply control frame 수와 follow-up frame recv 횟수의 영향을 다시 분리한다. packed
   control 실험은 큰 효과가 없었지만, 현재 protocol은 request 하나당 control frame 4개와
   payload frame을 처리하므로 server recv 비용 상한을 별도로 확인해야 한다.
6. timeout scheduler wakeup은 일부 개선했으므로, 다음에는 timeout task allocation, pending request
   map, request/reply envelope parse/build 비용을 각각 켜고 끄는 진단 빌드로 측정한다. public
   timeout 계약을 바꾸지 않고 비용 상한만 잰다.
7. server `zlink_router_recv_part()`가 request/reply envelope를 해석하고 payload를 export하는
   비용을 별도로 측정한다. 필요하면 core 내부 자료구조 할당과 multipart frame 이동 비용을
   줄이는 방향을 검토한다.
8. completion callback drain에서 사용자 callback 호출 전후, `zlink_multipart_close`,
   latency bookkeeping을 구분한다. bench 비용이 유의미하면 bench 측정 의미를 별도 이슈로
   분리하고, core 병목과 섞지 않는다.
9. 같은 core 변경 후보를 적용한 뒤 `with_grpc` 1024/4096 serial을 먼저 확인한다. 성공 기준에
   못 미치면 c/perf 회귀 검증으로 넘어가지 않고 병목 분리를 반복한다.

## 10. 작업자용 지시문

아래 지시문은 이 문서를 기준으로 실제 개선 작업을 진행할 때 그대로 사용한다.

```text
/home/hep7/project/kairos/zlink 저장소에서 zlink C API request/reply serial 성능을 개선한다.

작업 범위:
- 성능 개선 코드는 core 계층만 수정한다.
- 허용되는 core 수정 위치는 기본적으로 core/src 와 core/include 다.
- zlink bindings 라이브러리, 다른 언어 binding, framework 구현은 수정하지 않는다.
- bindings/c/bench/with_grpc 와 bindings/c/perf 는 측정 도구다. bench 코드에 버그가 있거나
  측정 의미가 현재 성능 판단과 맞지 않는 경우에만 수정할 수 있다.
- bench/perf 수정은 core 성능 개선과 구분해서, 왜 측정 코드 수정인지 근거를 남긴다.
- perf 전용 shortcut, public API 의미 변경, framework 우회 구현은 만들지 않는다.

목표:
- zlink request/reply serial에서 request 하나를 보내고 reply completion을 기다리는 경로의
  병목을 줄인다.
- 최종 목표는 with_grpc 기준으로 1024바이트와 4096바이트 모두
  zlink-c-request-serial >= grpc-c-request-serial * 3 이다.
- backpressure는 의도된 동작이므로 문제로 보지 않는다.
- window/saturation/send 성능이 의미 있게 떨어지면 실패로 본다.

진행 순서:
1. 현재 코드와 기존 리포트로 병목 후보를 다시 확인한다.
2. 임시 계측이 필요하면 core와 with_grpc bench에만 좁게 넣고, 최종 패치에는 남기지 않는다.
3. submit, server recv wakeup, server reply, client dispatch, completion enqueue, callback drain을
   나누어 측정한다.
4. 병목 구간이 확인된 뒤 core 수정안을 적용한다.
5. core/build runtime을 다시 빌드한 뒤 with_grpc 1024/4096 조건으로 먼저 검증한다.
6. 성공 기준을 만족하면 c/perf request/reply single, request/reply multi, send/send echo를
   하나씩 실행해 회귀를 확인한다.

보고할 내용:
- 수정 전후 with_grpc 결과와 zlink/gRPC 비율
- 병목으로 확정한 구간과 그 근거
- core 수정 파일과 의도
- bench/perf 수정이 있었다면 측정 코드 수정 사유
- c/perf 회귀 확인 결과
- 성공 기준 PASS/FAIL
```
