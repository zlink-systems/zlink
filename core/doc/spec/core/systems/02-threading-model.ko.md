---
title: "Core threading model"
---

[English](https://zlink-systems.github.io/zlink/spec/core/systems/02-threading-model/) | 한국어

<!-- zlink-nav:start -->
[시스템 목차](README.ko.md) | [이전: Architecture](01-architecture.ko.md) | [다음: I/O thread](03-io-thread.ko.md)
<!-- zlink-nav:end -->

# Core threading model

> **이 장이 정의하는 것** — Core가 실제로 만드는 thread 종류와 각각의 책임, 그리고 thread
> 사이의 통신 구조.

## 1. Threading model 개요

zlink Core는 I/O 처리 thread와 [socket](../glossary.ko.md#socket)을 담는 최상위 container인
[Context](../glossary.ko.md#context)를 중심으로 역할이 다른 여러 thread를 나누어 사용한다.
이 문서는 그 thread 종류와 각각의 책임, 그리고 thread 사이에서 command와 payload가 어떻게
이동하는지 설명한다. 대상 독자는 Core 내부를 유지보수하거나 thread 경계를 검토하는
개발자다.

이 장의 서술은 대부분 구현 서술이다 — 구현이 바뀌면 이 문서를 코드에 맞춘다. caller가
의존하는 공개 계약은 다음 문서가 소유하며, 이 장은 그 계약을 다시 정의하지 않는다.

| 관련 계약 | 정의하는 문서 |
|---|---|
| thread 사이의 공개 경계 | [Core runtime 경계](../08-runtime-boundary.ko.md) |
| socket API별 thread 안전성 | [Socket 공통의 스레드 안전성](../socket/README.ko.md#2-스레드-안전성) |
| I/O thread 수 등 Context 옵션 | [Context](../01-context.ko.md#4-옵션) |

## 2. Thread 종류

네트워크 송수신을 실제로 처리하는 background thread를
[I/O thread](../glossary.ko.md#io-thread)라 한다. Application 소유 thread를 포함해 Core runtime에
관여하는 실행 thread는 다음 다섯 종류다.

| Thread | 책임 | 수량 |
|---|---|---|
| Application thread | public API 호출과 blocking wait | application 소유 |
| I/O thread | transport completion, engine state와 socket command 전달 | Context의 `io_threads` |
| Reaper thread | 종료된 socket과 owned object 정리 | Context당 1개 |
| Context control runtime | 내부 monitor queue 전달과 Auto HWM 재계산 | 시작된 Context당 1개(`core-ctrl`) |
| Timer scheduler | generic timer deadline과 fire count | timer가 있을 때 process당 1개 |

표의 engine은 connection 하나의 transport 송수신과 protocol encoding·decoding을 담당하는
내부 객체다. I/O thread의 event loop, engine 처리와 connection 할당 기준은
[I/O thread 내부 구조](03-io-thread.ko.md)가 설명한다.

Core에는 service mailbox나 MeshNode 전용 ingress thread가 없다.

## 3. Thread 간 통신

Application thread의 command는 thread 사이의 command 전달 채널인 mailbox를 통해 owner
thread로 전달한다. Payload data는 pipe queue를 통해 socket semantic 계층과 engine 사이를
이동한다. Connection은 하나의 I/O thread에 고정하며 같은 connection의 engine state를 여러
I/O thread가 동시에 변경하지 않는다.

다음 단락은 [Socket 공통의 스레드 안전성](../socket/README.ko.md#2-스레드-안전성)이 소유한
계약의 요약이다. 지원하는 handle type의 send, publish와 routed send는 여러 thread에서 동시
사용 가능하다. 저빈도 control path는 correctness를 위해 직렬화된다. 정식 API에 별도 계약이
없으면 받는 쪽은 single-consumer다 — 한 번에 하나의 소비자 thread만 message를 받는다.

## 4. Pull eventing

Socket monitor와 generic timer에는 application callback을 등록하지 않는다. Application은
`zlink_socket_monitor_recv()`와 `zlink_timer_recv()`로 event와 fire count를 직접 받으며,
poller로 두 handle의 readiness를 함께 기다릴 수 있다.

<!-- zlink-nav:start -->
[시스템 목차](README.ko.md) | [이전: Architecture](01-architecture.ko.md) | [다음: I/O thread](03-io-thread.ko.md)
<!-- zlink-nav:end -->
