---
title: "직렬 실행기 계층"
---

# 직렬 실행기 계층

[Execution 주제 목차](README.ko.md) · [스펙 목차](../README.ko.md) · [이전: 06. 상태 소유와 state lane](06-state-ownership-and-lanes.ko.md)

> 이 문서는 Spot·Actor·STREAM session이 각자의 작업을 어떤 직렬 실행 단위에 올리는지,
> 누가 그 단위의 수명을 소유하는지, 그리고 그 단위에 작업을 넣는 진입점이 무엇인지
> 정의한다. 여기서 정하는 이름과 진입점은 네 언어 runtime이 표기만 바꿔 그대로 쓴다.
> 어떤 실행 mode에서 무엇이 동시에 실행될 수 있는지는 용어집이 소유하며, 이 문서는 그
> mode에서 작업이 어느 queue를 지나는지를 정의한다.

## 1. 직렬 실행기 개요

Application이 Spot handler·Actor handler·timer callback·session callback을 등록하면,
runtime은 그 callback을 아무 thread에서나 실행하지 않고 **직렬 실행 단위** 하나에 줄을
세워 한 번에 하나씩 실행한다. 이 문서는 그 줄이 계층마다 몇 개이고 누가 그것을 만들고
없애는지, 그리고 작업을 어느 줄에 넣을지 누가 정하는지를 규정한다.

| 주체 | 이 문서에서 정하는 것 |
|---|---|
| Application | 실행 mode를 등록 시점에 고른다. 자기 작업이 어느 queue로 갈지는 고르지 않는다 |
| Runtime | 진입점마다 어느 queue를 쓸지 정하고, 그 queue의 수명을 소유한다 |

[상태 소유와 state lane](06-state-ownership-and-lanes.ko.md)과 이 문서는 다른 문제를
다룬다. state lane은 컴포넌트 하나가 **자기 mutable 상태**를 한 번에 한 turn만 만지게 하는
수단이고, 이 문서의 직렬 실행기는 **application 작업**을 순서대로 돌리는 수단이다. 한
runtime 안에 state lane은 상태를 소유하는 컴포넌트마다 하나씩 있고, 직렬 실행기는
Spot·Actor·session 인스턴스마다 하나씩 있다.

