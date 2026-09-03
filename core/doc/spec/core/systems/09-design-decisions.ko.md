---
title: "Core design decisions"
---

[English](https://zlink-systems.github.io/zlink/spec/core/systems/09-design-decisions/) | 한국어

<!-- zlink-nav:start -->
[시스템 목차](README.ko.md) | [이전: POSD module 구조](08-posd-module-structure.ko.md) | [다음: Core hot path](10-hot-path.ko.md)
<!-- zlink-nav:end -->

# Core design decisions

> **이 장이 답하는 것** — 비동기 I/O, socket API 형태 등 Core의 주요 설계 결정과 그 이유.

## 1. 설계 결정 개요

이 문서는 Core의 주요 설계 결정과 그 근거를 기록한다. 각 결정이 application에 보장하는
정확한 동작은 이 문서가 아니라 그 계약의 소유 문서가 정의한다 — 이 문서는 무엇을 왜 그렇게
정했는지만 담는다.

| 결정 | 계약·상세를 소유하는 문서 |
|---|---|
| [비동기 I/O](#2-비동기-io) | [Core runtime architecture](01-architecture.ko.md) |
| [Message ownership](#3-message-ownership) | [Message](../02-message.ko.md) |
| [Multipart atomicity](#4-multipart-atomicity) | [Message](../02-message.ko.md#4-multipart) |
| [Typed socket surface](#5-typed-socket-surface) | [Socket 공통](../socket/README.ko.md) |
| [Eventing 분리](#6-eventing-분리) | [Events](../04-events.ko.md), [Polling](../05-polling.ko.md), [Monitoring](../06-monitoring.ko.md) |

## 2. 비동기 I/O

Core의 비동기 I/O는 Boost.Asio 위에 둔다. Boost.Asio는 지원하는 network transport에 하나의
completion 기반 I/O model — I/O 작업을 먼저 요청해 두고 완료를 통지받는 방식 — 을 제공한다.

Engine은 protocol parsing과 transport operation을 session interface 뒤에 둔다. session은
transport connection 하나를 나타내는 내부 객체다. Engine과 session의 내부 구조는
[Core runtime architecture](01-architecture.ko.md)가 설명한다.

## 3. Message ownership

작은 payload는 message 구조체 안에 inline으로 저장하고, 큰 payload는 여러 message 핸들이
참조를 공유하는 shared storage를 사용한다. 명시적인 move, copy, close operation이 allocator
선택을 노출하지 않으면서 C API 경계의 ownership을 표현한다.

## 4. Multipart atomicity

여러 frame(part)을 하나의 논리적 message로 묶어 보내는 multipart send sequence는 하나의
논리적 queue operation으로 유지된다. message를 주고받는 endpoint인
[socket](../glossary.ko.md#socket) 종류별 send code는 이 sequence를 시작부터 끝까지 하나의
단위로 다루는 처리를 공통 multipart 경로에 맡긴다. sequence가 중간에 실패하면 공통 경로가
아직 보내지 않은 나머지 part를 정리하므로, 각 socket 종류의 send code가 그 정리를 따로
구현하지 않아도 된다.

## 5. Typed socket surface

Pattern별 metadata는 typed API로 반환한다. routing ID(ROUTER socket이 특정 peer를 식별하는
byte 열), topic, request sequence와 subscription state를 application payload frame에
삽입하지 않는다.

## 6. Eventing 분리

Poller는 readiness — source가 receive 또는 send를 진행할 가치가 있는 상태 — 를 보고하고,
monitor는 transport/protocol 전이를, generic timer는 시간 event를 보고한다. 이 세
mechanism은 application payload를 해석하지 않는다.
