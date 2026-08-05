---
title: "Core 설계 결정"
---

[English](design-decisions.en.md)

<!-- zlink-nav:start -->
[가이드 목차](../guide/README.ko.md) | [이전: I/O thread](io-thread.ko.md) | [다음: POSD module 구조](posd-module-structure.ko.md)
<!-- zlink-nav:end -->

# Core 설계 결정

> **이 장이 답하는 것** — 비동기 I/O, socket API 형태 등 Core의 주요 설계 결정과 그 이유.

## 비동기 I/O

Boost.Asio는 지원하는 network transport에 하나의 completion 기반 I/O model을 제공한다. Engine은
protocol parsing과 transport operation을 session interface 뒤에 둔다.

## Message ownership

작은 payload는 inline으로 저장하고 큰 payload는 shared storage를 사용한다. 명시적인 move, copy,
close operation으로 allocator 선택을 노출하지 않으면서 C API 경계의 ownership을 표현한다.

## Multipart atomicity

Multipart send sequence는 하나의 논리적 queue operation으로 유지된다. Socket별 send code는 transaction
처리를 공통 multipart 경로에 맡겨 caller가 partial failure cleanup을 반복하지 않게 한다.

## Typed socket surface

Pattern별 metadata는 typed API로 반환한다. Routing id, topic, request sequence와 subscription state를
application payload frame에 삽입하지 않는다.

## Eventing 분리

Poller는 readiness, monitor는 transport/protocol 전이, generic timer는 시간 event를 보고한다. 이
mechanism은 application payload를 해석하지 않는다.
