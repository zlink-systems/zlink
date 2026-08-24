---
title: "Systems"
---

[English](https://zlink-systems.github.io/zlink/spec/core/systems/) | 한국어

<!-- zlink-nav:start -->
[Core 스펙 목차](../README.ko.md) | [이전: RAW (STREAM) 프로토콜 상세](../protocol/02-raw.ko.md) | [다음: Architecture](01-architecture.ko.md)
<!-- zlink-nav:end -->

# Systems

> **이 장이 정의하는 것** — 특정 하나의 계약 문서에 속하지 않는 Core cross-cutting 내부
> 구조 문서의 색인.

이 문서는 여러 socket type과 공통 계약에 걸쳐 있는 Core 내부 구조 문서를 연결한다. 각 문서가
서술하는 내용은 대부분 구현 서술이며, 그 내용이 뒷받침하는 공개 계약은 각 문서가 링크로
가리키는 관련 spec 문서가 소유한다. 예외로 [06. Auto HWM](06-auto-hwm.ko.md)은 Auto HWM
budget 계약을 그 내부 구현과 함께 소유한다.

| 문서 | 내용 |
|---|---|
| [01. Core runtime architecture](01-architecture.ko.md) | Public API부터 I/O thread까지의 내부 구성요소와 경계 |
| [02. Core threading model](02-threading-model.ko.md) | thread 종류와 책임 |
| [03. I/O thread](03-io-thread.ko.md) | I/O thread의 생성과 작업 분배 |
| [04. Thread safety](04-thread-safety.ko.md) | 3단 thread-safety 계약의 구현 |
| [05. Connection Memory](05-connection-memory.ko.md) | connection당 메모리 구성 요소 |
| [06. Auto HWM](06-auto-hwm.ko.md) | Auto HWM budget 계약과 내부 구조 |
| [07. Core source layout](07-core-source-layout.ko.md) | source directory 구성과 include 방향 |
| [08. Core POSD module structure](08-posd-module-structure.ko.md) | 계층별 책임 분리 |
| [09. Core design decisions](09-design-decisions.ko.md) | 주요 설계 결정 사항 |
