---
title: "Connection별 memory"
---

[English](connection-memory.en.md)

<!-- zlink-nav:start -->
[가이드 목차](../guide/README.ko.md) | [이전: Socket option 기본값](socket-option-defaults.ko.md) | [다음: Multipart atomicity](multipart-atomicity.ko.md)
<!-- zlink-nav:end -->

# Connection별 memory

> **이 장이 답하는 것** — connection 하나가 실제로 어떤 메모리를 얼마나 할당하는가
> (고정 비용과 HWM에 비례하는 비용).

각 transport connection은 session, engine state, pipe endpoint, handshake buffer와 kernel
socket buffer를 할당한다. Queued message storage는 고정된 connection 비용이 아니라 effective message
size와 HWM에 따라 증가한다.

## 고정 구성 요소

- session과 engine object
- pipe metadata와 queue chunk
- routing id와 endpoint metadata
- protocol handshake state
- 운영체제 socket 구조

## 가변 구성 요소

Directional pipe는 complete message를 쓰는 동안 한 번만 byte charge를 계산한다. 이 값에는
payload와 routing frame byte가 들어가며 최소 한 개 `msg_t` 크기를 사용한다. Peer가
message 소유권을 반환하면 같은 charge를 byte credit으로 반환한다. 기존 pipe 동기화
구간에서 고정 횟수의 정수 연산만 사용하므로 allocator 조회, heap allocation, system
call 또는 hot path lock을 새로 추가하지 않는다.

Directional HWM은 이 accounted storage를 제한한다. 비어 있는 pipe는 소켓의 최대 message
크기를 넘지 않는 범위에서 HWM보다 큰 complete message 한 건을 허용하고, 그 뒤의 write를
중단한다. 끝나지 않은 multipart에는 이 예외를 적용하지 않는다. Paired transport의 hidden
Completion connection은 방향별 HWM을 262144 byte로 제한하고 network socket의 send·receive
buffer는 각각 65536 byte를 상한으로 사용한다. Monitor는 in-flight byte와 oversize 허용 이력을 제공한다. 이 값은 Core
회계를 설명하지만 process resident memory의 정확한 측정값은 아니다.

Kernel buffer는 platform autotuning에 따라 증가할 수 있다. TLS는 record와 handshake
storage를 추가한다. Monitor snapshot은 적용된 HWM 계획을 보고하지만 allocator와 kernel
overhead 전체를 측정하지는 않는다.

Capacity planning에서는 production transport와 message-size 분포를 사용해 idle, traffic 이후 잔류와
burst peak memory를 각각 측정한다.
