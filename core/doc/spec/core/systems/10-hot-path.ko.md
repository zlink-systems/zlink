---
title: "Core hot path"
---

[English](https://zlink-systems.github.io/zlink/spec/core/systems/10-hot-path/) | 한국어

<!-- zlink-nav:start -->
[시스템 목차](README.ko.md) | [이전: Core design decisions](09-design-decisions.ko.md)
<!-- zlink-nav:end -->

# Core hot path

> **이 장이 정의하는 것** — message 한 건마다 실행되는 Core 코드(hot path)의 범위, 그 안에서
> 금지되는 동작, 상태를 캐시하는 방식, 그리고 hot path 변경이 통과해야 하는 성능 gate.

## 1. 왜 별도 계약인가

Core의 정합성 계약(reconnect, generation, pair readiness, request correlation)은 대부분
"현재 상태를 다시 해석하는" 일반 경로로 구현된다. 그 경로는 연결이 바뀔 때 한 번 실행되는
것을 전제로 설계되었고, message마다 실행되면 처리량을 수십 퍼센트 떨어뜨린다. Contract test는
이 차이를 보지 못한다. 0.16.0에서 pull-completion 전환과 single-lane 전환이 각각 DEALER 계열
throughput을 25~35%, 6~16% 떨어뜨렸을 때 contract test는 전부 green이었다. 두 변경 모두 같은
형태였다 — 선택한 pipe를 endpoint 문자열로 다시 찾고, pair table을 mutex 아래에서 조회하고,
임시 vector를 할당하는 일반 경로를 message 경로 안에 넣었다.

따라서 hot path는 정합성 코드와 다른 규칙으로 다루며, 이 장이 그 규칙을 고정한다.

## 2. Hot path의 범위

다음 public 진입점에서 시작해 pipe write 또는 pipe read에 닿기까지의 호출 트리 전체가 hot
path다. 이 표는 규범이다: 표의 함수(또는 그 callee)를 고치는 변경은 §3의 규칙과 §5의 gate를
적용받고, 새 함수를 이 트리에 끼워 넣는 변경은 표를 함께 갱신해야 한다.

| 진입점 | 경로 |
|---|---|
| `zlink_send_part` (PAIR·DEALER·ROUTER·STREAM) | `submit_completion_aware_part` → `send_completion_submit_blocking` (거절 시 DONTWAIT는 `register_send_writable_wait`) → `try_admit_send_parts_scoped` → `xsend_selected_pipe` / `xsend_configured_endpoint` / `send_direct_with_retry` → `lb_t::sendpipe_to` → `pipe_t::write_*` |
| `zlink_send_part_rid` (ROUTER·STREAM) | 위와 같되 `send_direct_with_retry` 분기 |
| `zlink_request_part` FINAL (DEALER) | `request_part_common` → `submit_pull_blocking_request` → `request_admission_submit_blocking` → `try_admit_send_parts_scoped` → `arm_socket_pending_request_timeout` |
| `zlink_reply_part` FINAL (ROUTER) | `public_router_reply_submit` → `checkout_public_router_reply_target` → `send_public_router_reply_with_wait` → `retain_reply_transport_pipe` → `send_completion_staged_frames_on_pipe` |
| `zlink_recv_part` / `zlink_router_recv_part` | `recv_dealer_message_direct` / `router_recv_part_impl` → `recv_common` / `recv_routed` → `fq_t::recvpipe` → `pipe_t::read` → `reclassify_transport_pair_application_head` → `end_public_part_receive_delivery_hold` |
| `zlink_completion_recv` | `process_submit_commands` → `drive_request_pending` → `socket_completion::recv` |
| `zlink_poll` / `zlink_poller_wait` | `get_events_internal` → `process_commands` → `xhas_in` / `xhas_out` |
| I/O thread → socket 전달 | `pipe_t::flush` → `activate_read` command → `xread_activated` → `fq_t::activated`; `process_async_mailbox` |

## 3. Hot path 안에서 금지되는 동작

Hot path 안의 코드는 다음을 하지 않는다. 예외는 §4의 후퇴 경로뿐이다.

1. **Heap 할당.** `std::vector`·`std::string`의 임시 생성, `new`, `make_shared`를 message마다
   하지 않는다. 필요한 버퍼는 socket·load balancer·pipe의 멤버 scratch를 재사용한다.
2. **문자열로 identity 해석.** Endpoint identifier나 routing ID를 문자열로 만들어 pipe를 찾지
   않는다. 선택 시점에 얻은 `pipe_t*`를 같은 send scope 안에서 그대로 사용한다.
3. **Socket 단위 table 조회와 그 mutex.** Transport pair table, pending queue map, route history
   같은 socket 단위 컨테이너를 message마다 찾지 않는다. Message 경로가 묻는 상태는 §4의 캐시로
   답한다.
4. **조건 없는 부가 작업.** Hold 해제, head 재분류, deferred control flush처럼 "가끔 필요한" 작업은
   먼저 atomic 플래그로 필요 여부를 확인하고, 필요할 때만 lock을 잡는다.
5. **Reader를 재우는 미리보기.** 수신 경로에서 pipe head를 미리 보는 코드는 prefetch된 범위
   안에서만 본다. Ypipe를 sleep 상태로 만드는 probe는 message마다 `activate_read` command
   왕복을 만든다.
6. **고정 시간 sleep.** 재시도 대기는 socket mailbox에 park한다(`wait_submit_progress`). 고정
   슬라이스 sleep은 framed transport(WS·WSS)의 flush wait를 message마다 슬라이스 길이로
   늘린다.
7. **임시 owner의 신호 누락.** Async executor가 command를 대신 소비한 뒤 detach하는 모든 경로는
   `rearm_primary_signaler()`로 public poller를 깨운다. 이를 빠뜨리면 poller는 자기 timeout까지
   잠든다.

허용되는 것: pipe 자신의 `_in_sync`·`_out_sync`, atomic load/store, 고정 크기 스택 배열, 이미
잡고 있는 send/recv scope.

## 4. 상태 캐시와 후퇴 경로

Message 경로가 필요로 하는 socket 단위 상태는 상태가 바뀌는 지점에서 pipe에 atomic으로
게시하고, message 경로는 그 캐시만 읽는다.

| 질문 | 캐시 | 게시 지점 |
|---|---|---|
| 이 pipe는 ready한 transport pair의 Application lane인가 | `pipe_t::transport_pair_application_ready_cached()` | pair admission에서 set, 첫 physical detach에서 clear |
| 이 pipe는 lifecycle active인가 | `pipe_t::is_lifecycle_active()` (`_state` 미러) | `_state`가 `active`를 떠나는 모든 전이 |
| Public part receive에 hold가 걸려 있는가 | `_public_part_receive_delivery_hold_active` (atomic) | hold begin/end |

캐시로 답하지 못하는 경우(선택한 pipe가 그 사이 detach됨, backpressure로 재시도 대기가 필요함)
에만 일반 경로로 후퇴한다. 후퇴 경로는 다음을 지킨다.

- 첫 시도는 선택된 pipe로 직접 admit한다. 재시도 가능한 거절(`EAGAIN`, `ENOTCONN`,
  `EHOSTUNREACH`, `ECONNREFUSED`)일 때만 그 선택을 configured endpoint로 commit해 대기 루프로
  넘긴다. 다른 실패는 일반 경로가 냈을 errno로 즉시 반환한다.
- `ZLINK_SEND_FLAGS_DONTWAIT`는 대기 루프에 들어가지 않는다. 재시도 가능한 거절이면 payload가
  없는 wait token을 등록하고 `ZLINK_SUBMIT_BACKPRESSURED`로 즉시 반환한다. Endpoint commit
  규칙은 blocking `ZLINK_SEND_FLAGS_NONE` send와 REQUEST에만 적용된다.
- Wait token 등록과 WRITABLE record 게시는 message마다 실행되는 성공 경로 밖에 있다. 두 작업은
  거절이 일어났을 때와 credit·attach wake가 일어났을 때만 실행되므로 §3의 허용 범위(후퇴 경로)에
  속하며, 성공 경로에 대한 §3의 금지는 그대로 유효하다.
- 후퇴 경로의 계약(blocking send의 선택은 FINAL에서 한 번, 재시도는 같은 endpoint)은 fast
  path가 있어도 바뀌지 않는다.

## 5. 성능 gate

Hot path를 고치는 모든 변경은 다음 두 gate를 통과해야 한다. 둘 다 contract test와 별개이며,
어느 하나라도 실행되지 않았으면 그 변경은 검증되지 않은 것이다.

### 5.1 명령어 수 gate (`hotpath_gate`)

`core/tests/perf/hotpath_gate`는 callgrind로 message 한 건당 실행 명령어 수를 잰다. 벽시계
throughput과 달리 결정적이라 재측정이 필요 없다. 측정 cell과 기준값은
`core/tests/perf/hotpath_reference.json`에 체크인되고, cell별 ±5%를 넘으면 실패한다.

| cell | 측정 |
|---|---|
| `dealer_dealer_inproc` | DEALER→DEALER 단방향, send+recv 명령어/message |
| `dealer_router_reqrep_inproc` | DEALER request → ROUTER reply → completion, 명령어/request |
| `pair_inproc` | PAIR 단방향 |
| `router_router_tcp` | ROUTER↔ROUTER 단방향(count-2 negative control) |

기준값은 검증된 release 또는 승인된 변경의 값이며, 의도한 비용 증가는 근거와 함께 감독
판정으로만 갱신한다. 구현 작업이 기준값을 고치지 않는다. valgrind가 없는 환경에서는 test가
등록되지 않으며, 그 경우 "gate 미실행"이 보고에 남아야 하고 green으로 계산하지 않는다.

### 5.2 Release 비교 gate

직전 Core release와의 C perf 비교는 `bindings/c/perf/perf_regression_gate.py`가 수행하며, 두
단계로 판정한다. Release 준비 절차는 이 gate를 인용하고, 이 절이 판정 기준을 소유한다.

1. Cell(pattern, transport, size, metric) 단위 5%는 측정 오차 허용치다: throughput·bandwidth
   `>= 0.95`, latency `<= 1.05`.
2. 같은 (pattern, transport)에서 message size `64, 256, 1024, 65536`을 모두 실행하고, size별
   ratio의 기하평균이 throughput·bandwidth는 `>= 1.0`, latency는 `<= 1.0`이어야 한다. 즉 어떤
   pattern·transport의 평균 성능도 직전 release보다 내려가지 않는다.

Cell 판정과 집계 판정이 모두 통과해야 gate를 통과한 것이다. 통과하지 못한 cell을 gate 완화나
이월로 처리하지 않는다. 벤치의 측정 방식이 잘못됐다면(예: 포화 구간의 queue 깊이를 latency로
보고) gate가 아니라 벤치를 고친다.

## 6. 변경 절차

1. Hot path 함수를 고치는 변경은 diff에 §2 표의 어느 진입점에 속하는지 명시한다.
2. §3의 항목을 하나라도 위반하는 코드는 §4의 캐시·후퇴 경로 형태로 다시 쓴다.
3. §5.1 gate를 변경 전후로 실행해 결과를 기록한다. Release 준비 단계에서는 §5.2까지 실행한다.
4. 새 진입점이나 새 per-message 함수를 추가하면 §2 표와 §5.1 cell을 같이 추가한다.
