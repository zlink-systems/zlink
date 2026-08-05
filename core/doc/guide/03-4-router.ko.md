---
title: "ROUTER"
---

[English](03-4-router.en.md)

<!-- zlink-nav:start -->
[가이드 목록](README.ko.md) | [이전: DEALER](03-3-dealer.ko.md) | [다음: STREAM](03-5-stream.ko.md)
<!-- zlink-nav:end -->

# ROUTER

> **이 장의 계약 소유 문서** — [ROUTER socket 스펙](../spec/core/socket/07-router.ko.md)이
> 다룬다. 이 챕터는 그 계약을 언어별 예제로 보여준다.

ROUTER는 수신 메시지와 함께 routing id를 반환하고, 송신할 때 이 값을 사용해 peer를 선택한다.
하나의 socket이 여러 DEALER 또는 ROUTER peer와 통신할 때 사용한다.

## 메시지 수신

`zlink_router_recv_part()`는 payload를 part 단위로 반환한다. Routing id view는 같은 thread에서
다음 receive 계열 함수를 호출하기 전까지만 유효하다. 그 이후에도 사용해야 하면 복사한다.

```c
const zlink_routing_id_t *source_rid = NULL;
uint64_t request_seq = 0;
zlink_msg_t part;
zlink_part_flag_t more;

zlink_msg_init(&part);
zlink_recv_result_t rc = zlink_router_recv_part(
    router, &source_rid, &request_seq, &part, &more, 0);
/* source_rid는 peer를 식별하고, more는 multipart의 다음 part 여부를 나타낸다. */
```

일반 routed message에서는 `request_seq`가 0이다. 0보다 큰 값은
`zlink_router_reply_part()`로 응답할 수 있는 request를 나타낸다.

## Routed message 송신

`zlink_send_part_rid()`에 peer routing id를 전달한다. 마지막 전 part에는
`ZLINK_PART_MORE`, 마지막 part에는 `ZLINK_PART_FINAL`을 사용한다.

## Request와 reply

`zlink_router_request_part()`는 routed request를 제출하고 reply callback으로 완료 결과를 전달한다.
수신한 request에는 receive 결과의 source routing id와 request sequence를 사용해
`zlink_router_reply_part()`로 응답한다.

ROUTER mandatory와 handover 동작은 [Socket Option](12-socket-options.ko.md)의 typed router
option으로 설정한다. Routing id 수명과 복사 규칙은 [Routing ID](08-routing-id.ko.md), 같은
handle의 동시 사용 조건은 [Thread Safety](11-thread-safety.ko.md)를 참고한다.
