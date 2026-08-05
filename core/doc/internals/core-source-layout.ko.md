---
title: "Core source layout"
---

<!-- zlink-nav:start -->
[가이드 목차](../guide/README.ko.md) | [이전: POSD module 구조](posd-module-structure.ko.md) | [다음: STREAM 소켓 최적화](stream-socket.ko.md)
<!-- zlink-nav:end -->

# Core source layout

> **이 장이 답하는 것** — Core 11에 남는 raw runtime source가 어떻게 디렉터리와
> include 방향으로 나뉘는가.

이 문서는 Core 11에 남는 raw runtime source의 책임과 include 방향을 설명한다.

## Layer model

```text
core/
|-- include/zlink/          public raw C ABI
|-- src/api/                validation and C ABI facade
|-- src/runtime/sockets/    socket-type semantics
|-- src/runtime/core/       context, session, pipe and message runtime
|-- src/runtime/engine/     ZMP and RAW engines
|-- src/runtime/transports/ transport integration
|-- src/runtime/eventing/   poller, timer and socket monitor
`-- tests/                  raw contract and integration tests
```

## Dependency direction

Public API facade는 socket semantic 계층을 호출하고, socket semantic 계층은 runtime core의 공통
connection·pipe 메커니즘을 사용한다. Engine과 transport는 socket type의 application 의미를 알지 않는다.
하위 계층은 상위 계층 header를 include하지 않는다.

Core source에는 MeshName, ChannelName, application mailbox, Spot, Actor, Location Store와 service lifecycle
상태를 두지 않는다. Framework 편의를 위한 별도 native seam도 추가하지 않는다.

## Public include rule

Root `zlink.h`는 context, message, raw socket, eventing과 utility domain header만 포함한다. Public header는
`src/` private header를 include하지 않는다. 설치 header와 export manifest는
[Core runtime 경계](../spec/core/09-runtime-boundary.ko.md)와 일치해야 한다.
