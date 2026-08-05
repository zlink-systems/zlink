---
title: "Socket option"
---

[English](12-socket-options.en.md)

<!-- zlink-nav:start -->
[가이드 목록](README.ko.md) | [이전: Core C API](02-core-api.ko.md) | [다음: ZMP protocol](zmp-protocol.ko.md)
<!-- zlink-nav:end -->

# Socket option

> **이 장이 답하는 것** — `zlink_set_option()`·`zlink_get_option()`으로 다루는 공통·
> socket별 option을 정리한다. 정확한 값 범위는 각 socket 스펙이 소유한다.

공통 raw-socket option은 `zlink_set_option()`과 `zlink_get_option()`으로 처리한다. Router,
dealer, stream, pub, sub option은 각 typed accessor를 사용한다. 지원하지 않는 option은
`ZLINK_CONFIG_NOT_SUPPORTED`를 반환한다.

## 설정 시점

Routing id, TLS credential, transport buffer와 handshake 관련 값은 bind나 connect 전에 설정한다.
Runtime queue option이 기존 pipe에 미치는 영향은 public contract에 명시된 범위만 보장한다.

## Queue와 timeout option

- `ZLINK_OPT_SNDHWM`과 `ZLINK_OPT_RCVHWM`은 queue에 쌓인 byte를 제한한다. 값은 `uint64_t`
  byte이고 `0`은 무제한이다. 정확한 계산 방법과 오류는
  [socket spec](../spec/core/socket/README.ko.md)을 본다.
- `ZLINK_OPT_SNDTIMEO`와 `ZLINK_OPT_RCVTIMEO`는 blocking call 시간을 제한한다.
- `ZLINK_OPT_LINGER`는 close가 pending outbound data를 처리하는 방식을 정한다.
- Automatic-HWM option은 profile과 message 단위 byte 입력을 설정한다.

## Transport option

TCP keepalive, reconnect interval, kernel send/receive buffer, TOS와 TLS 설정은 transport
동작에 영향을 준다. Option에 의존하기 전에 platform 지원 여부를 확인한다.

Application code에서 내부 계산을 다시 구현하지 말고 `zlink_monitor_status()`로 적용된
automatic-HWM 계획을 읽는다.
