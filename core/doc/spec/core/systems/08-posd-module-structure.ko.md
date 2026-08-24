---
title: "Core POSD module structure"
---

[English](https://zlink-systems.github.io/zlink/spec/core/systems/08-posd-module-structure/) | 한국어

<!-- zlink-nav:start -->
[시스템 목차](README.ko.md) | [이전: Source layout](07-core-source-layout.ko.md) | [다음: 설계 결정](09-design-decisions.ko.md)
<!-- zlink-nav:end -->

# Core POSD module structure

> **이 장이 정의하는 것** — Core 소스를 POSD 원칙에 따라 계층과 module로 나누는 기준과 각
> 계층의 책임 경계.

## 1. 목표

좁은 공개 interface 뒤에 구현 복잡성을 숨기는 깊은 module을 만드는 설계 원칙을 POSD(*A
Philosophy of Software Design*)라 한다. Core는 이 원칙에 따라 좁은 raw
[socket](../glossary.ko.md#socket)(message를 주고받는 endpoint) C ABI 뒤에 transport,
connection, pipe와 protocol 복잡성을 숨긴다. Application service 의미를 Core helper나
option으로 다시 노출하지 않는다.

> **계약 소유** — 이 문서 전체는 계약 서술이 아니라 구현 서술이다. 여기 적힌 계층 구조가
> 코드와 다르면 문서를 코드에 맞춘다. Core가 application에 보장하는 공개 동작은 아래 표의
> 문서들이 소유하며, 이 문서는 그 계약을 다시 정의하지 않는다.

| 관련 계약 | 정의하는 문서 |
|---|---|
| 디렉터리 배치와 include 방향 | [Core source layout](07-core-source-layout.ko.md) |
| Context 생성·옵션·종료 | [Context](../01-context.ko.md) |
| message lifecycle와 ownership | [Message](../02-message.ko.md) |
| socket 생성·옵션·송수신 | [Socket 공통](../socket/README.ko.md) |

## 2. 계층별 책임

Core 소스는 다음 다섯 계층으로 나뉜다. 각 계층은 자기 행의 책임만 소유한다.

| 계층 | 책임 |
|---|---|
| Public C API | argument, handle, ownership과 result mapping |
| Socket semantics | socket type별 routing, multipart와 request correlation |
| Runtime core | context, session, pipe, mailbox command와 lifecycle |
| Engine | ZMP·RAW framing과 handshake |
| Transport | TCP, WebSocket, IPC, inproc과 TLS I/O |

경계는 양방향으로 지킨다. Public API가 transport type이나 protocol parser를 직접 분기하지
않는다. Runtime core는 socket type별 정책을 알지 않으며, engine은 application payload
의미를 해석하지 않는다. 각 계층이 어느 디렉터리에 있고 include 방향을 어떻게 제한하는지는
[Core source layout](07-core-source-layout.ko.md)이 서술한다.

## 3. 위험 신호

다음 신호는 위 계층 경계가 무너지고 있다는 뜻이다. Framework는 Core 위에서 application
service를 제공하는 상위 계층이다.

- Framework service 개념이 public Core type이나 option으로 추가된다.
- API facade가 인자를 그대로 전달하는 service-specific pass-through가 된다.
- 동일한 protocol field를 여러 engine이나 binding에서 수동으로 정의한다.
- Socket semantics가 application callback이나 Framework helper로 밀려난다.
- Framework가 private Core header나 symbol을 직접 사용한다.

이 신호가 나타나면 새 helper를 추가하기 전에 책임 경계를 다시 검토한다.
