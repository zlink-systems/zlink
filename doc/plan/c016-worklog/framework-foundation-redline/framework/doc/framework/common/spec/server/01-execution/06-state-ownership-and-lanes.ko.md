---
title: "상태 소유와 state lane"
---

# 상태 소유와 state lane

[Execution 주제 목차](README.ko.md) · [스펙 목차](../README.ko.md) · [이전: 05. Payload 소유권과 codec](05-payload-ownership-and-codec.ko.md) · [다음: 07. 직렬 실행기 계층](07-serial-executor-layers.ko.md)

> 이 문서는 컴포넌트가 자신의 mutable 상태를 어떤 메커니즘으로 지키는지를 정의한다 —
> 한 번에 하나의 turn만 그 상태를 만지도록 보장하는 primitive의 계약이다. 모든 언어
> runtime이 이 계약을 따라야 한다. 순서·타임아웃·오류 코드 같은 관측 가능한 동작은
> 다른 문서가 소유하며, 이 문서가 정의하는 규칙은 그 관측 가능한 동작을 바꾸지 않는다.

## 1. 상태 소유 개요

컴포넌트는 자신이 소유한 mutable 상태(field·collection)를 가진다. 이 문서는 그 상태를
읽고 쓰는 코드가 동시에 하나만 실행되도록 보장하는 메커니즘의 규칙을 정의한다 —
어떤 상태가 어느 primitive로 지켜지는지 판별하는 기준, 그 primitive가 제공해야 하는
보장, 그리고 재진입을 만들지 않는 구조다.

이 문서가 정의하지 않는 것 — Handler가 언제 실행되고 언제 다른 handler에게 자리를
내주는지는 [Handler turn과 execution gate](02-handler-turn-and-execution-gate.ko.md)가,
message가 socket에서 handler에 이르는 동안의 소유권과 복사는
[Payload ownership과 codec](05-payload-ownership-and-codec.ko.md)이 소유한다. 이 문서의
규칙은 컴포넌트 내부 상태를 보호하는 메커니즘에 관한 것이지, application이 관측하는
순서·타임아웃·오류 코드를 바꾸는 규칙이 아니다.

## 2. 용어 구분 — state lane과 Application/lifecycle lane

