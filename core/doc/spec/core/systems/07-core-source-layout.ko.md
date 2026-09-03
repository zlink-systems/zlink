---
title: "Core source layout"
---

[English](https://zlink-systems.github.io/zlink/spec/core/systems/07-core-source-layout/) | 한국어

<!-- zlink-nav:start -->
[시스템 목차](README.ko.md) | [이전: Auto HWM](06-auto-hwm.ko.md) | [다음: POSD module 구조](08-posd-module-structure.ko.md)
<!-- zlink-nav:end -->

# Core source layout

> **이 장이 정의하는 것** — Core 0.13.0에 남는 raw runtime source가 디렉터리와 include
> 방향으로 어떻게 나뉘는지의 구현 서술.

## 1. Core source layout 개요

이 문서는 Core 0.13.0에 남는 raw runtime source의 디렉터리 구획별 책임과, 구획 사이에
허용하는 include 방향을 설명한다. 대상 독자는 Core source를 수정하거나 새 파일의 위치를
정하는 유지보수자다.

이 문서 전체는 공개 계약이 아니라 **구현 서술**이다. 문서와 코드가 어긋나면 코드가
사실이며, 문서를 코드에 맞춘다. Application이 의존하는 공개 계약은 이 문서가 아니라 각
기능의 스펙 문서가 소유한다.

관련 계약의 소유 문서는 다음과 같다.

| 관련 계약 | 정의하는 문서 |
|---|---|
| 설치 header와 export manifest | [Core runtime 경계](../08-runtime-boundary.ko.md) |
| 각 용어의 짧은 정의 | [Core 용어](../glossary.ko.md) |

## 2. Layer model

Source는 다음 구획으로 나뉜다.

```text
core/
|-- include/                public raw C ABI (root zlink.h, zlink_enum.h, zlink_errno.h)
|   `-- zlink/              domain header (common.h, core/, message/, socket/, eventing/)
|-- src/api/                validation and C ABI facade (core, message, monitoring, socket)
|-- src/runtime/sockets/    socket-type semantics
|-- src/runtime/core/       context, session, pipe, message runtime과 poller·timer·monitor 기반
|-- src/runtime/engine/     ZMP and RAW engines
|-- src/runtime/protocol/   ZMP·RAW wire codec (encoder/decoder)
|-- src/runtime/transports/ transport integration
|-- src/runtime/utils/      allocator, clock, err 등 공용 utility
`-- tests/                  raw contract and integration tests
```

위 구획에서 [socket](../glossary.ko.md#socket)은 message를 주고받는 endpoint이고,
[Context](../glossary.ko.md#context)는 I/O thread와 socket을 담는 최상위 container다.
`src/runtime/sockets/`가 socket type별 의미를, `src/runtime/core/`가 context와
session·pipe·message runtime을 구현한다.

## 3. Include 방향

`src/api/`의 public API facade는 socket semantic 계층을 호출하고, socket semantic 계층은
runtime core의 공통 connection·pipe 메커니즘을 사용한다. Engine과 transport는 socket
type의 application 의미를 알지 못한다. 하위 계층은 상위 계층 header를 include하지 않는다.

MeshName, ChannelName, application mailbox, Spot, Actor, Location Store와 service lifecycle
상태는 Core 위에서 application service를 제공하는 상위 계층인 Framework가 소유하는 개념이다
([POSD module 구조](08-posd-module-structure.ko.md) §3). Core source에는 이 개념들을 두지
않는다. Framework 편의를 위한 별도 native seam도 추가하지 않는다.

## 4. Public include 규칙

Root `zlink.h`는 `zlink/common.h`와 core(`zlink/core/api.h`), message, socket, eventing domain
header만 포함한다.
Public header는 `src/` private header를 include하지 않는다. 설치 header와 export manifest는
[Core runtime 경계](../08-runtime-boundary.ko.md)와 일치해야 한다.
