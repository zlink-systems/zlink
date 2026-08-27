---
title: "Spot timer"
---

# Spot timer

[Spot·Actor 주제 목차](README.ko.md) · [스펙 목차](../README.ko.md) · [이전: 09. Object lifecycle](09-object-lifecycle.ko.md)

> 이 문서는 [Spot](../00-foundation/02-glossary.ko.md#spot)이 반복 또는 지연 실행을 등록하는 timer의
> 계약과, 등록한 timer 수가 늘어도 자원이 그에 비례해 늘지 않는 구현을 정의한다. Timer
> callback은 같은 Spot의 다른 [application 작업](../00-foundation/02-glossary.ko.md#spot-turn)과 같은
> [execution gate](../01-execution/02-handler-turn-and-execution-gate.ko.md)를 거쳐 실행되므로, 언제
> callback이 실행되고 밀렸을 때 무엇을 받는지를 여기서 정의하고 gate 자체의 규칙은
> [02. Handler turn과 execution gate](../01-execution/02-handler-turn-and-execution-gate.ko.md)를 가리킨다.

## 1. Timer generation과 cancel

Spot timer는 network record와 같은 Spot application turn에서 callback을 실행한다. 각 언어
service runtime은 platform timer의 만료를 Spot queue record로 바꾸며, backend와 관계없이
아래 의미를 유지한다.

**같은 timer key를 다시 등록하면 generation이 증가한다.** 이미 queue에 들어가 있던 이전
generation의 record는 callback을 실행하지 않는다 — 재등록 전에 예약된 tick이 재등록 뒤에
실행되면 caller가 기대한 새 주기·새 callback과 다른 것이 실행되기 때문이다.

**Cancel은 해당 generation 이후 callback의 시작만 막는다.** 이미 시작한 callback은 강제로
중단하지 않는다 — cancel 시점에 이미 실행 중인 handler를 강제 종료하면 handler가 다루던
상태가 일관되지 않은 채로 남을 수 있다.

**반복 timer가 handler 실행보다 빠르게 만료돼도 같은 key의 callback을 동시에 실행하지
않는다.** 중복 만료는 하나의 pending record로 합칠 수 있다 — 동시 실행을 허용하면 같은
timer의 두 callback이 같은 상태를 동시에 바꿀 수 있기 때문이다.

| 상황 | 동작 |
|---|---|
| 같은 key 재등록 | generation 증가 |
| 이전 generation의 queue record | callback 실행 안 함 |
| cancel | 해당 generation 이후 callback 시작 차단(이미 시작한 callback은 중단하지 않음) |
| 반복 timer가 handler보다 빠르게 만료 | 같은 key의 callback을 동시 실행하지 않음, 중복 만료를 pending record 1개로 병합 가능 |

Callback은 다음 tick 정보를 받는다.

```text
DeliveryIndex  // 이 timer generation에서 실제로 시작한 callback의 1부터 시작하는 연속 번호
ScheduledIndex // 최초 nominal due time을 1로 하는, 이번 callback이 대표하는 nominal tick 번호
SkippedTicks   // 이전에 전달한 ScheduledIndex와 이번 값 사이에서 callback을 만들지 않은 nominal tick 수
```

`DeliveryIndex`는 callback마다 정확히 1 증가한다. `ScheduledIndex`는 감소하지 않고
`ScheduledIndex >= DeliveryIndex`를 유지한다. `SkippedTicks`는 첫 callback에서는
`ScheduledIndex - 1`이고, 이후에는 `현재 ScheduledIndex - 이전 ScheduledIndex - 1`이다.
Scheduler의 wall-clock 오차나 나노초 단위의 정밀한 시각은 public 결과가 아니다 — 이 세 필드만이
caller가 관찰하는 timing 계약이다.

## 2. Overrun policy

반복 timer는 handler가 다음 tick 전에 끝나지 못했을 때 지나간 tick을 어떻게 다룰지 정하는
세 overrun policy 중 하나를 사용한다. 기본값은 `SkipLateTicks`다.

| Policy | Handler가 늦게 끝났을 때 다음 callback |
|---|---|
| `SkipLateTicks`(기본) | 이미 지난 nominal tick을 건너뛰고 관찰 시점의 최신 due tick 하나만 전달한다. |
| `CatchUpBounded` | 지나간 nominal tick을 순서대로 전달하되 한 catch-up 구간에서 최대 `MaxCatchUpTicks`개만 전달하고, 더 오래된 tick은 건너뛴다. |
| `DelayNextTick` | Handler terminal 뒤 period를 다시 계산해 다음 tick을 예약하며, missed tick을 catch-up하지 않는다. |

`MaxCatchUpTicks`의 기본값은 `1`이다. `CatchUpBounded`에서는 `1..INT_MAX` 범위여야 하며,
다른 policy에서는 이 값이 동작에 영향을 주지 않는다. Relocation encoding은 무시되는 값을
그대로 public 의미로 만들지 않고, 유효한 기본값으로 맞출 수 있다.

## 3. Owner lease와 admission

Spot timer는 service runtime이 current [owner lease](../00-foundation/02-glossary.ko.md#owner-lease)와
admission deadline을 확인한 뒤에만 admission할 수 있다.

Lease 갱신이 멈춰 monotonic
deadline을 넘으면, Framework process가 일시 정지된 상태였더라도 재개 후 새 tick을 queue에
넣거나 callback을 시작하지 않는다.

이전 object·owner authority의 pending tick도 실행하지
않는다 — timer가 owner lease 밖에서 계속 돌면 이미 다른 owner로 넘어간 object의 상태를
옛 owner의 callback이 건드릴 수 있기 때문이다.

## 4. 공유 scheduler — 자원은 등록 수에 비례하지 않는다

Spot마다 timer를 여러 개 등록할 수 있으므로 timer 수는 Spot 수보다 빠르게 늘어난다. Spot
10,000개에 timer를 두 개씩 등록하면 timer가 20,000개다.

**Timer는 공유 scheduler 하나가 관리하며, 등록마다 전용 자원을 만들지 않는다.** 등록마다
자원을 만드는 방식과 하나의 scheduler가 모두 관리하는 방식은 필요한 자원 수가 다르다.

| 방식 | 1만 Spot × timer 2개일 때 필요한 자원 |
|---|---|
| 등록마다 전용 자원(OS timer, 대기 루프, 지연 호출) | 그 자원이 **2만 개** |
| 공유 scheduler + 마감 시각 우선순위 대기열 | 스레드 하나와 대기열 항목 2만 개 |

공유 scheduler 방식이 모든 runtime의 공통 구조다. Queue 항목 수는 같지만 scheduler와 core
thread는 하나만 필요하다. Scheduler는 모든 Spot의 timer를 마감 시각 우선순위 queue에서
관리한다.

*내부 확인 조건 — 등록한 timer 수가 늘어도 scheduler 자원(전용 thread·OS timer 개수)이
그에 비례해 늘지 않는다는 것은 내부 자원 계수로만 확인한다. Timer 등록·callback 실행이라는
공개 표면만으로는 관찰할 수 없다.*

## 5. 늦은 tick 처리의 내부 구현

주기를 넘겨 늦게 실행될 때 지나간 tick을 어떻게 할지는 §2가 정의하는 공개 option이다.
구현은 세 policy 중 하나를 골라 고정하지 않는다 — 특히 `DelayNextTick`은 "다음 예약을
처리 완료 뒤에 한다"는 세 policy 중 하나(고정 지연)일 뿐이며, 고정 주기를 없애는 규칙이
아니다.

**기본 동작(`SkipLateTicks`)은 밀린 tick을 하나로 합치는 것이다.** §1의 "중복 만료를 한
번의 pending record로 합칠 수 있다"는 규칙을 scheduler가 그대로 구현한다. Application이
`CatchUpBounded`를 골랐다면 그 option이 정한 개수까지가 상한이며, 구현이 그 개수를
임의로 하나로 줄이지 않는다.

**Tick 통계를 timer 수명 동안 무한정 쌓지 않는다.** 전달한 tick과 실패 기록을 계속
누적하면 오래 실행되는 timer가 사용하는 메모리가 계속 증가한다.

*내부 확인 조건 — 오래 실행되는 timer가 tick 통계로 메모리를 계속 늘리지 않는다는 것은
내부 메모리 사용량 측정으로만 확인한다. 공개 표면(tick 정보 필드)만으로는 관찰할 수 없다.*

## 6. Tick이 실행 권한으로 들어가는 경로

Timer callback은 그 Spot의 실행 권한을 거쳐 실행된다. `SpotWide`에서는 공유 권한을,
`PerActor`에서는 timer 이름별 권한을 쓴다. Timer가 자기 권한을 얻지 못하면 그 tick은
보관 자리에 남았다가 다음에 다시 시도한다. 실행 권한을 획득·반납하는 규칙 자체는
[02. Handler turn과 execution gate](../01-execution/02-handler-turn-and-execution-gate.ko.md)가 소유한다.

## 7. 고빈도 timer의 batch 처리

고빈도 timer도 관리형 언어에서 native callback 경계를 매 tick마다 왕복하지 않는다.
Platform timer가 Framework scheduler에 wakeup 신호를 보내면, scheduler가 만료된 record를
batch로 처리한다.

## 8. 검증 요구

공개 표면(timer 등록·재등록·cancel 호출, callback에 전달되는 `DeliveryIndex`·
`ScheduledIndex`·`SkippedTicks`, overrun policy 설정과 그 결과로 전달되는 tick 수)만으로
다음을 확인한다. 각 항목은 contract test 하나로 이어진다.

**Generation과 cancel**

- 같은 key를 다시 등록하면 이전 generation의 이미 대기 중인 tick은 callback을 실행하지
  않는다.
- Cancel 뒤에는 그 generation 이후의 tick이 callback을 시작하지 않지만, cancel 시점에 이미
  실행 중이던 callback은 끝까지 실행된다.
- 반복 주기가 handler 실행 시간보다 짧아도 같은 key의 callback이 동시에 두 번 실행되지
  않는다.

**Tick 정보**

- `DeliveryIndex`는 callback마다 정확히 1씩 증가한다.
- `ScheduledIndex`는 감소하지 않고 항상 `DeliveryIndex` 이상이다.
- `SkippedTicks`는 첫 callback에서 `ScheduledIndex - 1`, 이후에는 `현재 ScheduledIndex -
  이전 ScheduledIndex - 1`과 같다.

**Overrun policy**

- `SkipLateTicks`로 등록한 timer가 밀리면 다음 callback은 밀린 tick을 건너뛰고 최신 tick
  하나만 받는다(`SkippedTicks`가 건너뛴 수를 반영한다).
- `CatchUpBounded`로 등록한 timer가 밀리면 `MaxCatchUpTicks`개까지만 순서대로 tick을 받고,
  그 이상은 건너뛴다.
- `DelayNextTick`으로 등록한 timer는 handler 종료 시점을 기준으로 다음 tick 주기가
  다시 계산된다.

**Owner lease**

- Owner lease가 만료된 뒤에는 새 tick이 callback을 시작하지 않고, lease가 갱신되지 않는
  동안 process가 일시 정지됐다 재개해도 밀린 tick이 한꺼번에 실행되지 않는다.

---

[Spot·Actor 주제 목차](README.ko.md) · [스펙 목차](../README.ko.md) · [이전: 09. Object lifecycle](09-object-lifecycle.ko.md)
