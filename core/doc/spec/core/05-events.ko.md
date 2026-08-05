---
title: "Event와 readiness 카탈로그"
---

[English](05-events.en.md) | 한국어

<!-- zlink-nav:start -->
[Core 스펙 목차](README.ko.md) | [이전: Result Enums](04-errno-map.ko.md) | [다음: Polling](06-polling.ko.md)
<!-- zlink-nav:end -->

# Event와 readiness 카탈로그

> **이 장이 정의하는 것** — socket event와 readiness 값의 카탈로그. 소비 경로는
> [Polling](06-polling.ko.md)과 [Monitoring](07-monitoring.ko.md)이 다룬다.

이 문서는 ZLink Core raw event family와 readiness 의미의 경계를 정의한다.

## 1. Event family

| Family | Source | 전달 API | 의미 |
|---|---|---|---|
| socket monitor | raw socket monitor handle | handler 또는 recv | bind, connect, handshake, disconnect, protocol과 close |
| poller readiness | raw socket, FD 또는 generic timer | `zlink_poll`, poller wait | receive 또는 submit retry를 수행할 가치가 있음 |
| timer fire | generic timer handle | handler 또는 timer recv | 누적 fire count가 있음 |

Monitor event는 관측 기록이고 readiness는 현재 work 존재 가능성을 알리는 level-triggered 상태다. Readiness
하나가 message 하나와 일대일로 대응하거나 다음 operation의 성공을 보장한다고 가정하지 않는다.

## 2. Raw socket lifecycle

Raw socket monitor는 endpoint bind/listen, outgoing connect, accept, handshake success/failure, disconnect,
protocol error와 close를 기록한다. Disconnect reason은 transport error, handshake failure, Context 종료와
unknown을 구분한다. Event는 service topology나 application payload를 포함하지 않는다.

## 3. Ordering과 overflow

같은 monitor queue에서는 Core가 event를 commit한 순서를 보존한다. 서로 다른 connection I/O thread 사이의
전역 wall-clock order는 제공하지 않는다. Queue overflow와 status counter의 정확한 계약은
[Monitoring](07-monitoring.ko.md)이 소유한다.
