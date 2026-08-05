---
title: "Core threading model"
---

[English](threading-model.en.md) | 한국어

<!-- zlink-nav:start -->
[가이드 목차](../guide/README.ko.md) | [이전: Architecture](architecture.ko.md) | [다음: Thread safety](thread-safety.ko.md)
<!-- zlink-nav:end -->

# Core threading model

> **이 장의 계약 소유 문서** — thread 사이의 공개 경계는
> [Core runtime 경계](../spec/core/09-runtime-boundary.ko.md)가 다룬다. 이 장은 Core가 실제로
> 만드는 thread 종류와 각각의 책임을 설명한다.

## 1. Thread 종류

| Thread | 책임 | 수량 |
|---|---|---|
| Application thread | public API 호출과 blocking wait | application 소유 |
| I/O thread | transport completion, engine state와 socket callback | Context의 `io_threads` |
| Reaper thread | 종료된 socket과 owned object 정리 | Context당 1개 |
| Timer scheduler | generic timer deadline과 fire count | runtime 소유 |

Core 11에는 service mailbox나 MeshNode 전용 ingress thread가 없다.

## 2. Thread 간 통신

Application thread의 command는 mailbox를 통해 owner thread로 전달한다. Payload data는 pipe queue를 통해
socket semantic 계층과 engine 사이를 이동한다. Connection은 하나의 I/O thread에 고정하며 같은 connection의
engine state를 여러 I/O thread가 동시에 변경하지 않는다.

지원하는 handle type의 send, publish와 routed send는 여러 스레드에서 동시 사용 가능하다. 저빈도
control path는 correctness를 위해 직렬화된다. 정식 API에 별도 계약이 없으면 receive는
single-consumer다.

## 3. Callback

Socket message와 transport monitor callback은 정식 API가 지정한 thread에서 실행한다. Callback 안에서 같은
handle의 destructive close나 receive mode 변경을 재진입하면 정식 result·errno로 거부한다. Context 종료 뒤
새 callback을 시작하지 않는다.
