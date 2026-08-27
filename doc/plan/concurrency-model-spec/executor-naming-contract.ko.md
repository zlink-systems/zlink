# 실행기 구조 — 명명·계약 고정

작성 2026-08-28. [스펙 초안](spec-draft-concurrency-model.ko.md) §4.4에서 확정한
**(A) dotnet 조율 + java primitive**를 구현이 갈라지지 않게 **클래스·메서드 이름까지** 고정한다.

**구현은 별도 세션에서 진행한다.** 이 문서가 그 세션의 계약서다.

---

## 0. 언어별 명명 변환 규칙

이름은 한 번만 정하고, 각 언어는 **기계적 변환**만 한다. 새 이름을 짓지 않는다.

| 언어 | 타입 | 메서드 | 필드 |
|---|---|---|---|
| dotnet | `ZLinkSpotSerialExecutor` | `PascalCase` + `Async` 접미 | `_camelCase` |
| java | `ZLinkSpotSerialExecutor` | `camelCase` | `camelCase` |
| cpp | `spot_serial_executor_t` | `snake_case` | `_snake_case` |
| node | `ZLinkSpotSerialExecutor` (파일 `spot-serial-executor.ts`) | `camelCase` | `camelCase` |

**규칙**: 아래 §1~§3의 정본 이름에서 위 표대로 변환한다. 의미를 바꾸는 개명은 금지한다.
(예: `executeActor` → cpp `execute_actor`는 허용, `dispatch_actor`는 금지)

---

## 1. 조율자 — `ZLinkSpotSerialExecutor`

**정본: dotnet `Runtime/Spots/ZLinkSpotSerialExecutor.cs`**

Spot 하나마다 인스턴스 하나. **세 종류 직렬 단위의 수명과 라우팅을 모두 소유**한다.

### 1.1 소유 상태 (고정)

| 정본 이름 | 타입 | 의미 |
|---|---|---|
| `spotQueue` | 직렬 큐 1개 | Spot 자신의 메시지·lifecycle |
| `actorQueues` | `Map<ActorId, 직렬 큐>` | Actor별 |
| `timerQueues` | `Map<TimerName, 직렬 큐>` | Timer별 |
| `executionMode` | `SpotWide` \| `PerActor` | 큐 중첩을 결정 |

**R-N1.** 이 넷은 **조율자만 소유한다.** 다른 계층이 Actor 큐나 Timer 큐를 따로 들지 않는다.
> java 현행 위반: `ZLinkActorDispatchSerials.queues`가 Actor 계층에 있다 → 조율자로 이관한다.
> cpp 현행 위반: `spot_runtime` 안 이름 맵으로 흩어져 있다 → 조율자로 모은다.

**R-N2.** `actorQueues` · `timerQueues` 맵 자체는 **state lane이 소유**한다. lock으로 지키지 않는다.
> dotnet 현행 위반: `_laneGate` lock을 쓴다 → state lane으로 바꾼다.
> java는 이미 `ZLinkStateLane`으로 지킨다 — 이 형태가 맞다.

### 1.2 진입점 (고정)

**호출자는 어느 큐로 가는지 모른다.** 조율자가 정한다.

| 정본 이름 | 인자 | 가는 큐 |
|---|---|---|
| `executeSpot` | operation | `spotQueue` |
| `executeActor` | `actorId`, operation | §1.3 표 |
| `executeTimer` | `timerName`, operation | §1.3 표 |
| `executeLifecycle` | operation | `spotQueue` (lifecycle 우선순위) |

**R-N3.** 위 넷 외의 공개 진입점을 만들지 않는다. 호출자가 큐를 직접 고르는 API를 노출하지 않는다.

### 1.3 라우팅 규칙 (고정)

| 경로 | `PerActor` | `SpotWide` |
|---|---|---|
| `executeActor` | `actorQueues[actorId]` | `actorQueues[actorId]` → **`spotQueue`** (2단) |
| `executeTimer` | `timerQueues[timerName]` | **`spotQueue`만** (timer 큐 생성 안 함) |
| `executeSpot` | `spotQueue` | `spotQueue` |
| `executeLifecycle` | `spotQueue` | `spotQueue` |

**R-N4.** Actor가 `SpotWide`에서 2단인 이유는 **순서가 아니라 용량 회계**다(§3 참조).
Timer는 payload가 없어 회계할 것이 없으므로 `SpotWide`에서 큐를 만들지 않는다.
**이 비대칭을 "버그"로 보고 대칭화하지 않는다.**

---

