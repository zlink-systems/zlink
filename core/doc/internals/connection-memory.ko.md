---
title: "Connection별 memory"
---

[English](connection-memory.ko.md)

<!-- zlink-nav:start -->
[가이드 목차](../guide/README.ko.md) | [이전: Socket option 기본값](socket-option-defaults.ko.md) | [다음: Multipart atomicity](multipart-atomicity.ko.md)
<!-- zlink-nav:end -->

# Connection별 memory

> **이 장이 답하는 것** — connection 하나가 실제로 어떤 메모리를 얼마나 할당하는가
> (고정 비용과 HWM에 비례하는 비용).

각 transport connection은 session, engine state, pipe endpoint, handshake buffer와 kernel
socket buffer를 할당한다. Queued message storage는 고정된 connection 비용이 아니라 각 frame의
실제 accounted byte와 HWM에 따라 증가한다.

## 고정 구성 요소

- session과 engine object
- pipe metadata와 queue chunk
- routing id와 endpoint metadata
- protocol handshake state
- 운영체제 socket 구조

## 가변 구성 요소

Directional pipe는 frame마다 payload와 `sizeof(msg_t)`를 byte charge로 계산한다. Decoder는
frame length를 확인한 직후 origin queue의 provisional credit을 먼저 얻고 그 뒤에만 payload
buffer를 할당한다. Multipart 마지막 frame에서는 같은 provisional 합계를 committed message로
전환하며 counter를 다시 증가시키지 않는다. Write 실패, rollback, close와 detach는 실제로
제거한 provisional·committed frame charge를 정확히 한 번 반환한다.

Application directional HWM은 physical queue byte와 그 queue에서 Framework로 이전된
retained-credit lease byte를 함께 제한한다. Retained receive는 queue charge를 줄이지 않고
owner만 application lease로 바꾸며, release가 exact origin generation의 read credit을
반환한다. Origin이 먼저 detach되면 마지막 lease가 반환될 때까지 retired registry entry를
유지한다.

비어 있는 application pipe는 소켓의 최대 message 크기를 넘지 않는 범위에서 HWM보다 큰
complete message 한 건을 허용하고, 그 뒤의 write를 중단한다. 끝나지 않은 multipart에는
이 예외를 적용하지 않는다. DEALER·ROUTER completion progress lane은 terminal reply와 error
reply 전용이며 byte HWM, LWM, manual HWM과 Core budget reservation을 적용하지 않는다.
Application pipe가 가득 차도 유효한 completion record는 connection이 유지되고 allocation이
성공하면 수용한다.

Monitor는 queue·application lease·completion의 current byte와 oversize 허용 이력을 구분해
제공한다. Core budget은 정상 상태의 pipe별 HWM 분배 기준이지 context 실제 사용량 hard
cap이 아니다. 이 값은 Core 회계를 설명하지만 process resident memory의 정확한 측정값은
아니다.

Kernel buffer는 platform autotuning에 따라 증가할 수 있다. TLS는 record와 handshake
storage를 추가한다. Monitor snapshot은 적용된 HWM 계획을 보고하지만 allocator와 kernel
overhead 전체를 측정하지는 않는다.

Capacity planning에서는 production transport와 message-size 분포를 사용해 idle, traffic 이후 잔류와
burst peak memory를 각각 측정한다.