이 문서의 **[state lane](../00-foundation/02-glossary.ko.md#state-lane)**은 컴포넌트 하나가 자신의 mutable 상태를 소유하는 실행 단위다.
그 컴포넌트의 상태를 읽고 쓰는 모든 코드는 이 lane 위에서만 실행된다.

상태 접근을 직렬화하는 state lane과 handler 작업을 분류하는
[application lane](../00-foundation/02-glossary.ko.md#application-lane)·[lifecycle lane](../00-foundation/02-glossary.ko.md#lifecycle-lane)은 다른 단위다.
Handler FIFO의 실행 객체별 범위는 [Handler turn과 execution gate §7](02-handler-turn-and-execution-gate.ko.md#execution-lanes)이 소유한다.
그 실행 객체는 Location Store authority가 가리키는 MeshNode [Owner](../00-foundation/02-glossary.ko.md#owner)와 구별한다.

| | state lane (이 문서) | application lane / lifecycle lane (02 §7) |
|---|---|---|
| 단위 | 컴포넌트 하나(예: 하나의 binding table, 하나의 catalog) | 실행 객체(Spot·Actor 등) 하나 — 02 §7 참조 |
| 목적 | 그 컴포넌트의 상태를 한 번에 하나의 turn만 만지게 한다 | Owner가 처리할 작업을 우선순위별로 줄 세운다 |
| 담는 것 | 그 컴포넌트의 상태를 읽고 쓰는 코드 조각 | 업무 payload·timer callback(application) 또는 join·leave·relocation·lifecycle control(lifecycle) |
| admission·우선순위 | 없다 — 들어온 순서대로(FIFO) 실행할 뿐 건수·byte 한도나 lane 간 우선순위가 없다 | 있다 — 건수·byte 한도, owner 점유 시간 예산, lifecycle 우선순위 규칙 |
| 소유 문서 | 이 문서 | [02 §7](02-handler-turn-and-execution-gate.ko.md#7-lane-분리와-우선순위-구현) |

컴포넌트 하나가 state lane을 가지는 것과, 그 컴포넌트가 속한 Spot·Actor의 handler가
application lane·lifecycle lane 중 어디로 admission되는지는 서로 다른 층의 결정이다. 한
handler 실행이 application lane에서 순서를 기다리는 동안, 그 handler가 만지는 상태는 또
다른 컴포넌트의 state lane 위에서 직렬화될 수 있다.

## 3. 금지되는 형태

state lane이 막는 형태는 다음과 같다 — **lock 같은 배타적 접근 안에서 상태의 스냅샷을
꺼내고, 그 접근을 해제한 뒤, async 경계 너머에서 그 스냅샷으로 결정을 내리는 형태.**

```csharp
Entry entry;
lock (_gate) { if (!_entries.TryGetValue(key, out entry)) return; }   // 여기서 해제
await SendAsync(entry.Route);                                         // 낡았을 수 있는 값으로 실행
```

이 형태는 실수가 아니라 **메커니즘이 강제하는 결과**다. Lock은 `await`을 감쌀 수 없다.
그래서 lock으로 상태를 지키면서 그 상태를 바탕으로 비동기 작업을 해야 하는 코드는
예외 없이 "스냅샷을 꺼내고 lock을 놓은 뒤 그 값으로 행동한다"는 모양이 된다. Lock을
다시 잡을 수 있는 지점(비동기 작업 완료 이후)에 도달했을 때, 그 사이 다른 turn이 같은
상태를 이미 바꿨을 수 있으므로 스냅샷은 구조적으로 낡는다.

state lane은 이 형태 자체를 없앤다. Lane의 turn 안에는 해제 지점이 없으므로, 상태를 읽는
코드와 그 결정을 확정하는 코드가 같은 turn 안에 들어간다.

```csharp
// contract pseudocode이며 실제 API가 아니다 — 실제 시그니처는 언어별 interface가 소유한다.
// turn 안에서 상태를 읽고, 그 결정의 유효성을 되돌릴 수 없게 고정한다.
var claim = await lane.Run(() =>
    entries.TryGetValue(key, out var entry)
        ? entry.ClaimRoute()      // generation·ownership을 이 turn 안에서 확정한다
        : null);                  // 평범한 map이다. 잠그지 않는다

if (claim is null) return;
await SendAsync(claim);           // 장기 외부 호출은 turn 밖에서 — §4·§5
```

**turn 밖으로 나가는 값이 무엇인지가 갈림길이다.** 위 금지 형태는 mutable 상태의 사본을
들고 나가서 그 값이 아직 맞다고 가정한다. 이쪽은 turn 안에서 유효성이 고정된 claim을 만들어
들고 나가므로, 그 사이 상태가 바뀌어도 claim이 무엇을 가리키는지는 변하지 않는다.

**장기 외부 호출을 turn 안에서 기다리지 않는다.** state lane의 turn이 하는 일은 상태를 읽고
결정을 확정하는 데까지다 — turn 안에서 외부 operation의 완료를 기다리는 것은 §4가 금지한다.

## 4. 상태 분류와 판별 기준

컴포넌트의 mutable 상태는 다음 분류 중 하나로 분류한다. 분류가 정해지면 그 상태를
지키는 메커니즘은 기계적으로 정해진다.

| 분류 | 판별 기준 | 메커니즘 |
|---|---|---|
| **C1 — 순수 조회 레지스트리** | 단일 map이고, 연산이 조회·추가·삭제로 끝나며, 다른 field·collection과 걸친 불변식이 없다. 배타적 접근 블록을 두 개의 원자 연산으로 쪼개도 불변식이 깨지지 않는다 | concurrent map (언어별 thread-safe map 구현) |
| **C2 — 교차 불변식 상태** | 여러 collection·field에 걸친 불변식이 있거나, 하나의 결정을 내린 뒤 그 결정에 따라 비동기 행동을 이어간다 | state lane 소유 + 평범한 map(잠그지 않는다) |
| **C3 — 원자 카운터·플래그** | 정수 증가, 플래그 확인, 단일 참조 교체만 한다 | atomic 연산(언어별 원자 증가·비교교환·volatile 참조) |

```csharp
// contract pseudocode이며 실제 API가 아니다 — 실제 시그니처는 언어별 interface가 소유한다.
class RouteRegistry            // C1 — 조회·추가·삭제뿐이고 걸친 불변식이 없다
{
    ConcurrentMap<RouteKey, Route> routes;
}

class BindingTable             // C2 — 두 map이 서로를 참조한다. 한 쪽만 바뀌면 깨진다
{
    ZLinkStateLane lane;
    Map<ActorId, SessionId> actorToSession;   // 평범한 map
    Map<SessionId, ActorId> sessionToActor;   // lane이 배타성을 보장한다

    Task Bind(actorId, sessionId) => lane.Run(() =>
    {
        actorToSession[actorId]   = sessionId;   // 둘이 한 turn에서 함께 바뀐다
        sessionToActor[sessionId] = actorId;
    });
}

class SendCounter              // C3 — 증가만 한다
{
    Atomic<long> sent;
}
```

한 컴포넌트 안에 세 종류가 섞여 있으면 **C2가 이긴다** — 가장 강한 요구가 그
컴포넌트 전체의 메커니즘을 결정한다. 컴포넌트를 부분적으로만 lane으로 옮기고 나머지를
concurrent map이나 별도 lock으로 남기면, C2가 막으려는 교차 불변식 위반이 그 경계에서
다시 나타난다.

**C2를 concurrent map으로 치환하지 않는다.** Concurrent map은 map 하나에 대한 개별
연산만 원자적으로 만든다. 여러 collection·field에 걸친 불변식은 map 단위의 원자성으로
지켜지지 않는다 — 원자성이 그 map 하나로 쪼개지면서 불변식 자체가 깨진다.

**C2를 다른 배타적 접근 primitive(semaphore 등)로 치환하지 않는다.** 배타적 접근을
제공하는 primitive를 lock에서 semaphore로 바꿔도 [§3](#3-금지되는-형태)이 막는 형태는
그대로 남는다 — 얻는 것은 숫자(잠금 개수)가 줄었다는 것뿐이고, "배타적 접근 해제 뒤
스냅샷으로 행동한다"는 구조는 바뀌지 않는다.

### 상태 보호와 작업 프로토콜 직렬화를 구분한다

외부 비동기 작업 전체를 하나씩 실행하기 위한 semaphore·socket gate·dispose gate는
컴포넌트의 mutable 상태를 보호하는 state lane과 책임이 다르다. 다음 조건을 모두
만족할 때만 그 gate를 작업 프로토콜 primitive로 유지할 수 있다.

- gate가 보호하는 것이 외부 resource operation의 시작·종료 또는 exact-once
  disposal이며, C2 상태의 일부를 별도로 소유하지 않는다.
- operation을 시작하기 전에 필요한 generation·identity·ownership을 gate 안에서
  확정한다.
- gate를 놓은 뒤에는 mutable 상태 스냅샷이 아니라 Task, reservation, seal token 또는
  단독 ownership을 이전받은 resource를 사용한다.
- 완료 경로는 같은 ownership을 정확히 한 번 반납한다.

작업 프로토콜 gate 안에서 state lane의 완료를 기다리는 것은 허용한다. 그러나 반대
방향 — state lane의 turn 안에서 그 gate의 획득이나 장기 operation의 완료를 기다리는
것 — 은 금지한다. 두 방향을 모두 허용하면 서로가 서로의 완료를 기다리는 역방향
deadlock이 생긴다.

이 예외는 C2 상태를 여러 lock으로 나누는 허가가 아니다. 같은 불변식에 참여하는
field와 collection은 여전히 하나의 state lane이 소유한다.

## 5. state lane의 보장과 제약

state lane은 다음을 보장한다.

- **한 번에 한 turn만 실행한다.** 같은 lane 위의 두 turn이 동시에 실행되지 않는다.
- **FIFO다.** Lane에 들어온 순서대로 실행한다. 앞쪽 삽입은 없다.
- **비재진입이다.** 이미 그 lane의 turn 위에서 실행 중인 코드가 같은 lane에 다시
  진입하려 하면 거부한다.
- **소유한 collection은 잠그지 않는다.** Lane이 소유한 상태를 담는 collection은 평범한
  구조로 둔다 — lock이 아니라 lane의 단일 실행이 배타성을 보장하므로 collection 자체가
  thread-safe일 필요가 없다.

```csharp
// contract pseudocode이며 실제 API가 아니다 — 실제 시그니처는 언어별 interface가 소유한다.
interface ZLinkStateLane
{
    Lane Current { get; }        // 지금 실행 중인 lane. 없으면 null
    bool IsOnLane { get; }       // 호출자가 이 lane의 turn 위에 있는가
    Task<T> Run<T>(work);        // 이 lane의 turn 하나에서 실행하고 결과를 돌려준다
    bool TryPost(work);          // 결과를 기다리지 않고 넣는다. 닫혀 있으면 false
    void ThrowIfReentrant();     // 이미 이 lane 위면 그 자리에서 예외
    Task Close();                // 새 제출을 막고 이미 받은 작업을 끝낸다
}
```

이 여섯은 4개 언어가 같은 이름과 같은 의미를 갖는다(§7).

**재진입은 데드락이 아니라 예외로 검출되어야 한다.** 왜 재진입이 처리할 방법이 없는
상태인지는 구체적인 순서로 보면 드러난다.

1. 같은 컴포넌트의 public 메서드 A가 이미 lane의 turn 위에서 실행 중이다.
2. A의 본문이 같은 컴포넌트의 public 메서드 B를 호출한다.
3. B도 상태에 접근하려고 같은 lane에 들어가야 하므로, "지금 이 lane에서 실행 중인
   turn이 끝나기"를 기다린다.
4. 그런데 지금 이 lane에서 실행 중인 turn이 바로 B를 부른 A 자신이다.

B는 A가 끝나기를 기다리고, A는 B가 끝나야 자신의 turn을 마칠 수 있다 — 결국 A가 A
자신이 끝나기를 기다리는 것과 같다. 이 대기는 다른 turn이 끼어들어도 풀리지 않으므로
영원히 끝나지 않는다.

이 상태를 그대로 두면(hang) 서버는 조용히 멈춘다. 어느 호출이 원인인지 로그나 스택
트레이스에 남지 않으므로, 원인을 찾으려면 그 순간의 실행 덤프를 통째로 뒤져야 한다.
Lane은 이 상황을 hang이 아니라 재진입이 일어난 바로 그 호출 지점에서 예외로 끝내야
한다 — 어느 lane에 재진입했는지 알려주는 오류가 그 자리에서 발생하면, 스택 트레이스가
원인 코드를 곧바로 가리킨다.

### 완료 신호와 블로킹 호환 경계

Lane work의 완료 신호는 lane의 현재 소유권 표시가 해제된 뒤에 caller continuation을
실행해야 한다. 완료 API가 dependent continuation을 완료 thread에서 inline 실행할 수
있는 언어에서는, 완료 신호를 lane-current scope 안에서 직접 완료하지 않는다. 완료를
scope 밖 scheduler에 게시하거나 continuation을 비동기로 강제한다.

기존 동기 표면이 state lane의 완료를 블로킹 대기하는 호환 경계는 다음 조건을 모두
만족할 때만 허용한다.

- 제출한 lane 항목이 현재 보유 중인 외부 gate를 다시 획득하지 않는다.
- lane 항목의 모든 완료 신호는 continuation을 비동기로 실행한다.
- 그 동기 표면이 반환되기 전에 상태 등록·캡처가 완료돼야 하는 계약이 있거나, 공개 동기
  signature를 바꿀 수 없다는 사유가 기록돼 있다.

이 세 조건 중 하나라도 확인할 수 없으면 gate를 보유한 채 lane 완료를 기다리지 않는다.
호출 경로를 비동기로 전파하거나, gate와 lane의 책임을 다시 분리한다.

### 반환 전 완료 보장

원본 동기 메서드가 waiter 등록, generation 캡처, store 판독 또는 ownership claim을 **반환
전에** 끝냈다면, lane으로 옮긴 뒤에도 caller가 반환을 관찰하기 전에 그 작업이 끝나 있어야
한다. 비동기 fire-and-forget 게시로 바꾸지 않는다.

```csharp
// contract pseudocode이며 실제 API가 아니다 — 실제 시그니처는 언어별 interface가 소유한다.
Task<Reservation> Reserve(key)
{
    // 반환을 관찰한 caller는 이미 예약이 잡혔다고 믿는다.
    return lane.Run(() => reservations.Add(key));
}

Task<Reservation> Reserve_WRONG(key)
{
    lane.TryPost(() => reservations.Add(key));   // 아직 안 잡혔는데 반환한다
    return pendingReservation;
}
```

완료 신호를 기다리는 **이후** 단계는 비동기로 남겨도 된다. 등록·캡처 자체를 반환 뒤로
미루지 않는다.

**공개 계약이 동기면 state lane 도입만을 이유로 비동기로 바꾸지 않는다.** 내부 호출자가
이미 비동기이고 관측 계약이 변하지 않을 때만 async signature를 전파한다.

**동적 제어 표면(실행 중 변경 가능한 flag·진단 레벨 등)은 dual 표면으로 제공한다.**
bindings의 완료 표면 정책(binding 스펙 async-coroutine-policy — ASYNC 분류 operation에도
sync terminal을 나란히 제공)과 같은 모형이다: async 표면이 정본이고, sync 표면은 그 위의
최소 bridge 1개(위 세 조건 충족)로 제공한다. sync 표면은 **framework가 소유한 실행
문맥(handler·lane·turn·완료 callback) 밖 전용**이며 — 설정 시점, 운영 도구, 테스트가
그 대상이다. framework 실행 문맥 안에서는 async 표면을 쓴다. 언어별로 한쪽 표면만
제공해 발산하지 않는다.

## 6. 재진입을 만들지 않는 구조

재진입은 우연히 생기지 않는다. 세 가지 자리에서 구조적으로 생기며, 각 자리에 정해진
모양이 있다.

**유형 ① — lane 안에서 같은 컴포넌트의 public 표면을 다시 부르는 자리.** 한 public
진입점이 lane에 들어간 turn 안에서 같은 컴포넌트의 다른 public 메서드를 호출하면, 그
메서드도 같은 lane에 들어가려 하므로 재진입이 된다. 이런 자리는 lane에 들어가지 않는
private 메서드로 분리한다. Public 진입점은 lane에 한 번만 들어가고, lane 안에서는 그
private 메서드의 본문을 직접 호출한다.

```csharp
// contract pseudocode이며 실제 API가 아니다 — 실제 시그니처는 언어별 interface가 소유한다.
Task Bind(actorId, sessionId) => lane.Run(() => BindOnLane(actorId, sessionId));
Task Rebind(actorId, sessionId) => lane.Run(() =>
{
    UnbindOnLane(actorId);              // Unbind()를 부르면 lane에 다시 들어간다
    BindOnLane(actorId, sessionId);     // lane 안에서는 본문을 직접 부른다
});

void BindOnLane(actorId, sessionId) { ... }    // lane에 들어가지 않는다
void UnbindOnLane(actorId) { ... }
```

**유형 ② — lane turn 안에서 시작한 장기 비동기 작업이 lane 소유권을 상속하는 경우.**
Turn 안에서 timeout·retry·background loop 같은 작업을 시작하면, 그 작업이 실행되는
문맥이 "지금 이 lane 위에서 실행 중"이라는 표시를 그대로 물려받을 수 있다. 지연이 끝난
뒤 그 작업이 같은 lane에 다시 들어가려 하면, 실제로는 원래 turn이 이미 끝난 뒤인데도
재진입으로 검출된다.

이런 장기 작업을 시작하는 자리에서는 실행 문맥의 흐름을 끊는다. 다만 문맥 흐름을
끊는 조치는 그 비동기 작업이 실제로 thread 전환을 거친 뒤에만 효력이 있다. 호출한
async 함수의 동기 prefix가 첫 `await` 전에 같은 lane에 재진입할 수 있다면, 문맥 흐름을
끊는 것만으로는 충분하지 않다 — 이 경우 작업 시작 자체를 별도 scheduler에 게시해 동기
prefix까지 원래 turn 밖에서 실행되게 한다. 반대로 첫 동작이 실제 비동기 지연이고 그
전에 lane 재진입이 없다면, 문맥 흐름을 끊는 것만으로 충분하다.

```csharp
// contract pseudocode이며 실제 API가 아니다 — 실제 시그니처는 언어별 interface가 소유한다.
await lane.Run(() =>
{
    state.retrying = true;
    StartRetryLoop(key);        // 이 안에서 문맥 흐름을 끊는다
});

void StartRetryLoop(key)
{
    using (SuppressLaneContext())          // "지금 이 lane 위" 표시를 물려주지 않는다
        RunDetached(() => RetryLoop(key));  // 동기 prefix가 lane에 닿을 수 있으면
                                            //   시작 자체를 별도 scheduler에 게시한다
}
```

**유형 ③ — 기존 배타적 접근 안에서 외부 callback을 호출하던 경우.** Monitor 재진입으로
동작하던 callback을 lane turn 안에서 직접 호출하면, 그 callback이 같은 컴포넌트의
public 표면에 재진입한다. 다음 세 단계로 나눈다.

1. turn A에서 검증, 결과 산출, 원본이 callback 전에 끝내던 상태 전이 전부와 placeholder
   ownership claim을 완료한다.
2. callback은 lane turn 밖에서 호출한다.
3. turn B에서는 그 placeholder를 callback 결과 Task로 정확히 교체하거나 실패를 정산한다.

원본이 callback 전에 끝내던 상태 전이를 turn B로 미루지 않는다. Callback 실행 창의
경쟁 관측자는 원본의 "배타적 접근이 끝난 뒤"와 같은 상태를 봐야 한다.

**배타적 접근 primitive의 재진입 허용에 기대지 않는다.** 일부 언어의 배타적 접근
primitive(C#의 Monitor, Java의 `synchronized`)는 같은 thread의 중첩 획득을 허용한다.
배타적 접근 안에서 호출한 외부 callback·listener가 같은 primitive를 다시 획득하는 구조는
그 허용 덕에만 동작하는 잠재 재진입이다 — state lane으로 옮기는 순간 재진입 예외가 되고,
옮기지 않더라도 그 접근이 보장하려던 원자성이 밖으로 나간 제어의 행동에 의존하게 된다.
이런 자리는 유형 ③으로 분리한다. 같은 컴포넌트 안에서 제어가 밖으로 나가지 않는 private
중첩 획득(self-call)은 이 금지의 대상이 아니다.

## 7. 언어별 매핑

.NET에서는 `Zlink.Framework.Runtime.Execution.ZLinkStateLane`이 이 문서가 정의하는
규칙의 기준 구현이다. 상태 접근은 이 lane에 제출한 작업으로 실행하며, lane이
소유한 collection은 평범한 `Dictionary`로 둔다. 재진입은 hang이 아니라 그 호출 지점에서
즉시 `InvalidOperationException`으로 검출된다.

`ZLinkStateLane`은 Spot·Actor **실행**에 쓰는 `ZLinkSerialExecutionQueue`와 별개다.
`ZLinkSerialExecutionQueue`에는 relocation seal과 lifecycle admission이 함께 있는데, 이는
상태 소유 목적에는 필요 없는 책임이다 — 상태를 지키려는 컴포넌트가 이 실행용 queue를
가져다 쓰면 그 queue가 지고 있는 relocation·lifecycle 책임까지 함께 떠안게 된다. 그래서
상태 소유에는 이 실행용 queue를 쓰지 않는다.

다른 언어로 포팅할 때는 구체적인 자료형이나 언어 관용구를 그대로 옮기는 대신, 이 문서가
정의하는 같은 보장 — 한 번에 한 turn만 실행, FIFO, 재진입의 즉시 예외 검출, 소유
collection의 무잠금 — 을 만족하는 그 언어의 primitive를 쓴다.

**다만 공개 표면의 이름과 계약은 예외다.** state lane의 다음 여섯은 4개 언어가 같은 의미를 갖는다.
앞의 다섯은 이름도 표기 변환만으로 일치한다. `close`는 현재 언어마다 이름이 갈리므로
**통일 대상**이다 — 표의 이름은 현재 실측이고, 목표는 `close` 한 벌이다.

| 계약 | 의미 | .NET | java | cpp | node |
|---|---|---|---|---|---|
| `current` | 현재 실행 중인 lane | `Current` | `current()` | `current()` | `current` |
| `isOnLane` | 이 lane 위인가 | `IsOnLane` | `isOnLane()` | `is_on_lane()` | `isOnLane` |
| `run` | lane turn에서 실행 | `RunAsync()` | `runAsync()` | `run()` | `run()` |
| `tryPost` | 대기 없이 게시 | `TryPost()` | `tryPost()` | `try_post()` | `tryPost()` |
| `throwIfReentrant` | 재진입 시 예외 | `ThrowIfReentrant()` | `throwIfReentrant()` | `throw_if_reentrant()` | `throwIfReentrant()` |
| `close` | 종료 | `DisposeAsync()` | `closeAsync()` | `close()` | `dispose()` |

<a id="state-lane-reentrancy"></a>
- **같은 state lane의 현재 turn을 기다리는 재진입은 진입 지점에서 거부한다.**
  `throwIfReentrant` 검사는 build mode나 과거 lock 구현과 관계없이 필수다.
  검사가 없으면 현재 turn 뒤에 놓인 작업을 그 turn이 기다려 교착되기 때문이다.

실행기 계층(Spot·Actor·Session 조율자와 직렬 큐 primitive)의 이름과 계약은
[07. 직렬 실행기 계층](07-serial-executor-layers.ko.md)이 정한다.

.NET은 lane 소유권 표시에 `AsyncLocal`을 쓰므로, 장기 작업을 시작하는 지점에서는
`ExecutionContext.SuppressFlow`로 그 상속을 끊는다. Async 함수의 동기 prefix가 첫
`await` 전에 lane에 재진입할 수 있으면, `Task.Run` 등으로 시작 자체를 별도
scheduler에 게시한다.

Java는 lane-current `ThreadLocal` scope 안에서 `CompletableFuture.complete`를 호출하지
않는다. `complete`는 비동기 표시가 없는 dependent를 완료 thread에서 inline 실행할 수
있다. Current scope를 해제한 뒤 완료하거나, `completeAsync`처럼 별도 scheduler에서
완료하는 API를 쓴다.

Node.js의 동기 메서드는 하나의 JavaScript turn 안에서 끝나는 동안 다른 callback과
동시에 실행되지 않는다. 따라서 `await` 전후로 상태 접근이 갈라지지 않고 public
재진입 검출도 필요 없는 동기 표면은 Promise로 바꾸지 않는다. State lane이 필요한 곳은
`await` 전후로 상태가 갈라지는 비동기 경로와, 재진입 검출이 필요한 표면뿐이다.

## 8. 검증 요구

공개 표면(state lane에 제출한 작업의 반환값과 예외, 재진입 호출이 받는 오류, 컴포넌트
메서드가 반환한 시점에 관측되는 상태)만으로 다음을 확인한다. 각 항목은 test 하나로
이어진다.

**단일 실행과 순서**

- 서로 다른 호출자가 동시에 같은 lane에 작업을 제출해도 갱신이 유실되지 않는다 — 이후
  조회가 모든 제출을 반영한 값을 돌려준다.
- 같은 lane에 제출한 작업은 제출한 순서대로 실행된다.
- 닫힌 lane에 결과를 기다리는 호출을 제출하면 즉시 예외로 끝나고, 결과를 기다리지 않는
  제출은 실패를 반환한다.

**재진입**

- 이미 그 lane의 turn 위에서 실행 중인 코드가 같은 lane에 다시 진입을 시도하면, 멈추지
  않고 그 호출 지점에서 즉시 예외로 끝난다.
- Lane turn에서 시작한 장기 작업은 원래 turn이 끝난 뒤 같은 lane에 정상 진입하며, 거짓
  재진입 예외를 만들지 않는다. 동기 prefix가 있는 장기 작업도 마찬가지다.
- 외부 callback은 turn 밖에서 실행되며, callback이 관찰하는 상태는 원본이 callback 전에
  끝내던 상태 전이가 모두 반영된 상태다.

**완료 경계**

- turn 안에서 완료를 신호해도 그 continuation이 재진입 예외로 끝나지 않고 정상 실행된다.
- 반환 전 등록·캡처 계약이 있는 메서드는, caller가 반환을 관찰하는 시점에 그 등록·캡처가
  이미 완료돼 있다.
- 작업 프로토콜 gate를 보유한 채 lane 완료를 기다리는 호출이 멈추지 않고 반환된다.

**교차 불변식**

- 한 불변식에 참여하는 여러 collection을 바꾸는 호출이 동시에 들어와도, 바깥에서 두
  collection을 함께 읽었을 때 서로 어긋난 상태가 관측되지 않는다.


---

[Execution 주제 목차](README.ko.md) · [스펙 목차](../README.ko.md) · [이전: 05. Payload 소유권과 codec](05-payload-ownership-and-codec.ko.md) · [다음: 07. 직렬 실행기 계층](07-serial-executor-layers.ko.md)
