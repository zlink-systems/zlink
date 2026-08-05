---
title: "ZMP protocol"
---

<!-- zlink-nav:start -->
[가이드 목록](README.ko.md) | [이전: Socket option](12-socket-options.ko.md) | [다음: Core 용어](glossary.ko.md)
<!-- zlink-nav:end -->

# ZMP protocol

> **이 장이 답하는 것** — socket 사이에서 handshake와 message frame을 교환하는 wire
> protocol의 개념을 소개한다. 정확한 frame 형식은
> [ZMP protocol internals](../internals/protocol-zmp.ko.md)가 소유한다.

ZMP는 zlink raw socket 사이에서 handshake와 message frame을 교환하는 wire protocol이다. Application은
일반적으로 protocol을 직접 구현하지 않고 socket API를 사용한다.

## Frame

ZMP data frame은 message boundary와 multipart continuation을 표현한다. Routing id와 subscription 같은
socket metadata는 해당 socket pattern의 typed API가 payload와 분리해 반환한다.

## Handshake와 metadata

Connection handshake는 socket type, identity와 protocol metadata를 교환한다. Transport가 TLS나 WSS면
TLS handshake가 먼저 완료돼야 ZMP handshake를 진행한다.

## Request/reply envelope

DEALER와 ROUTER의 typed request/reply API는 Core가 관리하는 control part로 message type과 request
sequence를 전달한다. Application payload는 control part 뒤에 유지되며 receive API는 control part를
제거한 payload와 request metadata를 반환한다.

Wire format의 구현 상세는 [ZMP internals](../internals/protocol-zmp.ko.md)를 참고한다. Application
topology protocol은 Framework package가 소유하며 Core ZMP guide의 범위에 포함되지 않는다.
