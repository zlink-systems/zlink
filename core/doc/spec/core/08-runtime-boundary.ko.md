---
title: "Core runtime 경계"
---

[English](https://zlink-systems.github.io/zlink/spec/core/08-runtime-boundary/) | 한국어

<!-- zlink-nav:start -->
[Core 스펙 목차](README.ko.md) | [이전: Utilities](07-utilities.ko.md) | [다음: 소켓 개요](socket/README.ko.md)
<!-- zlink-nav:end -->

# Core runtime 경계

> **이 장이 정의하는 것** — Core가 raw socket·transport로만 유지하는 범위와, 그 위 계층이
> 넘지 않아야 할 경계.

이 문서는 ZLink Core 공개 C ABI가 제공하는 runtime 경계를 정의한다. Core는 message transport와
운영체제 I/O를 캡슐화한 raw socket runtime이다. Application service topology와 stateful object runtime은
Framework가 소유한다.

## 1. Core가 제공하는 기능

Core는 다음 기능을 공개 C ABI로 제공한다.

- Context와 I/O thread lifecycle
- message allocation, ownership, multipart frame과 routing ID
- PAIR, PUB, SUB, XPUB, XSUB, DEALER, ROUTER와 STREAM raw socket
- bind, connect, disconnect, endpoint와 connection lifecycle
- TCP, WebSocket과 TLS transport
- classic PUB/SUB와 raw STREAM
- raw socket monitor, generic event, poll과 poller
- generic timer, thread, stopwatch, atomic counter와 proxy
- request, handshake와 reconnect timeout
- Paired DEALER/ROUTER의 receive-flow 상태

## 2. Framework가 소유하는 기능

Core는 다음 service 개념을 공개 C ABI, 설치 header, exported symbol 또는 compatibility facade로
제공하지 않는다.

- MeshName, ChannelName membership와 service discovery
- MeshNode lifecycle, peer admission과 node·channel messaging
- ready batch, claim, receive batch와 reply token
- Spot, Actor, Instance Spot activation과 Logical Multicast
- Actor transfer, bound STREAM session과 service drain
- MeshNode monitor, service snapshot과 Spot 소유 timer

따라서 Core 설치 tree에는 `zlink/service/*.h`가 없고 root `zlink.h`도 service header를 포함하지 않는다.
Raw socket에는 ChannelName setter나 getter가 없다. Generic poller는 socket, file descriptor와 generic timer만
다루며 service owner나 claim을 반환하지 않는다. Socket monitor는 transport와 protocol 상태만 보고한다.

Framework runtime은 언어 binding의 공개 raw socket API만 사용해 service 계약을 구현한다. Framework를 위한
공통 native service runtime, 별도 Core C SPI, private binding 진입점과 language-neutral service C ABI를 두지
않는다.

Core는 paired DEALER/ROUTER socket의 receive-flow 상태를 completion lane의 frame으로
운반하고 그 frame을 runtime 내부에서 소비한다. 이 상태의 공개 표면은 설정용
`zlink_socket_set_receive_flow_state()`, 관측용 receive-flow monitor event 3개, monitor
status snapshot의 receive-flow field다. Raw flow-state frame을 수신, 송신, encode 또는
decode하는 공개 API는 없으며 flow-state frame은 application receive 호출로 전달되지 않는다.
따라서 binding이 이 frame을 직접 만들거나 해석하지 않는다.

## 3. Transport liveness 경계

Core는 orderly disconnect, transport failure와 protocol failure를 socket monitor로 보고하고 configured endpoint의
reconnect를 처리한다. TCP connection의 half-open 감지 정책이 필요한 raw application은 운영체제 TCP keepalive와
TCP 재전송 상한을 설정하거나 자신의 application protocol로 상태를 확인한다.

Framework service connection의 liveness message, Location owner lease와 STREAM session ping·pong은 Framework가
처리한다. Core는 이 service message를 해석하거나 application handler의 처리 가능 상태를 판정하지 않는다.

Core 공개 option 집합에는 `ZLINK_OPT_HEARTBEAT_IVL`, `ZLINK_OPT_HEARTBEAT_TTL`과
`ZLINK_OPT_HEARTBEAT_TIMEOUT`이 포함되지 않는다. Raw ZMP command 집합에도 `zmp_control_heartbeat`와
`zmp_control_heartbeat_ack`가 없다. 같은 값을 alias, deprecated option 또는 compatibility command로
제공하지 않는다.

## 4. Ownership과 오류 경계

Raw message와 socket handle의 allocation, retain, copy와 close 규칙은 Core 공개 spec이 정한다. Framework는
binding이 공개한 ownership 계약을 따르며 Core가 소유한 buffer view를 application callback 수명 밖으로
노출하지 않는다.

Core 오류는 raw socket, transport, protocol과 operating-system failure를 나타낸다. Framework는 이 오류를
service operation의 typed terminal result로 변환한다. Core error code를 Framework application 계약으로 그대로
승격하거나 service retry 정책을 Core에 추가하지 않는다.

Core는 accepted service work, handler completion, Actor transfer, checkpoint와 host termination의 progress를
판정하지 않는다. 각 언어 Framework runtime이 raw I/O progress와 application dispatch progress를 분리해
운영한다.

## 5. 공개 표면 검증

Core public surface 검증은 다음 조건을 만족해야 한다.

- install tree와 exported symbol에 service header·type·function이 없다.
- `zlink_socket_set_channel_name`, `zlink_socket_get_channel_name`, MeshNode poll·monitor와
  `zlink_spot_timer_new`가 없다.
- Public header와 exported symbol에 Framework 전용 C SPI와 service compatibility facade가 없다.
- raw socket, generic poller·timer와 socket monitor contract test가 통과한다.
- raw option과 ZMP command inventory가 정식 socket·protocol spec과 일치한다.
- Framework runtime은 Core public raw surface만 사용한다.
- Core public API와 implementation에 ChannelName, service dispatch, Spot, Actor, transfer와 maintenance 의미가
  없다.

## 내부 구조

> **이 장의 계약 소유 문서** — Core가 유지하는 공개 경계는 이 문서의 계약 부분이 다룬다.
> 이 절은 그 경계를 내부 계층이 실제로 어떻게 나눠 지키는지 설명한다.

Core 0.9.0은 raw socket과 transport만 구현한다. Public API facade는 argument·handle·ownership을 검증하고,
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

### Public binding 경계

Core public header는 context, message, raw socket, endpoint, option, poller, timer와 monitor 계약만 제공한다.
C++, .NET, JVM과 Node.js binding은 이 raw 계약을 언어에 투영한다. Framework runtime은 설치된 binding의
public API만 사용한다.

Framework 전용 service C ABI, private callback SPI, shared native service runtime과 별도 loader를 만들지 않는다.
Raw capability가 부족하면 generic raw socket 사용자에게도 필요한 primitive인지 먼저 검토하고 public Core
spec과 네 binding 계약을 함께 갱신한다. Framework가 Core private header와 export되지 않은 symbol을 직접
사용하지 않는다.

### Socket와 pipe ownership

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
남은 terminal callback metadata queue에는 payload가 없는 timeout·disconnect·shutdown
결과만 보관한다. 이 queue는 transport lane이나 wire record가 아니다.

### Transport liveness 경계

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

### Timer 경계

Core timer는 raw socket engine, reconnect와 poller integration에 필요한 monotonic scheduling primitive다.
Timer는 application Spot turn, Actor lifecycle, Instance lease와 transfer phase를 알지 못한다. Framework object
timer와 deadline scheduler는 binding의 공개 timer·poller 또는 해당 언어 scheduler로 구현한다.

Timer owner가 종료되면 callback 등록을 취소하고 pending callback이 owner state를 다시 참조하지 못하게 한다.
Timer ID는 같은 owner lifetime 안에서만 의미가 있으며 Framework operation ID나 generation으로 사용하지
않는다.

### Monitor 경계

Core monitor는 raw bind, accept, connect, disconnect, retry, protocol과 transport failure를 보고한다. Event는
raw socket과 connection lifetime을 설명하며 MeshName, ChannelName, peer admission, Spot, Actor, Location Store와
host termination 결과를 포함하지 않는다.

Framework는 binding의 public monitor API로 raw event를 받고 자신의 peer registry와 state reducer에 적용한다.
Core raw monitor queue와 Framework typed observer queue는 서로 다른 resource다. Framework observer의 느린
소비, coalescing과 metric policy를 Core monitor가 처리하지 않는다.

### Raw-only 불변 조건

- Core source와 public ABI는 service protocol command와 state machine을 소유하지 않는다.
- Core는 application mailbox, ready owner, claim, reply token과 terminal request state를 만들지 않는다.
- Core는 Spot·Actor·Instance identity, generation과 activation barrier를 해석하지 않는다.
- Core는 Location·Checkpoint Store, lease, owner CAS와 maintenance recovery를 호출하지 않는다.
- Raw engine timer와 raw monitor는 connection resource다.
- Framework는 public binding API만 사용하고 Core private symbol에 의존하지 않는다.
- Raw socket option과 monitor event는 service public API에 그대로 전달하지 않는다.
