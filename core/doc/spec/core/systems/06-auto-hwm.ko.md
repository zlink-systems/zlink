---
title: "Auto HWM 내부 설계"
---

[English](https://zlink-systems.github.io/zlink/spec/core/systems/06-auto-hwm/) | 한국어

<!-- zlink-nav:start -->
[시스템 목차](README.ko.md) | [이전: Connection별 memory](05-connection-memory.ko.md) | [다음: Source layout](07-core-source-layout.ko.md)
<!-- zlink-nav:end -->

# Auto HWM 내부 설계

이 문서는 Core 유지보수자가 Auto HWM의 memory 제한을 구현할 때, message 처리 경로에서
어떤 상태를 읽고 변경해야 하는지 정의한다. Application이 관찰하는 budget 계산, HWM
적용 범위와 오류는 [Context 스펙](../01-context.ko.md#auto-hwm-memory-budget-계산)과
[Socket 스펙](../socket/README.ko.md#transportbuffer)가 소유한다.

## HWM이 제한하는 값

Auto HWM은 context memory budget을 application용 directional queue에 나누어 각 queue의
HWM을 정한다. Message를 받아들일지는 context 전체 사용량이 아니라, 그 message가 들어갈
physical queue의 미반환 byte와 적용된 HWM으로 판단한다.

한 physical queue의 미반환 byte는 다음 값의 합이다.

```text
frameCharge = payloadBytes + sizeof(msg_t)

outstandingCharge =
    provisionalCharge
  + committedQueueCharge
  + retainedLeaseCharge
```

`sizeof(msg_t)`는 allocator 사용량을 측정한 값이 아니다. Payload가 없는 frame도 queue
slot과 message object를 사용하므로, byte HWM에서 비용이 0이 되지 않게 하는 고정값이다.
Payload만 합산하면 빈 single-part message나 빈 multipart frame을 HWM과 관계없이 계속
보관할 수 있으므로 memory 제한으로 사용할 수 없다.

`provisionalCharge`는 decoder가 payload buffer를 할당하기 전에 예약한 값이다.
`committedQueueCharge`는 queue가 보관하는 frame의 값이다. Queue에서 꺼낸 message를
Application이 계속 보유하면 그 값은 `retainedLeaseCharge`로 이동한다. 소유 위치가
바뀌어도 writer가 돌려받지 못한 합계는 변하지 않는다.

예를 들어 HWM이 1,024 byte이고 미반환 charge가 900 byte이면, charge가 124 byte 이하인
frame만 일반 규칙으로 받아들인다. 다른 queue가 비어 있거나 context 전체 합계가 budget
아래라는 사실은 이 판단을 바꾸지 않는다.

## 책임 분리

| 처리 위치 | 입력 | 결과 |
|---|---|---|
| Budget planner | memory 입력, profile, application queue 목록 | queue별 목표 HWM |
| Queue 설정 경로 | 목표 HWM, 현재 적용값, queue generation | 같은 generation에 적용할 HWM |
| Message 처리 경로 | 대상 queue의 미반환 charge, frame charge | 수락 또는 backpressure |
| Snapshot 경로 | queue별 HWM과 charge | context 조회 결과 |

Budget planner는 option 변경과 queue 연결·해제 때만 실행한다. Planner가 만든 context 전체
합계와 snapshot 통계는 message 수락 조건으로 사용하지 않는다.

## Message 처리 순서

일반 frame은 다음 순서로 처리한다.

1. Writer 또는 decoder가 payload 크기에 고정 frame 비용을 더해 candidate charge를 구한다.
2. 덧셈이 `uint64_t` 범위를 넘으면 frame을 받아들이지 않는다.
3. Decoder는 payload buffer를 할당하기 전에 대상 queue의 local 상태에 candidate charge를
   예약한다. Application writer가 이미 만든 message를 제출할 때는 이 단계를 생략한다.
4. 적용된 HWM이 0이 아니고 미반환 charge에 candidate charge를 더한 값이 HWM보다 크면
   frame을 받아들이지 않는다. 공개 스펙의 빈 queue oversize 예외는 별도로 적용한다.
5. Enqueue가 끝나면 예약한 값을 다시 더하지 않고 provisional 상태에서 committed 상태로
   바꾼다.
6. Queue가 frame을 제거하면 committed 값을 줄인다. Application이 frame을 계속 보유하면
   같은 값을 retained lease로 옮기고, 그렇지 않으면 writer에 byte credit을 반환한다.
7. Drop, allocation 실패, protocol 오류와 종료는 자신이 실제로 보유한 값을 한 번만
   반환한다.

이 순서에서 정상 frame 처리는 queue와 함께 생성한 local 상태만 읽고 변경한다.

## Multipart와 큰 message

Multipart는 각 frame의 charge를 누적한다. Decoder는 wire header에서 frame payload 크기를
확인한 뒤 buffer allocation 전에 그 frame의 charge를 예약한다. 마지막 frame은 앞에서
예약한 값을 다시 증가시키지 않고 multipart 전체를 읽을 수 있게 공개한다.

최종 크기를 아직 모르는 multipart가 HWM에 도달하면 다음 frame의 buffer를 할당하기 전에
멈춘다. Allocation 실패나 protocol 오류로 multipart를 폐기하면 그 multipart가 예약하거나
queue에 기록한 charge를 모두 반환한다.

비어 있는 queue에는 전체 charge를 admission 시점에 아는 complete message 한 건을 HWM보다
크더라도 받아들일 수 있다. 이 예외는 두 message에 동시에 적용하지 않으며
`ZLINK_OPT_MAXMSGSIZE` 검사를 건너뛰지 않는다. 자세한 공개 동작은
[Socket 스펙의 HWM 설명](../socket/README.ko.md#transportbuffer)을 따른다.

## Retained receive와 queue generation

Queue에서 꺼낸 frame의 memory를 Application이 반환할 때까지 유지하는 receive를 retained
receive라고 한다. Retained receive는 queue에서 frame을 제거할 때 charge를 반환하지 않고,
lease가 끝날 때 원래 queue generation의 writer에 반환한다.

Queue를 detach하거나 다시 연결하면 새 generation을 만든다. 이전 generation의 lease가
끝나도 새 generation의 charge를 줄이거나 writer를 깨우지 않는다. 이전 generation은 마지막
lease와 예약이 끝날 때까지 반환 대상만 유지한 뒤 제거한다.

## HWM 변경

HWM을 늘리면 현재 queue generation에 새 값을 적용한다. HWM을 줄였을 때 미반환 charge가
새 목표보다 크면 이미 받아들인 frame을 제거하지 않는다. 새 frame을 받지 않고 charge가
목표 이하가 될 때까지 기다린 뒤 새 HWM을 적용한다.

DEALER·ROUTER가 terminal reply와 error reply를 진행시키는 completion queue에는 application
HWM을 적용하지 않는다. Monitor queue도 application budget을 나누는 queue 목록에서 제외한다.

## Message 처리 경로의 비용 제한

Send, receive와 decoder admission 경로에서는 다음 작업을 수행하지 않는다.

- context 전체 mutex 획득
- queue ID나 reservation ID를 찾기 위한 전역 map 탐색
- reservation을 위한 frame별 heap allocation
- context 전체 current·provisional·peak 합계의 frame별 갱신
- HWM 판단에 사용하지 않는 통계의 global atomic 갱신
- 다른 physical queue의 사용량 조회

Decoder reservation은 decoder 또는 pipe가 소유한 inline 상태로 표현한다. 필요한 값은 대상
queue 참조, generation, reserved charge와 예약 여부다. Queue lifecycle registry는 queue
연결·해제와 이전 generation 정리에 사용할 수 있지만, 정상 frame을 받을 때마다 조회하지
않는다.

## Snapshot과 통계

Snapshot은 조회 시점에 queue별 local 상태를 모아 context 합계를 만든다. Snapshot을 만드는
동안에는 필요한 registry lock을 사용할 수 있지만, message 처리 경로와 같은 lock으로 모든
queue를 직렬화하지 않는다.

Peak 통계는 snapshot 조회와 Auto HWM 재계산이 queue별 값을 모은 시점의 합계 중 가장 큰
값을 기록한다. Frame마다 context 전체 합계를 갱신하지 않으므로 두 관측 시점 사이에서만
유지된 값은 peak에 포함되지 않을 수 있다. 통계를 reset하거나 snapshot 조회를 반복해도 HWM
수락 결과와 writer credit은 바뀌지 않는다.

## 구현 위치

| 책임 | 구현 위치 |
|---|---|
| Profile 경계와 queue별 HWM 계산 | `auto_hwm_policy.*` |
| Context 입력, 재계산과 snapshot API | `ctx_auto_hwm_*` |
| Physical queue identity와 generation | `ctx_physical_queue_registry.*` |
| Queue-local charge, HWM 판단과 byte credit | `pipe.*` |
| Allocation 전 reservation | `zmp_decoder.*`, `session_base_pipe_io.cpp`, `pipe.*` |
| Retained lease release | retained receive API와 queue lifecycle code |

## 검증 요구사항

기능 test는 다음 결과를 확인한다.

- Payload가 없는 single-part와 multipart frame도 유한 HWM을 소비한다.
- Incremental multipart는 allocation 전에 HWM에서 멈추고 rollback 뒤 예약값이 남지 않는다.
- 비어 있는 queue의 complete oversize message는 한 건만 받아들인다.
- 일반 dequeue는 writer에 credit을 반환하고 retained dequeue는 lease가 끝날 때 반환한다.
- Lease를 보유한 채 detach해도 새 generation에 credit을 반환하지 않는다.
- HWM 감소는 기존 frame을 유지하고 queue가 줄어든 뒤 적용된다.
- Completion과 monitor queue는 application HWM 계산과 수락 결과를 바꾸지 않는다.
- Snapshot 조회와 metrics reset은 같은 message sequence의 수락 결과를 바꾸지 않는다.
