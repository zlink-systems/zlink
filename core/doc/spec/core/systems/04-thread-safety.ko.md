---
title: "Thread safety"
---

[English](https://zlink-systems.github.io/zlink/spec/core/systems/04-thread-safety/) | 한국어

<!-- zlink-nav:start -->
[시스템 목차](README.ko.md) | [이전: I/O thread](03-io-thread.ko.md) | [다음: Connection별 memory](05-connection-memory.ko.md)
<!-- zlink-nav:end -->

# Thread safety

> **이 장이 정의하는 것** — 어느 API를 여러 thread에서 동시에 불러도 되고 어느 API는
> 직렬화해야 하는지를 Core 내부가 어떻게 지키는지의 구현 서술.

## 1. Thread safety 개요

zlink의 공개 API는 여러 application thread가 같은 핸들을 동시에 호출하는 상황을 전제한다.
어느 호출을 동시에 허용하고 어느 호출을 직렬화하는지는 공개 계약이 정하고, 이 문서는 Core
내부가 그 계약을 어떻게 지키는지 서술한다. 대상 독자는 Core 유지보수자다.

이 문서의 내용은 구현 서술이다. Caller가 의존하는 thread-safety 계약은 이 문서가 아니라
다음 문서가 소유한다.

| 관련 계약 | 정의하는 문서 |
|---|---|
| [socket](../glossary.ko.md#socket) 핸들 API의 동시 호출 허용 범위와 close 규칙 | [Socket 공통 §2 스레드 안전성](../socket/README.ko.md#2-스레드-안전성) |
| 함수별 동시 호출 가능 여부 | 각 spec 문서 함수 레퍼런스의 "**스레드 안전성:**" 레이블 |
| thread 종류와 책임 | [Threading model](02-threading-model.ko.md) |

## 2. 세 등급

Core 구현은 공개 API를 동시 호출 특성에 따라 세 등급으로 나누어 지킨다.

| 등급 | 의미 |
|---|---|
| Hot path | 여러 application thread가 지원 socket의 send·request를 동시에 호출할 수 있다. |
| Control path | option, bind·connect와 handler 등록은 handle별로 직렬화된다. |
| Lifecycle | close·destroy는 같은 handle의 다른 mutable operation과 동시에 실행하지 않는다. |

## 3. 내부 규칙

Core는 위 세 등급을 다음 규칙으로 지킨다.

**송수신 경로.** socket 유형별 송수신 동작을 구현하는 socket semantic 계층은, peer 선택에
쓰는 routing state를 필요한 최소 lock으로 보호한다. message를 pipe — socket과
session/engine 사이에서 message를 전달하는 queue([Architecture](01-architecture.ko.md) 참고)
— 에 받아들이는 admission은 message ownership 전이와 함께 commit한다. 즉 pipe가 message를
수락하는 판단과 그 message의 소유권이 library로 넘어가는 일이 따로 떨어지지 않는다.
Connection에서 protocol 처리를 담당하는 engine의 state는 그 connection의
[I/O thread](../glossary.ko.md#io-thread)가 소유한다.

**수신 모드.** callback mode와 synchronous receive mode는 같은 queue의 single consumer다 —
한 queue에서 message를 꺼내는 소비자는 하나여야 하므로 두 모드를 동시에 등록하지 않는다.

**Public API guard와 close.** 공개 API 진입을 지키는 guard는 handle pin — API 호출이 진행
중인 동안 핸들을 유효하게 고정하는 표시 — 과 close state만 관리한다. Service kind나
application lifecycle을 분기하지 않는다. Close가 진행 중이면 새 operation을 정식 terminal
result로 거부하고, active callback이나 API가 있으면 bounded close 계약에 따라 기다리거나
`BUSY`를 반환한다. Caller가 이때 관찰하는 결과는
[Socket 공통 §2 스레드 안전성](../socket/README.ko.md#2-스레드-안전성)이 정의한다.
