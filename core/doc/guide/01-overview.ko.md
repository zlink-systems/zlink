---
title: "zlink 개요"
---

[English](01-overview.en.md)

<!-- zlink-nav:start -->
[가이드 목록](README.ko.md) | [다음: Socket pattern 선택](03-0-socket-patterns.ko.md)
<!-- zlink-nav:end -->

# zlink 개요

> **이 장의 계약 소유 문서** — [Core 스펙 목차](../spec/core/README.ko.md)가 다룬다. 이
> 챕터는 runtime 범위와 socket pattern을 개념 중심으로 소개한다.

zlink Core는 raw 메시징 runtime이다. Context, socket pattern, multipart message,
poller, generic timer, socket monitor, TLS와 network transport를 제공한다. Application
topology와 stateful object 동작은 Framework package가 담당한다.

## Runtime 계층

```text
+------------------------------------------------------+
| Application and language bindings                    |
+------------------------------------------------------+
| Public C API: context, socket, message, eventing      |
+------------------------------------------------------+
| Socket patterns: PAIR, PUB/SUB, XPUB/XSUB,           |
| DEALER/ROUTER, STREAM                                 |
+------------------------------------------------------+
| Protocol engines and transports                      |
+------------------------------------------------------+
| Context, I/O threads, pipes, mailboxes, messages      |
+------------------------------------------------------+
```

Public API는 handle과 인자를 검증한 뒤 socket runtime에 작업을 전달한다. 각 socket 구현은
pattern별 동작을 처리하고, protocol engine은 frame을 인코딩하며, transport는 운영체제 I/O를
수행한다.

## Socket pattern

| Socket | 용도 |
|---|---|
| PAIR | 일대일 통신 |
| PUB/SUB | topic 기반 배포 |
| XPUB/XSUB | subscription을 관찰하는 proxy |
| DEALER/ROUTER | 비동기 routed request/reply |
| STREAM | raw byte stream 연동 |

Build 설정에 따라 `tcp`, `ipc`, `inproc`, `ws`, `wss`, `tls` endpoint를 사용할 수 있다.

## 시작 순서

Core와 test를 build한다.

```bash
cmake -S core -B core/build -DWITH_TLS=ON -DBUILD_TESTS=ON
cmake --build core/build
```

[Socket Pattern](03-0-socket-patterns.ko.md)에서 통신 방식을 고른 뒤 [Core API](02-core-api.ko.md)와
[Message API](09-message-api.ko.md)를 사용한다. Raw socket event와 snapshot은
[Monitoring](06-monitoring.ko.md)에서 설명한다.

<!-- zlink-nav:bottom:start -->
[Core API →](02-core-api.ko.md)
<!-- zlink-nav:bottom:end -->
