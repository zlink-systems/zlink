---
title: "Connection Memory"
---

[English](https://zlink-systems.github.io/zlink/spec/core/systems/05-connection-memory/) | 한국어

<!-- zlink-nav:start -->
[시스템 목차](README.ko.md) | [이전: Thread safety](04-thread-safety.ko.md) | [다음: Auto HWM](06-auto-hwm.ko.md)
<!-- zlink-nav:end -->

# Connection Memory

> **이 장이 정의하는 것** — connection 하나가 실제로 어떤 memory를 얼마나 할당하는가
> (고정 비용과 HWM에 비례하는 가변 비용).

## 1. Connection memory 개요

각 transport connection은 두 종류의 memory를 사용한다. 하나는 connection을 만들 때
할당하는 **고정 비용**이며, 구성은 inproc와 socket 기반 transport가 다르다. 다른 하나는
queue가 보관하는 message storage와 pending request lifecycle state인 **가변 비용**으로,
고정된 connection 비용이 아니다. Queue storage는 각 frame의 실제 accounted byte와, queue에 유지할 byte를 제한하는 상한인
[HWM](../glossary.ko.md#hwm)에 따라 증가한다.

이 문서는 그 두 비용의 구성과, 이를 관찰·측정할 때의 한계를 정의한다. 대상 독자는
connection당 memory 사용량을 추정하고 capacity planning을 수행하는 개발자다.

관련 계약의 소유 문서는 다음과 같다.

| 관련 계약 | 정의하는 문서 |
|---|---|
| memory budget 계산, byte 회계와 HWM admission | [Auto HWM](06-auto-hwm.ko.md) |
| [socket](../glossary.ko.md#socket) 옵션과 HWM 관찰 동작 | [Socket 공통](../socket/README.ko.md) |
| 각 용어의 짧은 정의 | [Core 용어](../glossary.ko.md) |

## 2. 고정 구성 요소

connection 하나를 만들 때 traffic 양과 무관하게 할당하는 구성 요소는 transport에 따라
다음과 같이 나뉜다.

| Transport | 고정 구성 요소 |
|---|---|
| inproc | 두 socket endpoint를 직접 연결하는 pipepair. session과 engine은 만들지 않으며 protocol handshake state와 운영체제 socket도 없다. |
| socket 기반 transport | session, engine, pipe endpoint와 queue chunk, routing ID와 endpoint metadata, protocol handshake state, 운영체제 socket 구조. TLS는 record와 handshake storage를 추가한다. |

## 3. 가변 구성 요소

> **이 절의 계약 소유** — byte 회계, HWM admission과 budget 계산의 공개 계약과 내부 구현은
> [Auto HWM](06-auto-hwm.ko.md)이 소유한다. 이 절은 그 결과를 connection 하나의 memory 비용
> 관점에서 요약한다.

### 3.1 Frame byte charge

한 application 방향의 message를 담는 물리 queue를
[directional queue](../glossary.ko.md#directional-queue)라 한다. Directional pipe는 frame마다
payload와 `sizeof(zlink_msg_t)`를 byte charge로 계산한다. 이 charge는 다음 순서로 예약되고
반환된다.

1. 수신한 wire byte를 frame으로 해석하는 decoder는 frame length를 확인한 직후 origin queue의
   provisional credit을 먼저 얻고, 그 뒤에만 payload buffer를 할당한다.
2. Multipart 마지막 frame에서는 같은 provisional 합계를 committed message로 전환하며 counter를
   다시 증가시키지 않는다.
3. Write 실패, rollback, close와 detach는 실제로 제거한 provisional·committed frame charge를
   정확히 한 번 반환한다.

### 3.2 빈 pipe oversize 예외

비어 있는 application pipe는 socket의 최대 message 크기를 넘지 않는 범위에서 HWM보다 큰
complete message 한 건을 허용하고, 그 뒤의 write를 중단한다. 끝나지 않은 multipart에는 이
예외를 적용하지 않는다.

### 3.3 Pending request lifecycle state

Pending map entry, callback과 timeout state는 live request 수에 따라 증가하며
[pending request 수용 한도](06-auto-hwm.ko.md#pending-request-수용)의 physical pair별 32 MiB
size-weighted work budget과 unresolved request count 한도 16,384가 completion liveness를
제한한다. Work charge는 실제 allocator byte나 보관 중인 payload byte가 아니며, queue HWM의
current·snapshot 값에 포함되지 않는다. Application HWM은 queue에 머무르는 frame만 제한하고
unresolved correlation의 별도 lifecycle 한도로 재사용되지 않는다.

### 3.4 Completion progress lane

ROUTER-ROUTER의 [completion progress lane](../glossary.ko.md#completion-progress-lane)은
terminal reply와 error reply를 진행시키고, peer 사이의 receive-flow-state frame을
동기화하는 별도 경로다. 이 lane에는 byte HWM, LWM, manual HWM과 Core budget
reservation을 적용하지 않는다. Application pipe가 가득 차도 유효한 completion record와
receive-flow-state frame은 connection이 유지되고 allocation이 성공하면 수용한다.

DEALER-ROUTER에는 이 별도 connection이 없다. Reply·error reply byte는 DATA·REQUEST와 같은
Application physical queue의 accounted byte에 포함되며 HWM과 peer PAUSED를 적용한다.

Completion lane의 `SNDBUF`·`RCVBUF` 기본값 `-1`은 transport 종류와 관계없이 OS 기본값과
autotuning을 유지한다. Application이 0 이상의 값을 명시하면 completion lane에는 최대 64 KiB로
제한한 값을 TCP·TLS·WS·WSS의 기반 TCP socket에 동일하게 적용한다.

## 4. 측정과 한계

실행 중 memory 상태를 조회하는 monitor는 application queue와 completion lane의 current byte,
oversize 허용 이력을 구분해 제공한다. 제거된 retained-credit 기능에 할당됐던
`application_accounted_bytes`·`outstanding_application_lease_count`·
`deferred_origin_credit_bytes`·`retired_queue_count` field는 ABI 호환을 위해 남아 있으며 항상
0이다. [Core budget](../glossary.ko.md#auto-hwm-budget) — Core가
memory 입력에서 계산해 application queue들의 HWM을 나눌 때 기준으로 삼는 byte 총량 — 은 정상
상태의 pipe별 HWM 분배 기준이지 context 실제 사용량 hard cap이 아니다. 이 값은 Core 회계를
설명하지만 process resident memory의 정확한 측정값은 아니다.

Kernel buffer는 platform autotuning에 따라 증가할 수 있다. TLS는 record와 handshake storage를
추가한다. Monitor snapshot은 적용된 HWM 계획을 보고하지만 allocator와 kernel overhead 전체를
측정하지는 않는다.

DEALER-ROUTER는 logical peer마다 physical connection 하나를 사용하므로 ROUTER-ROUTER보다 idle
session·engine·file descriptor와 기반 kernel·TLS·WebSocket storage가 한 connection만큼 적다.
이 차이는 Core budget을 process resident memory hard cap으로 바꾸지 않으며, kernel autotuning과
TLS storage가 monitor snapshot 밖이라는 경계도 바꾸지 않는다.

## 5. Capacity planning

Capacity planning에서는 production transport와 message-size 분포를 사용해 다음 세 값을 각각
측정한다.

- idle memory
- traffic 이후 잔류 memory
- burst peak memory

## 6. 구현 및 contract test 검증 요구

byte 회계, admission, dequeue credit과 oversize 예외의 상세 검증 항목은
[Auto HWM의 검증 요구](06-auto-hwm.ko.md#5-구현-및-contract-test-검증-요구)가 소유한다.
connection memory 관점에서 확인할 항목은 다음과 같다. 각 항목은 test 하나로 이어진다.

**byte charge**
- frame 하나를 수락하면 그 pipe의 accounted byte가 payload에 `sizeof(zlink_msg_t)`를 더한 만큼 증가한다.
- multipart 마지막 frame은 provisional 합계를 committed message로 전환할 뿐 counter를 다시 증가시키지 않는다.
- write 실패, rollback, close와 detach 뒤 실제 제거된 frame의 provisional·committed charge는 정확히 한 번 반환된다.

**HWM과 dequeue credit**
- application directional HWM은 Core queue가 현재 보관하는 physical frame byte에만 적용된다.
- Core queue가 complete message를 dequeue해 binding에 넘기면 그 message의 queue charge가 끝나고 writer credit을 반환한다. Binding이나 application이 payload를 계속 보유하는 수명은 Core HWM에 다시 계상하지 않는다.

**oversize와 completion**
- 빈 application pipe에 socket 최대 message 크기 이내이며 HWM보다 큰 complete message를 보내면 한 건 수락되고, 그 뒤의 write는 중단된다.
- 끝나지 않은 multipart에는 빈 pipe oversize 예외가 적용되지 않는다.
- ROUTER-ROUTER application pipe가 가득 찬 상태에서도 유효한 reply·error reply는 connection이
  유지되고 allocation이 성공하면 Completion lane으로 수용된다.
- RUNNING·PAUSED receive-flow-state frame은 DEALER-ROUTER에서 single Application connection의
  Core control 경로로, ROUTER-ROUTER에서 Completion lane으로 동기화된다.
- ROUTER-ROUTER completion progress lane에는 byte HWM, LWM, manual HWM과 Core budget reservation이 적용되지 않는다.
- DEALER-ROUTER reply·error reply는 Application physical queue byte에 포함되고 HWM·PAUSED를
  적용하며 Completion current·peak·pending 회계에는 포함되지 않는다.

**측정**
- monitor는 application queue·completion의 current byte와 oversize 허용 이력을 구분해 보고하고, retained-credit 호환 field는 항상 0으로 보고한다.
- Core budget은 정상 상태의 pipe별 HWM 분배 기준이며 context 실제 사용량 hard cap으로 동작하지 않는다 (상세 admission 계약은 [Auto HWM](06-auto-hwm.ko.md) 소유).
- 같은 transport에서 DEALER-ROUTER logical peer는 physical connection 하나, ROUTER-ROUTER는
  두 개를 사용한다. Idle resource 감소를 관찰해도 allocator·kernel·TLS를 포함한 process hard cap은
  제공되지 않는다.
