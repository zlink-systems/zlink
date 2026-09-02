---
title: "Events"
---

[English](https://zlink-systems.github.io/zlink/spec/core/04-events/) | 한국어

<!-- zlink-nav:start -->
[Core 스펙 목차](README.ko.md) | [이전: Errors](03-errors.ko.md) | [다음: Polling](05-polling.ko.md)
<!-- zlink-nav:end -->

# Events

> **이 장이 정의하는 것** — socket event와 readiness 값의 카탈로그. 소비 경로는
> [Polling](05-polling.ko.md)과 [Monitoring](06-monitoring.ko.md)이 다룬다.

## 1. Events 개요

zlink Core에서 application이 관찰하는 event는 세 family로 나뉜다.
[socket](glossary.ko.md#socket)의 연결 상태 변화를 별도 채널로 구독해 기록하는 monitor
event, 지금 receive나 submit retry를 시도할 가치가 있음을 알리는 readiness, 그리고
generic timer의 fire다. 이 문서는 ZLink Core raw event family와 readiness 의미의 경계 —
어떤 event가 언제, 어떤 값으로 발생하는가 — 를 정의한다.

event를 실제로 받는 경로의 계약은 다음 문서가 소유한다.

| 관련 계약 | 정의하는 문서 |
|---|---|
| readiness 소비 — `zlink_poll`과 poller wait API | [Polling](05-polling.ko.md) |
| monitor 열기, recv 소비, event mask와 `zlink_monitor_event_t` 선언, queue overflow와 status counter | [Monitoring](06-monitoring.ko.md) |

## 2. Event family

| Family | Source | 전달 API | 의미 |
|---|---|---|---|
| socket monitor | raw socket monitor handle | `zlink_socket_monitor_recv` | bind, connect, handshake, disconnect, protocol과 close |
| poller readiness | raw socket, FD 또는 generic timer | `zlink_poll`, poller wait | receive 또는 submit retry를 수행할 가치가 있음 |
| timer fire | generic timer handle | `zlink_timer_recv` | 누적 fire count가 있음 |

Monitor event는 이미 일어난 일의 관측 기록이다. 반면 readiness는 현재 work 존재
가능성을 알리는 level-triggered 상태다 — 한 번 발생하고 끝나는 기록이 아니라, 조건이
유지되는 동안 계속 참으로 관찰되는 상태 값이다. Readiness 하나가 message 하나와
일대일로 대응하거나 다음 operation의 성공을 보장한다고 가정하지 않는다.

## 3. Raw socket lifecycle

Raw socket monitor는 endpoint bind/listen, outgoing connect, accept, handshake
success/failure, disconnect, protocol error와 close를 기록한다. Disconnect reason은
transport error, handshake failure, [Context](glossary.ko.md#context) 종료와 unknown을
구분한다. Event는 service topology나 application payload를 포함하지 않는다.

## 4. Receive-flow event

Receive-flow를 지원하는 DEALER·ROUTER socket은 peer의 receive-flow 상태를 monitor event 3개로 보고한다.
peer는 자신의 PAUSED·RUNNING 상태를 flow-state frame으로 알려 오고, Core는 이 상태를 이
socket에서 그 peer로 이어지는 application pipe — socket과 peer 하나를 잇는 방향별
message 통로 — 에 적용한다.

`ZLINK_EVENT_SEND_FLOW_PAUSED`와 `ZLINK_EVENT_SEND_FLOW_RESUMED`는 이 socket의 application
pipe 하나에서 peer 상태가 실제로 PAUSED와 RUNNING 사이를 오갈 때, 그 전이를 pipe에 적용한
뒤에만 발생한다. `ZLINK_EVENT_FLOW_STATE_STALE`은 같은 connection에서 받은 flow epoch가
전진하지 않아 frame을 거부할 때 발생한다. 일반 data frame, peer가 이미 유지하는 상태를 다시 요청한 경우,
아무것도 바꾸지 않는 flow-state frame에는 event를 발생시키지 않는다. Core가 물리 connection
identity 불일치로 내부 폐기하는 flow-state frame도 public monitor event를 만들지 않으며,
[Monitoring](06-monitoring.ko.md)의 `flow_state_stale_total` counter에만 반영된다.

적용된 flow 상태의 버전 번호를 flow epoch라 한다. 각 event가 담는 값은 다음과 같다.

| Event | `value` | `flags` | 다른 field |
|---|---|---|---|
| `ZLINK_EVENT_SEND_FLOW_PAUSED` | 적용된 상태의 flow epoch | 없음 | PAUSED된 peer의 `routing_id`, `connection_id`, Application lane |
| `ZLINK_EVENT_SEND_FLOW_RESUMED` | 적용된 상태의 flow epoch | remote pause를 해제한 결과 pipe가 실제로 writable이면 `ZLINK_MONITOR_EVENT_FLAG_SEND_FLOW_WRITABLE` | PAUSED와 동일 |
| `ZLINK_EVENT_FLOW_STATE_STALE` | 받은 flow epoch | `ZLINK_MONITOR_EVENT_FLAG_FLOW_STATE_STALE_EPOCH` | 해당 peer의 `routing_id`, `connection_id`, Application lane |

byte [HWM](glossary.ko.md#hwm), transport wait, termination 같은 다른 원인이 계속 pipe를
막고 있으면 `ZLINK_MONITOR_EVENT_FLAG_SEND_FLOW_WRITABLE`이 없다. 따라서 RESUMED event만으로
다음 send가 수락된다고 보장하지 않는다.

`FLOW_STATE_STALE_EPOCH`는 flow epoch가 전진하지 않은 frame을 뜻한다. `value`는 받은 epoch이고,
현재 epoch는 같은 connection의 직전 PAUSED 또는 RESUMED event가 보고한 값이다.

이 event 3개는 monitor event mask의 bit 16, 17, 18을 사용하므로 `ZLINK_EVENT_ALL`은
`0x7FFFF`다. Mask를 직접 지정하는 monitor는 해당 bit를 설정해야 이 event를 받는다.

## 5. Ordering과 overflow

같은 monitor queue에서는 Core가 event를 commit한 순서를 보존한다. 서로 다른 connection
[I/O thread](glossary.ko.md#io-thread) 사이의 전역 wall-clock order는 제공하지 않는다.
Queue overflow와 status counter의 정확한 계약은 [Monitoring](06-monitoring.ko.md)이 소유한다.

## 6. 구현 및 contract test 검증 요구

공개 표면(monitor recv로 받은 event의 `event`·`value`·`flags`와 field, monitor
open 시 지정한 event mask)만으로 다음을 확인한다. 각 항목은 unit test 하나로 이어진다.

**Receive-flow event 발생**
- DEALER-DEALER·DEALER-ROUTER single connection과 ROUTER-ROUTER two-lane connection에서 peer 상태가 실제로
  PAUSED와 RUNNING 사이를 오가면, 그 전이를 application pipe에 적용한 뒤
  `ZLINK_EVENT_SEND_FLOW_PAUSED` 또는 `ZLINK_EVENT_SEND_FLOW_RESUMED`가 관찰된다.
- 일반 data frame, peer가 이미 유지하는 상태를 다시 요청한 경우, 아무것도 바꾸지 않는 flow-state frame에는 receive-flow event가 관찰되지 않는다.
- 같은 connection의 flow epoch가 중복·역행해 frame을 거부하면 `ZLINK_EVENT_FLOW_STATE_STALE`이 관찰된다.

**Event field와 flag**
- PAUSED·RESUMED event의 `value`는 적용된 상태의 flow epoch이고, event는 PAUSED된 peer의
  `routing_id`, `connection_id`와 Application lane을 담는다.
- 두 topology의 receive-flow event는 상태가 적용된 현재 Application pipe의 `connection_id`와
  Application `transport_lane`을 보고한다. Flow-state frame이 ROUTER-ROUTER Completion
  connection에서 왔더라도 event의 lane을 Completion으로 바꾸지 않는다.
- remote pause를 해제한 결과 pipe가 실제로 writable일 때만 RESUMED event에 `ZLINK_MONITOR_EVENT_FLAG_SEND_FLOW_WRITABLE`이 있다. byte HWM, transport wait, termination 같은 다른 원인이 pipe를 계속 막으면 이 flag가 없고, RESUMED event만으로 다음 send 수락이 보장되지 않는다.
- STALE event의 `flags`에는 `ZLINK_MONITOR_EVENT_FLAG_FLOW_STATE_STALE_EPOCH`이 있고 `value`는 받은
  epoch다. 현재 epoch는 같은 connection의 직전 PAUSED 또는 RESUMED event가 보고한 값과 같다.

**Event mask**
- Receive-flow event 3개는 monitor event mask의 bit 16, 17, 18을 사용하고 `ZLINK_EVENT_ALL`은 `0x7FFFF`다.
- Mask를 직접 지정한 monitor는 해당 bit를 설정한 경우에만 이 event를 받는다.

**Ordering**
- 같은 monitor queue에서 받은 event는 Core가 commit한 순서를 보존한다.
- 서로 다른 connection I/O thread의 event 사이에는 전역 wall-clock order를 보장하지 않는다.

Queue overflow와 status counter의 검증은 [Monitoring](06-monitoring.ko.md)이 소유한다.