## 1.5 Actor·Session 조율자 — 소유 단위가 다르다 (실측 2026-08-28)

범위는 **Spot·Actor·Session 셋**이다(미결 ③ 확정). 다만 셋의 소유 구조가 같지 않다.

### 실측

| 계층 | dotnet | java | cpp | node |
|---|---|---|---|---|
| Spot | `ZLinkSpotSerialExecutor` 1,320 | 없음 | 없음 | `spot-serial-executor` 314 |
| Actor | `ZLinkActorDispatchMailbox` 341 | 없음 | 없음 | 없음 |
| Session | `ZLinkStreamSessionSerialExecutor` 111 | 없음 (`ZLinkStreamRuntime`의 `stateLane`) | 없음 (`stream_runtime.dispatch_queue`) | `session-serial-executor` 73 |

**Spot과 결정적 차이**: Actor·Session 실행기는 **큐 맵이 없다. 인스턴스당 큐 하나**다.

```csharp
// dotnet ZLinkActorDispatchMailbox — Actor 하나당 인스턴스 하나
private readonly ZLinkStateLane _lane = new();
private readonly ZLinkSerialExecutionQueue _queue;   // ← 맵이 아니라 단수

// dotnet ZLinkStreamSessionSerialExecutor — Session 하나당 인스턴스 하나
private readonly ZLinkSerialExecutionQueue _queue;   // ← 단수
private readonly ZLinkStateLane _lane = new();
```

Spot만 "Spot 1 + Actor N + Timer M"을 **소유**하므로 맵을 갖는다. Actor·Session은 자기 것
하나만 갖고, 하위 소유 대상이 없다.

### 규범

**R-N1a.** 조율자는 **자기 계층의 직렬 단위 + 자신이 수명을 소유하는 하위 단위**만 갖는다.

| 계층 | 소유 | 근거 |
|---|---|---|
| **Spot** | `spotQueue` + `actorQueues` + `timerQueues` + `executionMode` | Actor·Timer의 수명이 Spot에 종속 |
| **Actor** | `actorQueue` (단수) | 하위 소유 대상 없음 |
| **Session** | `sessionQueue` (단수) | 하위 소유 대상 없음 |

**하위 단위가 없는 계층에 맵을 만들지 않는다.** Spot 형태를 기계적으로 복사하지 않는다.

**R-N1b.** 정본 이름:

| 계층 | 조율자 | 진입점 |
|---|---|---|
| Spot | `ZLinkSpotSerialExecutor` | §1.2의 4개 |
| Actor | `ZLinkActorSerialExecutor` | `executeActor` · `executeLifecycle` |
| Session | `ZLinkSessionSerialExecutor` | `executeApplication` · `executeControl` · `executeInfrastructure` · `executeFinal` |

> dotnet 현행 `ZLinkActorDispatchMailbox` → `ZLinkActorSerialExecutor`로 개명한다.
> "Mailbox"는 다른 계층과 어휘가 갈린다.
> dotnet 현행 `ZLinkStreamSessionSerialExecutor` → `ZLinkSessionSerialExecutor`.
> `Stream` 접두는 계층 이름과 중복이다.

**R-N1c.** Session 진입점 4종은 dotnet 현행(`EnqueueApplication`·`EnqueueControl`·
`EnqueueInfrastructure`·`EnqueueFinal`)을 정본으로 하되 **동사를 `execute`로 통일**한다.
계층마다 `enqueue`/`execute`가 갈리지 않게 한다.

**R-N1d.** java·cpp는 **세 계층 모두 조율자가 없다.** `ZLinkStreamRuntime`의 `stateLane`,
`stream_runtime.dispatch_queue`처럼 런타임 안에 직접 들고 있다. **셋 다 신설한다.**

**R-N1e.** node는 Spot·Session 조율자가 있고 Actor는 없다. **Actor 조율자를 신설하지 않는다** —
단일 스레드라 Actor별 직렬 단위가 무의미하다(R-N13과 같은 근거). `executeActor`는 Spot
조율자가 받아 `spotQueue`로 라우팅한다.

---

## 2. 직렬 큐 primitive — `ZLinkSerialExecutionQueue`

**정본: java `execution/ZLinkAsyncSerialQueue.java`의 책임 + dotnet의 이름**

> 이름은 dotnet `ZLinkSerialExecutionQueue`를 쓴다(java `AsyncSerialQueue`는 `Async`가
> 언어 관용구라 4언어 공통 이름으로 부적절). **책임은 java 것을 정본으로 한다.**

### 2.1 정책 (고정)

