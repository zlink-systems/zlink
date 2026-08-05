---
title: "Core thread safety 구현"
---

[English](thread-safety.en.md) | 한국어

<!-- zlink-nav:start -->
[가이드 목차](../guide/README.ko.md) | [이전: Threading model](threading-model.ko.md) | [다음: I/O thread](io-thread.ko.md)
<!-- zlink-nav:end -->

# Core thread safety 구현

> **이 장이 답하는 것** — 어느 API를 여러 thread에서 동시에 불러도 되고, 어느 API는
> 직렬화해야 하는지를 내부적으로 어떻게 지키는가. 사용자 규칙은
> [Thread safety 가이드](../guide/11-thread-safety.ko.md)가 다룬다.

## 1. 세 등급

| 등급 | 의미 |
|---|---|
| Hot path | 여러 application thread가 지원 socket의 send·request를 동시에 호출할 수 있음 |
| Control path | option, bind·connect와 handler 등록은 handle별로 직렬화됨 |
| Lifecycle | close·destroy와 같은 handle의 다른 mutable operation은 동시에 실행하지 않음 |

## 2. 내부 규칙

Socket semantic 계층은 routing state를 필요한 최소 lock으로 보호하고, pipe admission은 message ownership
전이와 함께 commit한다. Engine state는 connection의 I/O thread가 소유한다. Callback mode와 synchronous
receive mode는 같은 queue의 single consumer이므로 동시에 등록하지 않는다.

Public API guard는 handle pin과 close state만 관리한다. Service kind나 application lifecycle을 분기하지 않는다.
Close가 진행 중이면 새 operation을 정식 terminal result로 거부하고, active callback이나 API가 있으면 bounded
close 계약에 따라 기다리거나 `BUSY`를 반환한다.
