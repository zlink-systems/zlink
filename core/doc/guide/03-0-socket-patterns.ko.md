---
title: "Socket pattern 선택"
---

[English](03-0-socket-patterns.en.md)

<!-- zlink-nav:start -->
[가이드 목록](README.ko.md) | [이전: 개요](01-overview.ko.md) | [다음: Raw messaging 신뢰성](reliability.ko.md)
<!-- zlink-nav:end -->

# Socket pattern 선택

> **이 장의 계약 소유 문서** — [Core socket 스펙 목차](../spec/core/socket/README.ko.md)가
> 다룬다. 이 챕터는 통신 요구에 맞는 socket pattern을 고르는 기준을 비교한다.

Message 방향, peer 선택 방식과 framing 요구를 기준으로 pattern을 고른다.

| 요구 사항 | Pattern |
|---|---|
| 일대일 통신 | PAIR |
| Topic 배포 | PUB/SUB |
| Subscription을 관찰하는 proxy | XPUB/XSUB |
| 비동기 client와 worker | DEALER |
| 명시적인 peer routing | ROUTER |
| 외부 byte-stream client | STREAM |

## 공통 수신 방식

Raw socket은 일반적으로 poller와 part 단위 receive를 함께 사용한다. Socket을
`ZLINK_POLLIN`으로 등록해 기다린 뒤 multipart가 `ZLINK_PART_FINAL`에 도달할 때까지 typed
receive 함수를 호출한다.

- PAIR는 `zlink_recv_part()`를 사용한다.
- SUB는 topic을 별도로 반환하는 `zlink_subscribe_part()`를 사용한다.
- XPUB은 subscription 알림에 `zlink_xpub_recv_part()`를 사용한다.
- DEALER는 request/reply traffic에 `zlink_dealer_recv_part()`를 사용한다.
- ROUTER는 peer와 request metadata를 반환하는 `zlink_router_recv_part()`를 사용한다.
- STREAM은 `zlink_recv_handler()` 또는 `zlink_stream_packet_handler()`를 사용할 수 있다.

Monitor handle과 generic timer도 같은 poller에 등록할 수 있다.

## Routing id로 연결 종료

`zlink_disconnect_rid()`는 routing id가 일치하는 peer connection의 종료를 요청한다. Receive
metadata로 peer를 식별했지만 endpoint string을 저장하지 않았을 때 사용한다.

## 상세 가이드

- [PAIR](03-1-pair.ko.md), [PUB/SUB](03-2-pubsub.ko.md), [DEALER](03-3-dealer.ko.md)
- [ROUTER](03-4-router.ko.md), [STREAM](03-5-stream.ko.md), [Proxy](03-6-proxy.ko.md)
