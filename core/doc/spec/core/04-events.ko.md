---
title: "Event와 readiness 카탈로그"
---

[English](https://zlink-systems.github.io/zlink/spec/core/04-events/) | 한국어

<!-- zlink-nav:start -->
[Core 스펙 목차](README.ko.md) | [이전: Errors](03-errors.ko.md) | [다음: Polling](05-polling.ko.md)
<!-- zlink-nav:end -->

# Event와 readiness 카탈로그

> **이 장이 정의하는 것** — socket event와 readiness 값의 카탈로그. 소비 경로는
> [Polling](05-polling.ko.md)과 [Monitoring](06-monitoring.ko.md)이 다룬다.

이 문서는 ZLink Core raw event family와 readiness 의미의 경계를 정의한다.

## 1. Event family

| Family | Source | 전달 API | 의미 |
|---|---|---|---|
| socket monitor | raw socket monitor handle | handler 또는 recv | bind, connect, handshake, disconnect, protocol과 close |
| poller readiness | raw socket, FD 또는 generic timer | `zlink_poll`, poller wait | receive 또는 submit retry를 수행할 가치가 있음 |
| timer fire | generic timer handle | handler 또는 timer recv | 누적 fire count가 있음 |

Monitor event는 관측 기록이고 readiness는 현재 work 존재 가능성을 알리는 level-triggered 상태다. Readiness
하나가 message 하나와 일대일로 대응하거나 다음 operation의 성공을 보장한다고 가정하지 않는다.

## 2. Raw socket lifecycle

Raw socket monitor는 endpoint bind/listen, outgoing connect, accept, handshake success/failure, disconnect,
protocol error와 close를 기록한다. Disconnect reason은 transport error, handshake failure, Context 종료와
unknown을 구분한다. Event는 service topology나 application payload를 포함하지 않는다.

## 3. Receive-flow event

Paired DEALER/ROUTER socket은 peer의 receive-flow 상태를 monitor event 3개로 보고한다.
`ZLINK_EVENT_SEND_FLOW_PAUSED`와 `ZLINK_EVENT_SEND_FLOW_RESUMED`는 이 socket의 application
pipe 하나에서 peer 상태가 실제로 PAUSED와 RUNNING 사이를 오갈 때, 그 전이를 pipe에 적용한
뒤에만 발생한다. `ZLINK_EVENT_FLOW_STATE_STALE`은 Core가 flow-state frame을 stale이나 중복으로
판정해 거부할 때 발생한다. 일반 data frame, peer가 이미 유지하는 상태를 다시 요청한 경우,
아무것도 바꾸지 않는 flow-state frame에는 event를 발생시키지 않는다.

| Event | `value` | `flags` | 다른 field |
|---|---|---|---|
| `ZLINK_EVENT_SEND_FLOW_PAUSED` | 적용된 상태의 flow epoch | 없음 | PAUSED된 peer의 `routing_id`, `transport_pair_id`, `transport_pair_generation` |
| `ZLINK_EVENT_SEND_FLOW_RESUMED` | 적용된 상태의 flow epoch | remote pause를 해제한 결과 pipe가 실제로 writable이면 `ZLINK_MONITOR_EVENT_FLAG_SEND_FLOW_WRITABLE` | PAUSED와 동일 |
| `ZLINK_EVENT_FLOW_STATE_STALE` | 사유 flag가 선택하는 받은 generation 또는 받은 epoch | `ZLINK_MONITOR_EVENT_FLAG_FLOW_STATE_STALE_GENERATION`과 `ZLINK_MONITOR_EVENT_FLAG_FLOW_STATE_STALE_EPOCH` 중 정확히 하나 | `transport_pair_id`, 현재 generation을 담은 `transport_pair_generation` |

byte HWM, transport wait, termination 같은 다른 원인이 계속 pipe를 막고 있으면
`ZLINK_MONITOR_EVENT_FLAG_SEND_FLOW_WRITABLE`이 없다. 따라서 RESUMED event만으로 다음 send가
수락된다고 보장하지 않는다.

Stale 사유 2개가 `value`의 의미를 선택하므로 모호하지 않다. `FLOW_STATE_STALE_GENERATION`은
frame이 다른 connection generation을 지칭한 경우이며 `value`가 받은 generation,
`transport_pair_generation`이 현재 generation이다. `FLOW_STATE_STALE_EPOCH`는 frame이 현재
generation에 속하지만 epoch가 전진하지 않은 경우이며 `value`가 받은 epoch이고, 현재 epoch는
같은 pair의 직전 PAUSED 또는 RESUMED event가 보고한 값이다.

이 event 3개는 monitor event mask의 bit 16, 17, 18을 사용하므로 `ZLINK_EVENT_ALL`은
`0x7FFFF`다. Mask를 직접 지정하는 monitor는 해당 bit를 설정해야 이 event를 받는다.

## 4. Ordering과 overflow

같은 monitor queue에서는 Core가 event를 commit한 순서를 보존한다. 서로 다른 connection I/O thread 사이의
전역 wall-clock order는 제공하지 않는다. Queue overflow와 status counter의 정확한 계약은
[Monitoring](06-monitoring.ko.md)이 소유한다.
