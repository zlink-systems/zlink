---
title: "Core runtime architecture"
---

[English](https://zlink-systems.github.io/zlink/spec/core/systems/01-architecture/) | 한국어

<!-- zlink-nav:start -->
[시스템 목차](README.ko.md) | [이전: 시스템 개요](README.ko.md) | [다음: Threading model](02-threading-model.ko.md)
<!-- zlink-nav:end -->

# Core runtime architecture

> **이 장이 정의하는 것** — Public C API에서 I/O thread까지 message가 지나가는
> 내부 구성요소와 그 경계.

## 1. Core runtime 개요

Core는 raw socket runtime이다. Public C API는 [socket](../glossary.ko.md#socket) —
message를 주고받는 endpoint — 의 구현으로 진입하고, socket 구현은 thread 경계를 넘어
command를 전달하는 mailbox와 message를 전달하는 pipe를 통해
[I/O thread](../glossary.ko.md#io-thread) — 네트워크 송수신을 실제로 처리하는 background
thread — object와 command 및 message를 교환한다.

구성요소는 다음 계층으로 쌓인다.

```text
+------------------------------------------------------+
| Public C API                                         |
+------------------------------------------------------+
| Socket patterns and eventing                         |
+------------------------------------------------------+
| Context, socket base, sessions, pipes, mailboxes     |
+------------------------------------------------------+
| ZMP or raw engines                                   |
+------------------------------------------------------+
| TCP, IPC, inproc, WebSocket, TLS transports          |
+------------------------------------------------------+
```

이 문서는 이 구성요소들의 내부 구조만 서술한다(구현 서술 — 구현이 바뀌면 이 문서를
코드에 맞춘다). 대상 독자는 Core 유지보수자다. 각 구성요소가 밖으로 드러내는 공개
계약은 이 문서가 아니라 다음 문서가 소유한다.

| 관련 계약 | 정의하는 문서 |
|---|---|
| Core가 raw socket runtime으로만 유지하는 책임 경계 | [Runtime Boundary](../08-runtime-boundary.ko.md) |
| Context 수명과 옵션 | [Context](../01-context.ko.md) |
| socket 생성·옵션·송수신 | [Socket 공통](../socket/README.ko.md) |
| socket monitor 계약 | [Monitoring](../06-monitoring.ko.md) |
| poller·timer 등 utilities | [Utilities](../07-utilities.ko.md) |

## 2. Context와 thread

[Context](../glossary.ko.md#context)는 I/O thread와 socket을 담는 최상위 container다. 그
구현인 `ctx_t`는 socket slot, I/O thread, 닫힌 socket의 마무리 정리를 전담하는 reaper
thread, endpoint registry와 monitor 및 timer callback에 사용하는 generic control
runtime을 소유한다.

Socket close는 termination command를 보내고 소유한 object가 자원을 해제할 때까지
기다린다.

## 3. Socket과 session 경로

`socket_base_t`는 public socket state와 pattern별 동작을 소유한다. Session은 transport
connection 하나를 나타내는 내부 object다. Pipe는 multipart 순서를 유지하면서 socket
thread와 session/engine object 사이에서 message를 전달한다.

## 4. Engine과 transport

Asio engine은 비동기 read와 write를 제출하고 completion callback을 받는다. ZMP engine은
zlink message frame을 인코딩하고 raw engine은 byte-stream payload를 전달한다. Transport
class는 endpoint parsing, connection 설정과 운영체제 I/O를 처리한다.

## 5. Eventing

Poller는 socket, file descriptor와 generic timer readiness를 한 곳에서 통합한다. Socket
monitor는 raw transport와 protocol 전이를 관찰한다. Control runtime은 socket I/O thread
밖에서 monitor와 timer callback을 실행한다.
