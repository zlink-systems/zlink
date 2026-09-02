---
title: "ZLink Core 스펙"
---

[English](https://zlink-systems.github.io/zlink/spec/core/) | 한국어

<!-- zlink-nav:start -->
[스펙 목차](../README.ko.md) | [다음: Public Contract Governance](00-public-contract-governance.ko.md)
<!-- zlink-nav:end -->

# ZLink Core 스펙

> **이 장이 정의하는 것** — Core C API 스펙 전체의 목차와 각 장이 다루는 범위.

이 목차는 `zlink.h`가 제공하는 Core 공개 C ABI 계약 문서를 연결한다. 각 계약 문서는 공개
계약만 설명하고, source directory·socket 배선·queue 구조 같은 내부는 계약으로 서술하지
않는다. 계약과 관련된 내부 구조는 같은 문서의 "내부 구조" 절이 다루고, 여러 문서에 걸치는
cross-cutting 주제는 systems 문서가 다룬다.

## 1. 읽는 순서

처음 읽는 독자는 다음 순서를 권장한다. 각 장은 독립적으로 읽을 수 있지만, 이 순서가 앞
장의 개념을 뒤 장이 전제하는 방향과 일치한다.

1. [Context](01-context.ko.md) — 모든 socket이 속하는 최상위 container부터 시작한다.
2. [Message](02-message.ko.md) — socket이 주고받는 데이터 단위와 ownership 규칙.
3. [Socket 공통](socket/README.ko.md) — 모든 socket type에 공통인 lifecycle·옵션·송수신 형태.
4. 개별 socket 문서 — 사용할 socket type의 장만 골라 읽어도 된다
   ([PAIR](socket/01-pair.ko.md)부터 [STREAM](socket/08-stream.ko.md)까지).
5. [Errors](03-errors.ko.md) — 모든 함수가 공유하는 result·errno 체계로 마무리한다.

wire 프로토콜, event·polling·monitoring과 내부 시스템 구조는 필요할 때
[작업별 색인](#6-작업별-색인)에서 찾아 들어간다.

## 2. 공통 계약

| 문서 | 내용 |
|---|---|
| [Public Contract Governance](00-public-contract-governance.ko.md) | spec·header·test·package 일치 절차 |
| [Context](01-context.ko.md) | Context 생성, 종료와 설정 |
| [Message](02-message.ko.md) | message lifecycle, routing ID, ownership과 multipart 내부 구조 |
| [Errors](03-errors.ko.md) | 공개 result enum, errno, version과 result-errno 대응표 |
| [Events](04-events.ko.md) | 공통 event type과 readiness 의미 |
| [Polling](05-polling.ko.md) | poll item, poller와 source 지원표 |
| [Monitoring](06-monitoring.ko.md) | raw socket monitor와 status snapshot |
| [Utilities](07-utilities.ko.md) | timer, thread, stopwatch와 atomic helper |
| [Runtime Boundary](08-runtime-boundary.ko.md) | Core raw C ABI와 Framework service 책임 경계, 내부 계층 구조 |

## 3. Socket 계약

| 문서 | 내용 |
|---|---|
| [Socket 공통](socket/README.ko.md) | 공통 lifecycle, option, send·receive와 내부 option 기본값 |
| [Socket — PAIR](socket/01-pair.ko.md) | 1:1 연결 |
| [Socket — PUB](socket/02-pub.ko.md) | classic fanout publisher |
| [Socket — SUB](socket/03-sub.ko.md) | classic fanout subscriber |
| [Socket — XPUB](socket/04-xpub.ko.md) | subscription-aware publisher |
| [Socket — XSUB](socket/05-xsub.ko.md) | upstream subscription socket |
| [Socket — DEALER](socket/06-dealer.ko.md) | asynchronous request socket |
| [Socket — ROUTER](socket/07-router.ko.md) | routing-ID 기반 raw router |
| [Socket — STREAM](socket/08-stream.ko.md) | raw TCP/WS session socket, WS/WSS 내부 최적화 |

## 4. Wire 프로토콜

| 문서 | 내용 |
|---|---|
| [Protocol](protocol/README.ko.md) | ZMP·RAW 프로토콜 문서 색인 |
| [Protocol — ZMP v1.0](protocol/01-zmp.ko.md) | ZMP v1.0 wire 프로토콜 |
| [Protocol — RAW](protocol/02-raw.ko.md) | STREAM이 사용하는 RAW wire 프로토콜 |

## 5. Cross-cutting 시스템 구조

| 문서 | 내용 |
|---|---|
| [Systems](systems/README.ko.md) | Core 내부 시스템 문서 색인 |
| [Core runtime architecture](systems/01-architecture.ko.md) | Public API부터 I/O thread까지의 내부 구성요소 |
| [Core threading model](systems/02-threading-model.ko.md) | thread 종류와 책임 |
| [I/O thread](systems/03-io-thread.ko.md) | I/O thread의 생성과 작업 분배 |
| [Thread safety](systems/04-thread-safety.ko.md) | 3단 thread-safety 계약 구현 |
| [Connection Memory](systems/05-connection-memory.ko.md) | connection당 memory 구성 요소 |
| [Auto HWM](systems/06-auto-hwm.ko.md) | Auto HWM budget 계약과 내부 구조 |
| [Core source layout](systems/07-core-source-layout.ko.md) | source directory 구성과 include 방향 |
| [Core POSD module structure](systems/08-posd-module-structure.ko.md) | 계층별 책임 분리 |
| [Core design decisions](systems/09-design-decisions.ko.md) | 주요 설계 결정 사항 |
| [Core hot path](systems/10-hot-path.ko.md) | message당 실행 경로의 규칙과 성능 gate |

## 6. 작업별 색인

하려는 작업이 정해져 있으면 아래에서 시작 문서를 찾는다. 각 계약의 내용은 링크한 문서가
소유한다.

| 하려는 작업 | 시작 문서 |
|---|---|
| 언어 binding을 구현한다 | [읽는 순서](#1-읽는-순서) 전체를 따라 읽은 뒤 [Events](04-events.ko.md), [Polling](05-polling.ko.md), [Monitoring](06-monitoring.ko.md), [Utilities](07-utilities.ko.md) |
| 오류 처리를 구현한다 | [Errors](03-errors.ko.md), 그리고 사용하는 socket 장의 오류 절 |
| socket 준비 상태를 기다리거나 여러 socket을 감시한다 | [Events](04-events.ko.md), [Polling](05-polling.ko.md), [Monitoring](06-monitoring.ko.md) |
| 다른 언어·구현과 wire 수준에서 상호운용한다 | [Protocol — ZMP v1.0](protocol/01-zmp.ko.md), [Protocol — RAW](protocol/02-raw.ko.md) |
| queue 크기·memory 사용을 이해한다 | [Auto HWM](systems/06-auto-hwm.ko.md), [Connection Memory](systems/05-connection-memory.ko.md) |
| Core 내부 코드를 유지보수한다 | [Systems](systems/README.ko.md)부터 순서대로 |
| 스펙·header·test를 함께 바꾼다 | [Public Contract Governance](00-public-contract-governance.ko.md) |
| Core 위에 상위 계층을 얹는 경계를 확인한다 | [Runtime Boundary](08-runtime-boundary.ko.md) |
