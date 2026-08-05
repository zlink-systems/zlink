---
title: "Socket option 기본값"
---

[English](socket-option-defaults.en.md)

<!-- zlink-nav:start -->
[가이드 목차](../guide/README.ko.md) | [이전: STREAM 소켓 최적화](stream-socket.ko.md) | [다음: Connection별 memory](connection-memory.ko.md)
<!-- zlink-nav:end -->

# Socket option 기본값

> **이 장의 계약 소유 문서** — option별 공개 계약은 [소켓 공통 명세](../spec/core/socket/README.ko.md)와
> [소켓 옵션 가이드](../guide/12-socket-options.ko.md)가 다룬다. 이 장은 내부 기본값과
> 저장 구조를 설명한다.

`options_t`는 공통 raw-socket과 transport 기본값을 저장한다. Typed socket 구현은 pattern별 option을
검증한 뒤 적용한다.

## Queue 계획

`sndhwm`과 `rcvhwm`은 accounted byte를 제한하는 64-bit 값이다. 수동 기본값은
`4,096,000` bytes이며 `0`은 무제한이다. Message count HWM을 위한 호환 state는
유지하지 않는다. Runtime에서 HWM을 줄여도 이미 queue에 있는 message는 제거하지 않는다.
보관 byte가 새 limit 아래로 감소할 때까지 새 값의 적용을 보류한다.

Automatic HWM은 raw socket role, profile, planning unit과 관찰한 connection 수로
policy를 선택한다. 선택한 slot 값에 64-bit planning unit을 곱해 최종 byte HWM을
계산한다. Slot과 effective message size field는 진단값으로 유지하지만 pipe admission은
항상 실제 보관 byte를 사용한다. Hysteresis는 connection 수 경계에서 bucket이 빠르게
반복 전환되는 것을 막는다.

Core pipe low watermark는 `ceil(hwm_bytes / 2)`다. 이 값은 byte credit 반환을
제어하며 Framework의 receive 재개 profile로 변경할 수 없다.

## Application에 보이는 상태

`zlink_monitor_status()` ABI version 2는 계획·적용·보류된 64-bit HWM byte,
보류값의 유효 여부, in-flight byte, minimum message charge와 oversize 단일 message
허용 counter를 제공한다. 이 field는 진단 snapshot이다. Application은 내부 값을
직접 바꾸지 않고 public option으로 policy 입력을 설정한다.

## Transport 기본값

Reconnect, TCP keepalive, kernel buffer, TOS, handshake interval과 TLS field는 해당 transport가
적용한다. 지원하지 않는 조합은 typed configuration result로 실패한다.
