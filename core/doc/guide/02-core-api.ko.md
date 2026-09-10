---
title: "Core C API"
---

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
operation이 실행 중이면 close가 busy를 반환할 수 있다.

## 설정

공통 option은 `zlink_set_option()`과 `zlink_get_option()`으로 처리한다. Router, dealer,
stream, pub, sub family는 typed option 함수를 제공한다. Routing id와 TLS는 connection
handshake가 값을 사용하기 전에 설정한다.

## Message I/O

Core는 한 호출에 message record 전체를 주고받는다. Multipart 경계는 `zlink_msg_t` 배열의
순서와 part 수로 표현한다.

- `zlink_send()`는 일반 raw traffic의 모든 part를 배열로 보낸다.
- `zlink_send_rid()`는 routed peer를 선택하고 모든 part를 배열로 보낸다.
- `zlink_publish()`는 topic과 payload part 배열을 발행한다.
- `zlink_recv()`·`zlink_router_recv()`·`zlink_subscribe()`는 record 전체를 caller가 제공한
  배열에 채우고 part 수를 반환한다. `zlink_xpub_recv()`는 subscription event를 받는다.
- DEALER와 ROUTER request는 0이 아닌 completion ID를 반환한다. Reply와 terminal 결과는
  `zlink_completion_recv()`로 받고 각 record를 `zlink_completion_close()`로 해제한다.

수신 배열의 용량이 record의 part 수보다 작으면 record를 소비하지 않고
`ZLINK_RECV_BUFFER_TOO_SMALL`(`ENOBUFS`)과 필요한 수를 반환한다. 배열을 키워 다시 호출하면 같은
record를 정확히 한 번 받는다. 받은 배열은
[`zlink_multipart_close()`](../spec/core/02-message.ko.md#zlink_multipart_close)로 닫는다.

Send에 전달한 배열의 모든 message part는 성공·실패와 관계없이 Core가 소비한다. 호출 뒤 각 슬롯은
빈 초기화 상태로 남으므로 다시 보내려면 호출 전에 record 전체를 복사해 둔다.

## Eventing

Poller는 socket, file descriptor와 generic timer readiness를 기다린다. Socket monitor는 raw
transport와 protocol event를 보고하고 현재 status snapshot을 제공한다. Generic timer는 직접
receive하거나 poller readiness 뒤에 receive한다.

Result 값, ownership과 concurrency의 정확한 계약은 public header 주석과
[Core spec](../spec/core/README.ko.md)을 기준으로 한다.
