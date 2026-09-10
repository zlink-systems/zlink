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

Core는 part 단위 multipart API를 사용한다.

- `zlink_send_part()`는 일반 raw traffic을 보낸다.
- `zlink_send_part_rid()`는 routed peer를 선택한다.
- `zlink_publish_part()`는 topic과 payload를 발행한다.
- Typed receive 함수는 part 하나와 `ZLINK_PART_MORE` 또는 `ZLINK_PART_FINAL`을 반환한다.
- DEALER와 ROUTER request는 0이 아닌 completion ID를 반환한다. Reply와 terminal 결과는
  `zlink_completion_recv()`로 받고 각 record를 `zlink_completion_close()`로 해제한다.

수신에는 두 가지 형태가 있다.

- **part 단위** `*_recv_part()` — 호출당 part 하나와 `has_more`를 돌려준다. 단건이거나 part를
  하나씩 흘려보내며(부분 소비) 처리할 때, 그리고 초저할당 경로에 쓴다.
- **whole-message** `zlink_recv()`(PAIR·DEALER)·`zlink_router_recv()`(ROUTER) — record의 모든 part를
  한 번의 호출로 caller-제공 `zlink_msg_t` 배열에 채운다. 멀티파트가 대부분인 워크로드에서 호출·경계
  수를 줄인다. 배열 용량이 record의 part 수보다 작으면 record를 소비하지 않고 `ZLINK_RECV_BUFFER_TOO_SMALL`
  (`ENOBUFS`)과 함께 필요한 수를 돌려주므로, 배열을 키워 재시도하면 같은 record를 정확히 한 번 받는다.
  받은 배열은 [`zlink_multipart_close()`](../spec/core/02-message.ko.md#zlink_multipart_close)로 한 번에 닫는다.

무엇을 쓸지: 대부분-멀티파트면 `recv`(호출·할당 절감), 단건·스트리밍·초저할당이면 `recv_part`. 둘은
같은 socket에서 공존하며 계약(single-consumer·record 원자성)은 동일하다.

`*_part` send에 전달한 message part는 성공·실패와 관계없이 Core가 소비한다 — 호출 뒤 그 part는
빈 초기화 상태로 남으므로 다시 보내려면 호출 전에 복사해 둔다. 수신한 part는 정확히 한 번 close하거나
move해야 한다.

## Eventing

Poller는 socket, file descriptor와 generic timer readiness를 기다린다. Socket monitor는 raw
transport와 protocol event를 보고하고 현재 status snapshot을 제공한다. Generic timer는 직접
receive하거나 poller readiness 뒤에 receive한다.

Result 값, ownership과 concurrency의 정확한 계약은 public header 주석과
[Core spec](../spec/core/README.ko.md)을 기준으로 한다.
