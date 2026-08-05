---
title: "Core POSD module 구조"
---

[English](posd-module-structure.en.md) | 한국어

<!-- zlink-nav:start -->
[가이드 목차](../guide/README.ko.md) | [이전: 설계 결정](design-decisions.ko.md) | [다음: Source layout](core-source-layout.ko.md)
<!-- zlink-nav:end -->

# Core POSD module 구조

> **이 장이 답하는 것** — Core 소스를 POSD 원칙에 따라 계층·모듈로 어떻게 나누는가.

## 1. 목표

Core는 좁은 raw socket C ABI 뒤에 transport, connection, pipe와 protocol 복잡성을 숨긴다. Application
service 의미를 Core helper나 option으로 다시 노출하지 않는다.

## 2. 계층별 책임

| 계층 | 책임 |
|---|---|
| Public C API | argument, handle, ownership과 result mapping |
| Socket semantics | socket type별 routing, multipart와 request correlation |
| Runtime core | context, session, pipe, mailbox command와 lifecycle |
| Engine | ZMP·RAW framing과 handshake |
| Transport | TCP, WebSocket, IPC, inproc과 TLS I/O |

Public API가 transport type이나 protocol parser를 직접 분기하지 않는다. Runtime core는 socket type별 정책을
알지 않으며 engine은 application payload 의미를 해석하지 않는다.

## 3. 위험 신호

- Framework service 개념을 public Core type이나 option으로 추가함
- API facade가 인자를 그대로 전달하는 service-specific pass-through가 됨
- 동일한 protocol field를 여러 engine이나 binding에서 수동 정의함
- Socket semantics를 application callback이나 Framework helper로 밀어냄
- Private Core header나 symbol을 Framework가 직접 사용함

이 신호가 나타나면 새 helper를 추가하기 전에 책임 경계를 다시 검토한다.
