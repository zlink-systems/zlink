---
title: "Core runtime architecture"
---

[English](architecture.en.md)

<!-- zlink-nav:start -->
[가이드 목차](../guide/README.ko.md) | [다음: Threading model](threading-model.ko.md)
<!-- zlink-nav:end -->

# Core runtime architecture

> **이 장이 답하는 것** — Public C API에서 I/O thread까지 메시지가 지나가는
> 내부 구성요소와 그 경계.

Core는 raw socket runtime이다. Public C API는 socket 구현으로 진입하고, socket 구현은 mailbox와
pipe를 통해 I/O thread object와 command 및 message를 교환한다.

```text
+------------------------------------------------------+
| Public C API                                         |
+------------------------------------------------------+
| Socket patterns and eventing                         |
+------------------------------------------------------+
| Context, socket base, sessions, pipes, mailboxes      |
+------------------------------------------------------+
| ZMP or raw engines                                   |
+------------------------------------------------------+
| TCP, IPC, inproc, WebSocket, TLS transports          |
+------------------------------------------------------+
```

## Context와 thread

`ctx_t`는 socket slot, I/O thread, reaper, endpoint registry와 monitor 및 timer callback에
사용하는 generic control runtime을 소유한다. Socket close는 termination command를 보내고 소유한
object가 resource를 해제할 때까지 기다린다.

## Socket과 session 경로

`socket_base_t`는 public socket state와 pattern별 동작을 소유한다. Session은 transport connection
하나를 나타낸다. Pipe는 multipart 순서를 유지하면서 socket thread와 session/engine object 사이에서
message를 전달한다.

## Engine과 transport

Asio engine은 비동기 read와 write를 제출하고 completion callback을 받는다. ZMP engine은 zlink
message frame을 인코딩하고 raw engine은 byte-stream payload를 전달한다. Transport class는 endpoint
parsing, connection 설정과 운영체제 I/O를 처리한다.

## Eventing

Poller는 socket, file descriptor와 generic timer readiness를 통합한다. Socket monitor는 raw
transport와 protocol 전이를 관찰한다. Control runtime은 socket I/O thread 밖에서 monitor와 timer
callback을 실행한다.

Raw-only 책임 경계는 [Runtime Boundary](runtime-boundary.ko.md)를 참고한다.
