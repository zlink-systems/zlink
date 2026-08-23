---
title: "ZLink Core 스펙"
---

[English](https://zlink-systems.github.io/zlink/spec/core/) | 한국어

<!-- zlink-nav:start -->
[스펙 목차](../README.ko.md) | [다음: Public Contract Governance](00-public-contract-governance.ko.md)
<!-- zlink-nav:end -->

# ZLink Core 스펙

> **이 장이 정의하는 것** — Core C API 스펙 전체의 목차와 각 장이 다루는 범위.

이 목차는 `zlink.h`가 제공하는 Core 공개 C ABI 계약을 연결한다. 정식 API 문서는 공개 계약만
설명하며 source directory, socket 배선과 queue 구조는 설명하지 않는다. Contract 문서는 관련
내부 구조를 같은 문서의 "내부 구조" 절이나 cross-cutting 주제는 systems 문서로 함께 다룬다.

## 1. 공통 계약

| 문서 | 내용 |
|---|---|
| [공개 계약 관리](00-public-contract-governance.ko.md) | spec·header·test·package 일치 절차 |
| [Context](01-context.ko.md) | Context 생성, 종료와 설정 |
| [Message](02-message.ko.md) | message lifecycle, routing ID, ownership과 multipart 내부 구조 |
| [Errors](03-errors.ko.md) | 공개 result enum, errno, version과 result-errno 대응표 |
| [Events](04-events.ko.md) | 공통 event type과 readiness 의미 |
| [Polling](05-polling.ko.md) | poll item, poller와 source 지원표 |
| [Monitoring](06-monitoring.ko.md) | raw socket monitor와 status snapshot |
| [Utilities](07-utilities.ko.md) | timer, thread, stopwatch와 atomic helper |
| [Runtime 경계](08-runtime-boundary.ko.md) | Core raw C ABI와 Framework service 책임 경계, 내부 계층 구조 |

## 2. Socket 계약

| 문서 | 내용 |
|---|---|
| [Socket 목차](socket/README.ko.md) | 공통 lifecycle, option, send·receive와 내부 option 기본값 |
| [PAIR](socket/01-pair.ko.md) | 1:1 연결 |
| [PUB](socket/02-pub.ko.md) | classic fanout publisher |
| [SUB](socket/03-sub.ko.md) | classic fanout subscriber |
| [XPUB](socket/04-xpub.ko.md) | subscription-aware publisher |
| [XSUB](socket/05-xsub.ko.md) | upstream subscription socket |
| [DEALER](socket/06-dealer.ko.md) | asynchronous request source |
| [ROUTER](socket/07-router.ko.md) | routing-ID 기반 raw router |
| [STREAM](socket/08-stream.ko.md) | raw TCP/WS session socket, WS/WSS 내부 최적화 |

## 3. Wire 프로토콜

| 문서 | 내용 |
|---|---|
| [프로토콜 목차](protocol/README.ko.md) | ZMP·RAW 프로토콜 문서 색인 |
| [ZMP 프로토콜 상세](protocol/01-zmp.ko.md) | ZMP v1.0 wire 프로토콜 |
| [RAW (STREAM) 프로토콜 상세](protocol/02-raw.ko.md) | STREAM이 사용하는 RAW wire 프로토콜 |

## 4. Cross-cutting 시스템 구조

| 문서 | 내용 |
|---|---|
| [시스템 목차](systems/README.ko.md) | Core 내부 시스템 문서 색인 |
| [Architecture](systems/01-architecture.ko.md) | Public API부터 I/O thread까지의 내부 구성요소 |
| [Threading model](systems/02-threading-model.ko.md) | thread 종류와 책임 |
| [I/O thread](systems/03-io-thread.ko.md) | I/O thread의 생성과 작업 분배 |
| [Thread safety](systems/04-thread-safety.ko.md) | 3단 thread-safety 계약 구현 |
| [Connection별 memory](systems/05-connection-memory.ko.md) | connection당 메모리 구성 요소 |
| [Auto HWM 내부 설계](systems/06-auto-hwm.ko.md) | Auto HWM 내부 상태와 계산 |
| [Core source layout](systems/07-core-source-layout.ko.md) | source directory 구성과 include 방향 |
| [Core POSD module 구조](systems/08-posd-module-structure.ko.md) | 계층별 책임 분리 |
| [Core 설계 결정](systems/09-design-decisions.ko.md) | 주요 설계 결정 사항 |