실행 mode에 따라 무엇을 동시에 실행할 수 있는지는
[User Spot execution mode](../00-foundation/02-glossary.ko.md#user-spot-execution-mode)가
소유한다 — Spot handler·member Actor handler·timer callback이 어느 execution gate를 공유할지
정하는 등록 옵션이다. 이 문서는 그 mode에서 한 작업이 지나는 queue 경로만 정한다.

## 2. 계층과 소유

세 계층이 각각 직렬 실행 단위를 갖는다. **각 계층에는 조율자가 하나 있고, 그 조율자가
자기 계층의 queue와 자신이 만든 하위 queue의 수명을 함께 소유한다.** 조율자가 없으면
어느 작업이 어느 queue에서 도는지가 호출 지점마다 흩어져, 순서 보장을 코드에서 읽을 수
없게 된다.

```text
ZLinkSpotSerialExecutor          ← Spot 하나마다 조율자 하나
 ├── Spot queue                     하나
 ├── Actor queue                    Actor마다 하나    ─┐ 이 Spot이 사라질 때
 └── timer queue                    timer 이름마다 하나 ─┘ 함께 사라진다
                                    ↑ 이 둘은 `PerActor`에서만 만든다(§4)

ZLinkActorSerialExecutor         ← Actor 하나마다 조율자 하나
 └── Actor queue                    하나              ← 맵이 없다

ZLinkSessionSerialExecutor       ← session 하나마다 조율자 하나
 └── session queue                  하나              ← 맵이 없다
```

```csharp
// contract pseudocode이며 실제 API가 아니다 — 실제 시그니처는 언어별 interface가 소유한다.
class ZLinkSpotSerialExecutor            // Spot 하나마다 하나
{
    ZLinkUserSpotExecutionMode executionMode;   // 등록 때 고정된다
    ZLinkSerialExecutionQueue  spotQueue;
    ZLinkStateLane             lane;            // 아래 두 map을 소유한다
    Map<ZLinkActorId,   ZLinkSerialExecutionQueue> actorQueues;
    Map<ZLinkTimerName, ZLinkSerialExecutionQueue> timerQueues;
}

class ZLinkActorSerialExecutor           // Actor 하나마다 하나
{
    ZLinkSerialExecutionQueue queue;     // 단수 — 하위 소유 대상이 없어 map이 없다
    ZLinkStateLane            lane;
}
```

- **하위 queue를 갖는 계층은 Spot뿐이다.** Actor queue와 timer queue의 수명이 Spot에
  묶여 있기 때문이다 — 그 Spot이 사라지면 그 Spot의 Actor queue와 timer queue도 함께
  사라진다. Actor와 session에는 수명이 자신에게 묶인 하위 대상이 없으므로 queue를 이름으로
  찾는 map을 두지 않고, 인스턴스마다 queue 하나만 갖는다.
- **queue map은 [state lane](06-state-ownership-and-lanes.ko.md)이 소유한다.** 이 map은
  이름으로 queue를 찾기만 하는 collection이므로 06 §4의 C1에 해당하고, lock으로 지키지
  않는다.

**내부 확인 조건** — 조율자 밖에서 Actor queue나 timer queue를 field로 들고 있는 코드가
없다. Actor 조율자와 session 조율자에는 queue map field가 없다.

## 3. 진입점

**호출자는 자기 작업이 어느 queue로 가는지 모른다.** 조율자가 정한다. 호출자가 queue를
직접 고르는 인자나 표면을 두지 않는다 — 고를 수 있게 하면 §4의 경로 규칙이 호출 지점마다
달라져 실행 mode의 보장이 깨진다.

| 조율자 | 진입점 | 무엇을 제출하는가 | 어느 queue에서 도는가 |
|---|---|---|---|
| Spot | `executeSpot` | Spot handler 작업 | Spot queue |
| Spot | `executeActor(actorId)` | 그 Actor의 handler 작업 | §4의 경로 |
| Spot | `executeTimer(timerName)` | 그 이름의 timer callback | §4의 경로 |
| Spot | `executeLifecycle` | join·leave·relocation 같은 수명 제어 | Spot queue의 lifecycle lane |
| Actor | `executeActor` | 그 Actor의 handler 작업 | 자기 queue |
| Actor | `executeLifecycle` | 그 Actor의 수명 제어 | 자기 queue의 lifecycle lane |
| STREAM session | `executeApplication` | session callback에 전달할 packet | 자기 queue |
| STREAM session | `executeControl` | session 제어 명령 | 자기 queue |
| STREAM session | `executeInfrastructure` | 연결 상태 갱신 같은 하부 작업 | 자기 queue |
| STREAM session | `executeFinal` | 종료 직전 마지막 작업 | 자기 queue |

동사는 세 계층 모두 `execute`다. 계층마다 `enqueue`와 `execute`가 갈리면 같은 뜻의 호출을
계층을 옮길 때마다 다시 찾아야 한다.

## 4. Spot 실행 mode와 queue 경로

[User Spot execution mode](../00-foundation/02-glossary.ko.md#user-spot-execution-mode)가
정하는 것은 **queue를 몇 개 만드는가**다. 어느 mode에서도 작업 하나가 queue 두 개를 지나지
않는다.

진입점이 어느 queue에 연결되는지를 mode별로 그리면 다음과 같다. Actor 둘(A·B)과 timer
둘(`tick`·`beat`)이 있는 Spot을 예로 든다.

**`PerActor`** — 진입점마다 자기 queue가 따로 있다.

```text
  executeSpot ──────────┐
                        ├──▶ [  Spot queue   ] ──▶ 실행
  executeLifecycle ─────┘

  executeActor(A) ─────────▶ [ Actor A queue ] ──▶ 실행   ┐
  executeActor(B) ─────────▶ [ Actor B queue ] ──▶ 실행   │ 다섯 queue가 서로 독립이다.
  executeTimer("tick") ────▶ [  tick queue   ] ──▶ 실행   │ 동시에 다섯까지 진행된다.
  executeTimer("beat") ────▶ [  beat queue   ] ──▶ 실행   ┘
```

**`SpotWide`** — queue가 하나뿐이고 모든 진입점이 거기로 간다.

```text
  executeSpot ──────────┐
  executeLifecycle ─────┤
  executeActor(A) ──────┤
  executeActor(B) ──────┼──▶ [  Spot queue   ] ──▶ 실행
  executeTimer("tick") ─┤
  executeTimer("beat") ─┘

  ← Actor queue도 timer queue도 만들지 않는다.
```

- **`SpotWide`에서는 Actor queue를 만들지 않는다.** 실행 순서는 Spot queue 하나로 이미
  끝나고, 유입 제한은 이 계층의 권한이 아니며(§5), 그 mode에서는 Spot 전체가 한 줄이라
  Actor를 따로 세워도 어느 Actor가 더 빨리 돌지 않는다. 남는 것이 없는데 queue를 하나 더
  거치면, 그 겹침이 만드는 문제만 늘어난다 — Spot turn 안에서 Actor 작업을 제출하면 자기가
  쥔 turn을 기다리게 되므로 그것을 피하는 장치가 따로 필요해진다.
- **`SpotWide`에서는 timer queue도 만들지 않는다.** 같은 이유다.
- **`PerActor`에서만 Actor별·timer별 queue를 만든다.** 그 mode에서는 서로 다른 Actor와 서로
  다른 timer가 실제로 겹쳐 진행되므로 각자 줄이 필요하다.

**내부 확인 조건** — 한 작업이 queue 둘에 연달아 제출되는 자리가 없다. `SpotWide`로 등록된
Spot의 조율자에서 Actor queue map과 timer queue map이 비어 있다.

진입점 하나가 이 경로 판정을 전부 안고 있고, 호출자에게는 queue가 보이지 않는다.

```csharp
// contract pseudocode이며 실제 API가 아니다 — 실제 시그니처는 언어별 interface가 소유한다.
ExecuteActor(actorId, work)
{
    if (executionMode == SpotWide)
    {
        spotQueue.Enqueue(work);         // Actor queue를 만들지 않는다
        return;
    }

    // queue map은 state lane이 소유한다(§2). lock을 잡지 않는다.
    actorQueue = lane.Run(() => GetOrCreateActorQueue(actorId));
    actorQueue.Enqueue(work);            // §5의 건수 상한
}

ExecuteTimer(timerName, work)
{
    if (executionMode == SpotWide)
    {
        spotQueue.Enqueue(work);         // timer queue를 만들지 않는다
        return;
    }

    timerQueue = lane.Run(() => GetOrCreateTimerQueue(timerName));
    timerQueue.Enqueue(work);
}
```

## 5. 이 계층의 한도는 건수뿐이다

**유입 속도를 정하는 권한은 이 문서에 없다.** Ordinary ingress가 permit을 얻고 record를
receive하는 순서는
[Application job queue와 backpressure 「1. 두 독립된 capacity authority」](04-application-job-queue-and-backpressure.ko.md#1-두-독립된-capacity-authority)와
[「3. Ordinary ingress permit 순서」](04-application-job-queue-and-backpressure.ko.md#3-ordinary-ingress-permit-순서)가
소유한다. 여기서 다시 정의하지 않는다.

**queue는 payload 바이트를 세지 않는다.** Byte로 재는 것은 Core byte HWM과 소켓 수신 회전
한도뿐이고, Framework 쪽 한도는 모두 건수다(04 §9). owner별로 payload 바이트를 따로 세면
Core HWM을 다른 이름으로 다시 구현하는 것이 되고, 그렇게 세어도 owner 수만큼 곱해지므로
process memory를 묶지도 못한다.

queue가 갖는 건수 상한은 [04 §8](04-application-job-queue-and-backpressure.ko.md#8-backpressure-3단계와-한도-종류)의
owner FIFO가 소유한다. 그 상한이 걸리는 단위는 §4가 만든 queue를 따라간다 — `PerActor`는
Actor마다, `SpotWide`는 Spot 하나다.

Application job queue permit은 작업이 이 queue에 줄 서 있는 동안에도 계속 잡혀 있다(04 §3의
5단계). 따라서 모든 queue에 쌓인 총 건수는 이미 permit 수로 묶여 있고, 여기의 건수 상한은
그 위에 얹는 owner별 분배 장치다.

## 6. 직렬 queue primitive

`ZLinkSerialExecutionQueue`는 작업을 순서대로 실행하는 것만이 아니라, **수용량을 넘겼을 때
거절하는 것과 한 소유자가 오래 점유하지 못하게 하는 것까지 자기 계약으로 갖는다.** 이
책임을 호출자에게 남기면 호출 지점마다 다르게 처리되고, 그러면 어떤 부하에서도 지연 상한이
있다는 실시간 보장을 세울 수 없다.

### 6.1 정책

다음 값은 정책 객체 `ZLinkExecutionLanePolicy`로 주입받는다. queue 안에 상수로 박지
않는다 — Spot·Actor·session이 서로 다른 값을 쓰기 때문이다.

이 값들은 §5의 건수 상한을 정하는 것이지 유입 속도를 제한하는 값이 아니다. Ordinary
ingress의 admission은 [04](04-application-job-queue-and-backpressure.ko.md)가 소유한다.
payload 바이트를 재는 값은 두지 않는다(§5).

다음은 의미를 설명하는 contract pseudocode이며 실제 API가 아니다. 정확한 signature는 언어별
exact interface가 정의한다.

```text
ZLinkExecutionLanePolicy {
    applicationMessageCapacity   // application lane이 동시에 담는 작업 수 상한 (건, > 0)
    lifecycleMessageCapacity     // lifecycle lane이 동시에 담는 작업 수 상한 (건, > 0)
    lifecycleBurstLimit          // lifecycle 작업이 application 작업을 연속으로 앞지를 수 있는
                                 //   최대 건수 (건, > 0). 이 수를 넘기면 application 작업이
                                 //   한 건 실행된다
    ownerTimeBudget              // 한 소유자가 연속으로 turn을 점유할 수 있는 시간
                                 //   (밀리초, > 0)
}
```

### 6.2 진입점

```text
enqueue(작업)                    // application lane. 한 건으로 센다
enqueueLifecycle(작업)           // lifecycle lane. 대기 중인 application 작업을 앞지른다
enqueueBarrierNext(작업)         // 현재 turn 직후, 줄 서 있는 application 작업보다 먼저
isCurrent()                      // 호출한 thread가 이 queue의 turn을 점유하고 있는가
awaitQuiescence()                // 줄 선 작업이 모두 끝날 때까지 기다린다
close()                          // 새 제출을 받지 않고 이미 받은 작업을 끝낸다
```

### 6.3 자리 판정과 순서 발급의 원자적 범위

자리 판정·순서 번호 발급·queue 삽입 셋은 **전부 일어나거나 전혀 일어나지 않는다.**
호출자 하나의 제출에서 이 셋이 쪼개지지 않는다.

세 동작을 각각 별개로 처리하는 동시성 queue 자료구조로 치환하지 않는다. 쪼개면 두 호출자가
같은 여유를 보고 함께 판정을 통과한 뒤 둘 다 삽입해 상한을 넘기거나, 번호를 먼저 받은
작업이 나중에 삽입돼 순서가 뒤집힌다. 이 셋은 함께 움직여야 하는 값이므로
[06 §4의 C2](06-state-ownership-and-lanes.ko.md#4-상태-분류와-판별-기준)에 해당한다.

### 6.4 공정성

현재 소유자가 turn을 `ownerTimeBudget`보다 오래 점유하면, 그 소유자의 남은 작업을 계속
실행하지 않고 다음 소유자에게 넘긴다. 이 장치가 없으면 작업을 많이 쌓아 둔 소유자 하나가
queue를 계속 차지할 수 있고, 그러면 같은 queue에 걸린 다른 작업이 언제 시작되는지 상한을
말할 수 없다.

### 6.5 turn을 구동하는 loop

queue에 줄 선 작업을 실제로 실행하는 것은 **한 번에 하나만 들어가는 배출 loop**다. 이 loop가
작업 하나를 꺼내 turn 위에서 돌리고, 끝나면 다음 작업으로 넘어간다. §6.4의 양보는 이 loop가
slice를 끊는 것으로 구현된다.

```csharp
// contract pseudocode이며 실제 API가 아니다 — 실제 시그니처는 언어별 interface가 소유한다.
Drain()
{
    // 이 gate가 "한 번에 하나"를 보장한다. 이미 다른 호출이 돌고 있으면 그냥 돌아간다.
    if (!TryEnterDrain()) return;

    sliceStartedAt = Now();
    while (TryTakeNext(out work))      // lifecycle을 먼저 고르되 lifecycleBurstLimit을 지킨다(§6.1)
    {
        turn   = new Turn();
        result = RunOnTurn(work, turn);          // §6.6

        if (result == Completed) Release(work);
        // Suspended면 작업이 turn을 반납한 것이다. 완료는 나중에 처리하고
        // 이 loop는 바로 다음 작업으로 넘어간다.

        if (Now() - sliceStartedAt >= policy.ownerTimeBudget)
            break;                     // §6.4 — 여기서 소유자가 양보한다
    }
    ExitDrain();

    // 끊고 나온 남은 작업은 새 slice에서 이어 돈다.
    if (HasQueuedWork()) ScheduleDrain();
}
```

### 6.6 작업 하나를 구동하는 방법과 turn 반납

작업 하나는 첫 대기 지점에서 멈췄다가 이어서 도는 실행 단위다 — C#에서는 `async` 메서드를
컴파일러가 그런 상태 기계로 만든다. **배출 loop는 그 상태 기계를 시작만 하고, 끝날 때까지
기다릴지 중간에 turn을 돌려받을지를 판정한다.**

turn을 반납할 수 있어야 하는 이유는 외부 호출 때문이다. 작업이 원격 응답을 기다리는 동안
turn을 쥐고 있으면 그 queue 전체가 그 시간만큼 멈춘다. 무엇을 기다릴 때 반납하고 무엇을
기다릴 때 유지하는지는
[Handler turn과 execution gate 「3. `Yield` 시 gate와 claim」](02-handler-turn-and-execution-gate.ko.md#3-yield-시-gate와-claim)과
[Async와 Yield](../00-foundation/02-glossary.ko.md#async-yield)가 소유한다. 여기서는 그 반납을
어떻게 구동하는지만 보인다.

```csharp
// contract pseudocode이며 실제 API가 아니다 — 실제 시그니처는 언어별 interface가 소유한다.
// 배출 loop 쪽 — 작업을 시작하고 두 결말 중 하나를 기다린다.
RunOnTurn(work, turn)
{
    Turn.Current = turn;                   // 실행 중 "지금 이 turn"을 심는다
    operation = work();                    // 상태 기계 시작 — 첫 대기 지점까지 동기 실행

    if (operation.IsCompleted) return Completed;   // 한 번도 대기하지 않고 끝났다

    // 먼저 오는 쪽이 결말을 정한다.
    //   operation    — 작업이 끝났다
    //   turn.Yielded — 작업이 외부 호출을 기다리며 turn을 반납했다
    if (WaitAny(operation, turn.Yielded) == turn.Yielded)
        return Suspended;                  // loop는 다음 작업으로 넘어간다

    Await(operation);
    return Completed;
}
```

반납하는 쪽은 작업 안에서 외부 호출을 감싸는 자리다.

```csharp
// contract pseudocode이며 실제 API가 아니다 — 실제 시그니처는 언어별 interface가 소유한다.
// 작업 쪽 — 외부 호출을 감싸는 자리.
YieldFrameworkCall(submit)
{
    operation = submit();
    if (operation.IsCompleted)
        return operation.Result;      // 기다리지 않았으므로 반납할 이유가 없다

    turn.SignalYielded();             // → 배출 loop가 다음 작업으로 넘어간다
    result = Await(operation);

    // 결과가 왔다고 바로 이어서 실행하지 않는다. 다시 줄을 서서 turn을 받아야
    // 그 queue가 여전히 "한 번에 하나"로 남는다.
    AwaitResumePermit();
    return result;
}
```

**반납한 turn은 다시 받아야 이어서 실행한다.** 결과가 도착했다고 그 자리에서 이어 실행하면
그 순간 그 queue에서 두 작업이 함께 돌아 §6의 직렬 보장이 깨진다.

**내부 확인 조건** — 반납 지점의 재개 경로가 queue 제출을 거치지 않고 바로 이어지는 자리가
없다.

## 7. 상태 조회의 turn 경계

한 메시지를 처리하는 동안 필요한 상태 값은 **state lane의 한 turn에서 함께 읽어 immutable
snapshot으로 들고 다닌다.** 조회마다 별도 turn을 만들지 않는다.

```csharp
// contract pseudocode이며 실제 API가 아니다 — 실제 시그니처는 언어별 interface가 소유한다.
snapshot = lane.Run(() => new ActorStateSnapshot(
    registry.Actor(actorId),        // 셋을 한 turn에서 함께 읽는다.
    registry.Context(actor),        // 따로 읽으면 그 사이에 Actor가 사라질 수 있다.
    registry.ActorType(actorId)));
```

조회를 각각 별개 turn으로 쪼개면 두 가지가 함께 나빠진다. 값들이 서로 다른 시점의 것이
되어 한 메시지 처리 안에서 옛 값과 새 값이 섞이고, 메시지 한 건이 turn 완료를 여러 번
기다리게 된다.

같은 값을 한 처리 경로에서 두 번 이상 읽지 않는다. 첫 조회 결과를 그대로 들고 다닌다.

snapshot 타입 이름은 `<대상>StateSnapshot`으로 한다.

**내부 확인 조건** — 한 처리 경로에서 같은 registry 값을 두 번 조회하는 자리가 없다.

## 8. 소유 전제를 검사한다

상위 직렬 실행 단위가 "여기는 이미 직렬이다"를 보장하는 자리에서도 **그 전제를 검사 없이
믿지 않는다.** 전제가 깨졌을 때 검사하는 주체가 없으면 예외가 아니라 데드락으로 나타나고,
그때는 어느 호출이 전제를 깼는지 코드에서 찾기 어렵다.

| 위치 | 확인 |
|---|---|
| 컴포넌트 진입 경계 | `isOnLane` — 지금 이 lane 위에서 실행 중인가 |
| 재진입이 가능한 지점 | `throwIfReentrant` — 이미 점유한 turn에 다시 들어오는가 |

디버그 빌드에서는 필수다. 릴리스 빌드에서는 선택이되, **재진입이 실제로 일어났던
컴포넌트** — 재귀 잠금을 쓰던 곳 — 는 릴리스에서도 유지한다.

**내부 확인 조건** — 상위 직렬 소유를 전제하는 자리마다 `isOnLane` 또는
`throwIfReentrant` 호출이 있다.

## 9. 언어별 매핑

이름은 §2·§3·§6에서 정한 것을 쓰고, 표기만 언어 관용구로 바꾼다.

| 언어 | 타입 | 메서드 | 필드 |
|---|---|---|---|
| .NET | `ZLinkSpotSerialExecutor` | `PascalCase`, 비동기는 `Async` 접미 | `_camelCase` |
| java | `ZLinkSpotSerialExecutor` | `camelCase` | `camelCase` |
| cpp | `spot_serial_executor_t` | `snake_case` | `_snake_case` |
| node | `ZLinkSpotSerialExecutor` | `camelCase` | `camelCase` |

의미를 바꾸는 개명은 하지 않는다. `executeActor`를 cpp에서 `execute_actor`로 쓰는 것은
표기 변환이지만, `dispatch_actor`로 쓰는 것은 다른 이름을 짓는 것이다.

`SpotWide`에서 Actor queue를 만들지 않는 §4의 규칙은 node runtime이 이미 그렇게 동작한다.
나머지 세 언어는 현재 `SpotWide`에서도 Actor queue를 거치므로 §4에 맞춰야 한다.

## 10. 검증 요구

공개 표면(§3의 진입점 호출과 그 반환값, backpressure 거절, handler·callback이 실행된 순서와
시각, 재진입 호출이 받는 예외)만으로 다음을 확인한다. 각 항목은 test 하나로 이어진다.

**진입점**

- 네 언어의 Spot 조율자에 `executeSpot`·`executeActor`·`executeTimer`·`executeLifecycle`이
  있고, 어느 진입점에도 호출자가 queue를 지정하는 인자가 없다.
- Actor 조율자와 session 조율자의 진입점은 §3의 표와 같고, 그 밖에 작업을 넣는 공개
  표면이 없다.

**실행 mode별 동시 실행과 순서**

- `PerActor`로 등록한 User Spot에서 서로 다른 Actor 둘에 오래 걸리는 작업을 제출하면 두
  handler가 겹쳐 실행된다.
- `SpotWide`로 등록한 User Spot에서 같은 제출을 하면 앞 handler가 끝난 뒤에 다음 handler가
  시작된다.
- 같은 Actor에 연달아 제출한 작업은 두 mode 모두 제출 순서대로 실행된다.
- `PerActor`에서 서로 다른 timer 이름의 callback은 겹쳐 실행되고, 같은 timer 이름의
  callback은 제출 순서대로 실행된다.

**수용량과 backpressure**

- `PerActor`에서 Actor 하나에 작업을 몰아 `applicationMessageCapacity`를 채우면 그 Actor에
  대한 제출만 거절되고, 같은 Spot의 다른 Actor에 대한 제출은 계속 수락된다.
- `SpotWide`에서는 같은 상한이 Spot 단위로 걸린다(§5).
- 같은 건수를 제출하면 payload 크기와 무관하게 같은 지점에서 거절이 시작된다 — 이 계층은
  payload 바이트를 세지 않는다(§5).
- owner queue 포화로 인한 거절이 조용히 사라지지 않는다. 그 거절과 permit 대기를 구분해
  관찰하는 계약은 [04 §10](04-application-job-queue-and-backpressure.ko.md#10-검증-요구)이
  소유한다.
- `enqueueLifecycle`로 제출한 작업은 이미 줄 서 있는 application 작업보다 먼저 실행되고,
  연속으로 앞지르는 건수는 `lifecycleBurstLimit`에서 멈춰 application 작업이 한 건 실행된다.

**공정성**

- 작업을 많이 쌓아 둔 소유자가 `ownerTimeBudget`을 넘겨 점유하면, 그 뒤에 작업을 하나만
  제출한 다른 소유자의 작업이 먼저 쌓인 소유자의 남은 작업보다 먼저 시작된다.

**상태 조회의 시점 일치**

- 메시지 한 건을 처리하는 도중 그 Actor의 등록 정보를 바꿔도, 그 한 건의 처리 안에서
  바뀌기 전 값과 바뀐 뒤 값이 섞여 관측되지 않는다.

**재진입**

- 조율자가 실행 중인 작업 안에서 같은 조율자의 `execute*` 완료를 동기적으로 기다리면,
  멈추지 않고 그 호출 지점에서 즉시 예외가 관측된다.

**언어 간 동등성**

- 위 항목이 .NET·java·cpp·node에서 같은 결과를 낸다. node에서 "겹쳐 실행된다"는 async
  handler 둘이 `await`를 사이에 두고 번갈아 진행되는 것으로 관찰한다.

---

[Execution 주제 목차](README.ko.md) · [스펙 목차](../README.ko.md) · [이전: 06. 상태 소유와 state lane](06-state-ownership-and-lanes.ko.md)
