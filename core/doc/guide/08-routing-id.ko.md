---
title: "Routing ID"
---

[English](08-routing-id.en.md)

<!-- zlink-nav:start -->
[가이드 목록](README.ko.md) | [이전: Thread safety](11-thread-safety.ko.md) | [다음: PAIR 소켓](03-1-pair.ko.md)
<!-- zlink-nav:end -->

# Routing ID

> **이 장이 답하는 것** — ROUTER와 STREAM에서 연결된 peer를 식별하는 routing id의
> 형식과 수명을 설명한다. 정확한 계약은
> [ROUTER](../spec/core/socket/07-router.ko.md)와
> [STREAM](../spec/core/socket/08-stream.ko.md) socket 스펙이 소유한다.

Routing id는 routed raw socket에 연결된 peer를 식별한다. Core는 byte sequence와 길이를 가진
`zlink_routing_id_t`로 이 값을 표현한다.

## Local id 설정

Application에 고정된 local identity가 필요하면 connect 또는 bind 전에
`zlink_set_routing_id()`를 호출한다. 설정한 값은 `zlink_get_routing_id()`로 읽는다.

```c
const char id[] = "worker-7";
zlink_set_routing_id(socket, id, sizeof(id) - 1);
/* Connection handshake가 값을 사용하기 전에 routing id를 설정한다. */
```

## 수신과 응답

`zlink_recv_part()`, `zlink_subscribe_part()`, `zlink_router_recv_part()`는 thread-local
routing-id view의 pointer를 반환한다. 같은 thread에서 다음 receive 계열 함수를 호출하면 이 view가
무효화될 수 있다.

```c
const zlink_routing_id_t *source_rid = NULL;
uint64_t request_seq = 0;
zlink_msg_t part;
zlink_part_flag_t more;

zlink_msg_init(&part);
zlink_router_recv_part(
    router, &source_rid, &request_seq, &part, &more, 0);
/* 다음 receive 전에 사용할 수 없다면 source_rid를 즉시 복사한다. */
```

일반 routed message는 수신한 routing id와 `zlink_send_part_rid()`를 사용한다. Request에
응답할 때는 반환된 sequence를 함께 전달해 `zlink_router_reply_part()`를 호출한다.

## Peer 연결 종료

`zlink_disconnect_rid()`는 일치하는 peer connection의 비동기 종료를 요청한다. 성공은 종료 요청이
접수됐다는 뜻이며 transport 종료가 동기적으로 끝났다는 뜻은 아니다.
