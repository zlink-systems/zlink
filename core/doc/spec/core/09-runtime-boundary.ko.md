---
title: "Core runtime 경계"
---

[English](09-runtime-boundary.en.md) | 한국어

<!-- zlink-nav:start -->
[Core 스펙 목차](README.ko.md) | [이전: Utilities](08-utilities.ko.md) | [다음: 소켓 개요](socket/README.ko.md)
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
