---
title: "Socket option 기본값"
---

[English](socket-option-defaults.ko.md)

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
보관 byte가 새 limit 아래로 감소하는 순간 deferred shrink를 적용한다.

Automatic HWM은 context의 Core memory budget, profile 역할 경계와 고유 physical
directional queue registry를 사용한다. Registry는 같은 inproc ypipe를 endpoint마다
중복 등록하지 않고 stable queue ID와 generation으로 한 번만 센다. Manual reservation을
뺀 budget은 역할별 하한에서 시작해 상한에 도달하지 않은 physical queue에 bounded
water-filling으로 나눈다. 나눗셈 remainder는 stable queue ID 순서로 1 byte씩 지급한다.

Inproc 양 endpoint 값은 더하지 않는다. 한쪽만 finite manual이면 그 cap, 양쪽이 finite
manual이면 더 작은 cap, 한쪽이 unlimited manual이고 다른 쪽이 auto이면 auto plan을
사용한다. 양쪽이 unlimited면 admission은 unlimited로 유지하되 역할별 상한을 계산용으로
한 번 예약한다.

DEALER·ROUTER completion progress lane은 terminal reply와 error reply 전용이다. 이
lane에는 auto/manual HWM, LWM, inproc boost, 역할별 경계와 Core budget reservation을
적용하지 않는다. Auto HWM을 비활성화하면 live pipe의 마지막 applied HWM을 유지하고
이후 automatic planning에서 제외한다.

Core pipe low watermark는 `ceil(hwm_bytes / 2)`다. 이 값은 byte credit 반환을
제어하며 Framework의 receive 재개 profile로 변경할 수 없다.

## Application에 보이는 상태

`zlink_monitor_status()` ABI version 3은 계획·적용·보류된 64-bit HWM byte,
pending message count와 pending byte, in-flight byte, minimum message charge와
oversize 단일 message 허용 counter를 제공한다. Context budget snapshot은 physical queue
capacity, provisional·committed queue byte, application-held lease와 completion·monitor
queue를 각각 구분한다. 이 field는 진단 snapshot이다. Application은 내부 값을 직접
바꾸지 않고 public option으로 policy 입력을 설정한다.

## Transport 기본값

Reconnect, TCP keepalive, kernel buffer, TOS, handshake interval과 TLS field는 해당 transport가
적용한다. 지원하지 않는 조합은 typed configuration result로 실패한다.
