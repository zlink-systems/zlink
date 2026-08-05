---
title: "Core C API"
---

[English](02-core-api.en.md)

<!-- zlink-nav:start -->
[가이드 목록](README.ko.md) | [이전: Core 성능](10-performance.ko.md) | [다음: Socket option](12-socket-options.ko.md)
<!-- zlink-nav:end -->

# Core C API

> **이 장의 계약 소유 문서** — [Core 스펙 목차](../spec/core/README.ko.md)가 다룬다. 이
> 챕터는 context·socket·eventing API를 사용 순서대로 소개한다.

`<zlink.h>`를 include한다. `zlink_ctx_new()`로 context를 만들고 `zlink_socket()`으로 typed raw
socket을 만든다. 모든 socket을 닫은 뒤 context를 종료한다.

## Socket lifecycle

`zlink_bind()`와 `zlink_connect()`로 endpoint를 설정한다. `zlink_unbind()`와
`zlink_disconnect()`는 endpoint를 제거한다. `zlink_close()`는 socket resource를 해제한다. 다른
operation이나 callback이 실행 중이면 close가 busy를 반환할 수 있다.

## 설정

공통 option은 `zlink_set_option()`과 `zlink_get_option()`으로 처리한다. Router, dealer,
stream, pub, sub family는 typed option 함수를 제공한다. Routing id와 TLS는 connection
handshake가 값을 사용하기 전에 설정한다.

## Message I/O

Core는 part 단위 multipart API를 사용한다.

- `zlink_send_part()`는 일반 raw traffic을 보낸다.
- `zlink_send_part_rid()`는 routed peer를 선택한다.
- `zlink_publish_part()`는 topic과 payload를 발행한다.
- Typed receive 함수는 part 하나와 `ZLINK_PART_MORE` 또는 `ZLINK_PART_FINAL`을 반환한다.
- DEALER와 ROUTER request 함수는 `zlink_reply_handler_fn`으로 완료 결과를 전달한다.

성공한 send가 message part를 소비하기 전까지 caller가 소유한다. 수신한 part는 정확히 한 번 close하거나
move해야 한다.

## Eventing

Poller는 socket, file descriptor와 generic timer readiness를 기다린다. Socket monitor는 raw
transport와 protocol event를 보고하고 현재 status snapshot을 제공한다. Generic timer는 receive,
callback 또는 poller로 소비할 수 있다.

Result 값, ownership과 concurrency의 정확한 계약은 public header 주석과
[Core spec](../spec/core/README.ko.md)을 기준으로 한다.