**R-N5.** 다음을 **정책 객체로 주입**받는다. 하드코딩하지 않는다.

| 정본 이름 | 의미 |
|---|---|
| `applicationMessageCapacity` | application 레인 메시지 수 상한 |
| `applicationByteCapacity` | application 레인 바이트 상한 |
| `lifecycleMessageCapacity` | lifecycle 레인 메시지 수 상한 |
| `lifecycleByteCapacity` | lifecycle 레인 바이트 상한 |
| `fixedWorkByteCost` | payload 없는 작업의 고정 비용 |
| `lifecycleBurstLimit` | lifecycle이 연속 선점할 수 있는 최대 건수 |
| `ownerTimeBudget` | **한 소유자가 연속 점유할 수 있는 시간** |

정책 객체 이름: **`ZLinkExecutionLanePolicy`** (java 현행 이름 유지).

### 2.2 진입점 (고정)

| 정본 이름 | 의미 |
|---|---|
| `enqueue` | application 작업. `fixedWorkByteCost`로 회계 |
| `enqueueWithPayloadBytes` | application 작업 + 실제 payload 바이트 |
| `enqueueLifecycle` | lifecycle 레인 |
| `enqueueBarrierNext` | 현재 turn 직후, 대기 중 application turn보다 먼저 |
| `isCurrent` | 호출 스레드가 이 큐의 turn을 소유하는가 |
| `awaitQuiescence` | 큐가 빌 때까지 |
| `close` | 종료 |

**R-N6.** 용량 판정·시퀀스 발급·큐 삽입은 **한 임계 구역에서 원자적으로** 한다.
동시성 큐로 치환하지 않는다 — 쪼개면 용량 초과와 시퀀스 역전이 난다(C2 불변식).

**R-N7.** `ownerTimeBudget` 초과 시 **현재 소유자가 양보**한다. 이 장치가 없으면 한 작업이
큐를 무한정 점유할 수 있다.
> dotnet 현행 결손: 대응물이 **아예 없다** → 추가한다.
> cpp 현행 결손: 확인 후 추가한다.

---

## 3. 용량 회계 분담 (고정)

**R-N8.** `executeActor`가 `SpotWide`로 2단을 탈 때:

```
actorQueues[actorId] .enqueueWithPayloadBytes(payloadBytes, ...)   ← payload admission 소유
    └→ spotQueue     .enqueue(...)                                 ← 고정 turn 비용만
```

**Actor 큐가 payload admission을 소유하고, 상위 Spot 큐는 고정 비용만 예약한다.**
같은 payload를 두 큐에서 이중 예약하지 않는다.

> 근거: java 현행 주석 — *"The Actor queue owns payload admission. The shared Spot gate
> reserves only its fixed turn cost here."* 이 분담이 없으면 Actor별 용량 제한이 무의미해지거나
> 상위 큐가 조기 포화된다.

---

## 4. 상태 조회 — turn 경계 (고정)

**정본: java `ZLinkActorRuntime.ActorStateSnapshot` 패턴**

**R-N9.** 한 메시지 경로에서 필요한 상태는 **한 turn에서 스냅샷으로 함께 잡는다.**
조회마다 별도 turn을 만들지 않는다. (= 발견 10, 초안 R9)

```
// 정본 형태
snapshot = stateLane.run(() -> new ActorStateSnapshot(
    registry.actor(actorId),
    registry.context(actor),
    registry.actorType(actorId)));   // 셋을 한 turn에서
```

**R-N10.** 같은 값을 한 경로에서 **두 번 이상 읽지 않는다.** 스냅샷을 들고 다닌다.
> cpp 현행 위반: `spot_runtime.cpp:9617`과 `:9707`이 같은 authority fence를 두 번 읽는다.

스냅샷 타입 이름: **`<대상>StateSnapshot`** (예: `ActorStateSnapshot`, `SpotStateSnapshot`).

---

## 5. 자기 강제 (고정)

**R-N11.** 상위 직렬 큐가 소유를 보장하는 자리는 **반드시 단언한다.**

| 위치 | 호출 |
|---|---|
| 컴포넌트 진입 경계 | `isOnLane` 확인 |
| 재진입 가능 지점 | `throwIfReentrant` |

**단언 없는 상위 소유는 금지한다.**

> 실증: java `ZLinkStreamRuntime` pending-session이 상위 직렬 소유를 전제했다가 재진입이 뚫려
> **런타임 데드락**에 빠졌다 —
> `getOrCreateSessionState(:1379) → createSessionState(:1478) → getOrCreateSessionState(:1371)`.
> lane이었으면 `throwIfReentrant`가 즉시 예외로 잡았다.

