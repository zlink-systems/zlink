---
title: "Core raw runtime 내부 경계"
---

[English](runtime-boundary.en.md) | 한국어

<!-- zlink-nav:start -->
[가이드 목차](../guide/README.ko.md) | [이전: Multipart atomicity](multipart-atomicity.ko.md) | [다음: ZMP 프로토콜 상세](protocol-zmp.ko.md)
<!-- zlink-nav:end -->

# Core raw runtime 내부 경계

> **이 장의 계약 소유 문서** — Core가 유지하는 공개 경계는
> [Core runtime 경계](../spec/core/09-runtime-boundary.ko.md)가 다룬다. 이 장은 그 경계를
> 내부 계층이 실제로 어떻게 나눠 지키는지 설명한다.

Core 11은 raw socket과 transport만 구현한다. Public API facade는 argument·handle·ownership을 검증하고,
socket semantic 계층은 PAIR·PUB/SUB·DEALER/ROUTER·STREAM의 routing을 결정한다. Runtime core는
connection, session, pipe와 I/O thread를 관리하며 engine이 TCP·WebSocket·TLS framing을 처리한다.

```text
+------------------------------+
| Public C API                 |
+------------------------------+
| Raw Socket Semantics         |
+------------------------------+
| Runtime Core and Pipes       |
+------------------------------+
| Engines and Transports       |
+------------------------------+
```

Mesh topology, application mailbox, stateful object, discovery authority와 service lifecycle은 Core source와
public ABI에 두지 않는다. Framework runtime은 각 언어 binding의 공개 raw API를 사용한다.

## Public binding 경계

Core public header는 context, message, raw socket, endpoint, option, poller, timer와 monitor 계약만 제공한다.
C++, .NET, JVM과 Node.js binding은 이 raw 계약을 언어에 투영한다. Framework runtime은 설치된 binding의
public API만 사용한다.

Framework 전용 service C ABI, private callback SPI, shared native service runtime과 별도 loader를 만들지 않는다.
Raw capability가 부족하면 generic raw socket 사용자에게도 필요한 primitive인지 먼저 검토하고 public Core
spec과 네 binding 계약을 함께 갱신한다. Framework가 Core private header와 export되지 않은 symbol을 직접
사용하지 않는다.

## Socket와 pipe ownership

Context는 I/O thread와 global runtime resource를 소유한다. Socket은 option, endpoint, session과 pipe를
소유한다. Session은 한 transport connection의 protocol engine과 reconnect state를 관리하고, pipe는 socket
queue와 engine 사이의 message flow를 관리한다.

Socket close는 신규 send·receive와 callback 등록을 막고 session과 pipe를 종료한 뒤 handle을 무효화한다.
Engine timer와 monitor event는 해당 connection lifetime에 속한다. Close 뒤 늦게 도착한 engine callback은
종료한 socket state를 변경하지 않는다.

Core connection identity는 physical lifetime을 구분하는 raw 관측 값이다. Core는 이를 Mesh lifecycle
generation, descriptor revision, Actor authority owner generation이나 Location authority store version으로
해석하지 않는다.

DEALER/ROUTER request-reply에서 하나의 logical peer는 Application과 Completion transport
connection을 가진다. Core는 Application write를 허용하기 전에 두 connection의 pair ID,
pair generation, lane과 peer identity를 검증한다. 한 lane이 실패하면 두 lane을 모두
종료한다. 일반 message와 request는 Application lane을 사용하고 reply는 Completion
lane을 사용한다. 따라서 Application ingress가 backpressure로 중단되어도 이미 보낸
request를 완료할 수 있다.

각 lane의 payload는 directional network pipe에만 보관한다. 수신한 application payload를
숨은 PAIR queue로 옮기지 않으며 reply payload를 completion deque로 복사하지 않는다.
남은 completion control queue에는 payload가 없는 terminal 결과의 callback metadata만
보관한다.

## Transport liveness 경계

TCP와 WebSocket engine은 orderly disconnect, read·write failure와 protocol failure를 session에 전달한다. Session은
이를 socket monitor에 보고하고 configured endpoint의 reconnect state를 갱신한다. 운영체제 TCP keepalive와 TCP
재전송 상한은 transport option으로 적용하며 engine이 별도 application control frame을 만들지 않는다.

Framework service protocol의 liveness message는 raw application payload로 운반된다. Core는 그 body와 deadline을
해석하지 않으며 각 언어 Framework runtime이 infrastructure queue와 scheduler에서 처리한다.

Core source boundary에는 `ZLINK_OPT_HEARTBEAT_IVL`, `ZLINK_OPT_HEARTBEAT_TTL`,
`ZLINK_OPT_HEARTBEAT_TIMEOUT`, `zmp_control_heartbeat`, `zmp_control_heartbeat_ack`와 이 값을 처리하는
codec·parser·engine state가 없다. `heartbeat_ivl_timer_id`, `heartbeat_ttl_timer_id`,
`heartbeat_timeout_timer_id`와 해당 callback 분기도 포함하지 않는다. Generic engine timer와 reconnect
timer는 raw transport resource다.

## Timer 경계

Core timer는 raw socket engine, reconnect와 poller integration에 필요한 monotonic scheduling primitive다.
Timer는 application Spot turn, Actor lifecycle, Instance lease와 transfer phase를 알지 못한다. Framework object
timer와 deadline scheduler는 binding의 공개 timer·poller 또는 해당 언어 scheduler로 구현한다.

Timer owner가 종료되면 callback 등록을 취소하고 pending callback이 owner state를 다시 참조하지 못하게 한다.
Timer ID는 같은 owner lifetime 안에서만 의미가 있으며 Framework operation ID나 generation으로 사용하지
않는다.

## Monitor 경계

Core monitor는 raw bind, accept, connect, disconnect, retry, protocol과 transport failure를 보고한다. Event는
raw socket과 connection lifetime을 설명하며 MeshName, ChannelName, peer admission, Spot, Actor, Location Store와
host termination 결과를 포함하지 않는다.

Framework는 binding의 public monitor API로 raw event를 받고 자신의 peer registry와 state reducer에 적용한다.
Core raw monitor queue와 Framework typed observer queue는 서로 다른 resource다. Framework observer의 느린
소비, coalescing과 metric policy를 Core monitor가 처리하지 않는다.

## Raw-only 불변 조건

- Core source와 public ABI는 service protocol command와 state machine을 소유하지 않는다.
- Core는 application mailbox, ready owner, claim, reply token과 terminal request state를 만들지 않는다.
- Core는 Spot·Actor·Instance identity, generation과 activation barrier를 해석하지 않는다.
- Core는 Location·Checkpoint Store, lease, owner CAS와 maintenance recovery를 호출하지 않는다.
- Raw engine timer와 raw monitor는 connection resource다.
- Framework는 public binding API만 사용하고 Core private symbol에 의존하지 않는다.
- Raw socket option과 monitor event는 service public API에 그대로 전달하지 않는다.
