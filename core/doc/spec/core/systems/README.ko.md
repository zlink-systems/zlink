---
title: "시스템 — 목차"
---

[English](README.en.md) | 한국어

<!-- zlink-nav:start -->
[Core 스펙 목차](../README.ko.md) | [이전: RAW (STREAM) 프로토콜 상세](../protocol/02-raw.ko.md) | [다음: Architecture](01-architecture.ko.md)
<!-- zlink-nav:end -->

# 시스템 -- 목차

> **이 장이 정의하는 것** — 특정 하나의 계약 문서에 속하지 않는 Core cross-cutting 내부
> 구조 문서의 색인.

이 문서는 여러 socket 유형과 공통 계약에 걸쳐 있는 Core 내부 구조 문서를 연결한다. 각 문서가
서술하는 내용은 구현 서술이며, 그 내용이 뒷받침하는 공개 계약은 각 문서가 링크로 가리키는
관련 spec 문서가 소유한다.

| 문서 | 내용 |
|---|---|
| [Architecture](01-architecture.ko.md) | Public API부터 I/O thread까지의 내부 구성요소와 경계 |
| [Threading model](02-threading-model.ko.md) | thread 종류와 책임 |
| [I/O thread](03-io-thread.ko.md) | I/O thread의 생성과 작업 분배 |
| [Thread safety](04-thread-safety.ko.md) | 3단 thread-safety 계약의 구현 |
| [Connection별 memory](05-connection-memory.ko.md) | connection당 메모리 구성 요소 |
| [Auto HWM 내부 설계](06-auto-hwm.ko.md) | Auto HWM 내부 상태와 계산 |
| [Core source layout](07-core-source-layout.ko.md) | source directory 구성과 include 방향 |
| [Core POSD module 구조](08-posd-module-structure.ko.md) | 계층별 책임 분리 |
| [Core 설계 결정](09-design-decisions.ko.md) | 주요 설계 결정 사항 |