**R-N12.** 디버그 빌드는 필수, 릴리스는 선택. 단 **재진입 이력이 있는 컴포넌트**
(cpp에서 `recursive_mutex`를 쓰던 곳)는 릴리스에서도 유지한다.

---

## 6. 언어별 현행 대비 작업

| 언어 | Spot 조율자 | Actor 조율자 | Session 조율자 | 큐 primitive | 기타 |
|---|---|---|---|---|---|
| **dotnet** | **정본** 그대로 | **개명** `ActorDispatchMailbox`→`ActorSerialExecutor` | **개명** `StreamSessionSerialExecutor`→`SessionSerialExecutor` · 동사 `Enqueue*`→`Execute*` | `ownerTimeBudget` **추가**(R-N7) · 정책 주입(R-N5) | `_laneGate` lock → state lane(R-N2) |
| **java** | **신설** — `ZLinkActorDispatchSerials.queues` 이관(R-N1) | **신설** | **신설** — `ZLinkStreamRuntime.stateLane`에서 분리 | 책임 **정본**. 클래스명만 `ZLinkSerialExecutionQueue`로 | 이미 state lane 소유 — 유지 |
| **cpp** | **신설** — `spot_runtime` 이름 맵 이관(R-N1) | **신설** | **신설** — `stream_runtime.dispatch_queue`에서 분리 | 용량·우선순위·공정성 **추가**(R-N5·R-N7) | 조회 스냅샷 묶기(R-N9·R-N10) |
| **node** | 있음 — 큐 맵 없음(§7) | **만들지 않음**(R-N1e) | 있음(`session-serial-executor` 73줄) | `ZLinkBoundedSerialScheduler` 정렬 | turn 경계·lock 문제 없음 |

---

## 7. node 예외 (확인 완료 2026-08-28)

node `spot-serial-executor.ts`는 **`scheduler` 하나만 갖고 `actorQueues`·`timerQueues`가 없다.**
전체 검색에서도 actorId별 직렬 단위가 없다.

**이는 결손이 아니라 일관된 설계다.** node는 단일 스레드라 Actor 간 병렬 실행이 애초에
불가능하고, JS turn이 이미 원자적이다. Actor별 큐를 두어도 병렬성이 생기지 않는다.

**R-N13.** node는 `actorQueues`·`timerQueues`를 **두지 않는다.** `executeActor`·`executeTimer`
진입점은 §1.2대로 제공하되 내부적으로 `spotQueue`로 라우팅한다. 진입점 계약은 4언어 동일하고
구현만 다르다.

---

## 8. 정본 언어 — 계층별 (개정 필요)

| 계층 | 정본 | 근거 |
|---|---|---|
| state lane primitive | **.NET** | 4언어 이미 동일 |
| 실행기 조율자 | **.NET** | §1 — 소유 관계가 타입으로 드러난다 |
| 직렬 큐 primitive | **java** | §2 — 용량·우선순위·공정성 |
| turn 경계 (스냅샷) | **java** | §4 — 유일하게 지킨 구현 |

**`reference-first-porting-policy`의 ".NET 단일 정본"을 위 표로 개정해야 한다.**
개정하지 않으면 다음 포팅에서 또 충돌한다.

---

## 9. 검증

**R-N14.** 구현 세션은 다음을 확인한다.

1. 조율자 외에 Actor·Timer 큐를 소유하는 곳이 **0**인가 (R-N1)
1a. **하위 소유 대상이 없는 계층(Actor·Session)에 큐 맵이 없는가** (R-N1a)
1b. 세 계층 조율자 이름이 §1.5 표와 일치하는가. 동사가 `execute`로 통일됐는가 (R-N1b·R-N1c)
2. `actorQueues`·`timerQueues` 맵이 lock이 아닌 state lane 소유인가 (R-N2)
3. §1.2의 네 진입점 외 공개 API가 없는가 (R-N3)
4. `ownerTimeBudget`이 4언어에 있는가 (R-N7)
5. `SpotWide` Actor 경로에서 payload가 **이중 예약되지 않는가** (R-N8)
6. 한 메시지 경로에서 같은 값을 두 번 읽는 자리가 **0**인가 (R-N10)
7. 상위 소유를 전제하는 자리에 단언이 있는가 (R-N11)

각 항목은 정적 계수로 확인 가능하다. 게이트는 언어별 unit·계약 + Z0 + 6샘플이다
([`../concurrency-redesign/rules.ko.md`](../concurrency-redesign/rules.ko.md) §4).
