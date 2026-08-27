# 스펙 초안 — 동시성 모델 고정 (4언어 공통)

작성 2026-08-27 · **개정 2026-08-27** (조사 3건 완료 후 전면 재작성)

사용자 목표는 **"고성능 실시간 메시징이므로 lock을 최대한 없애고, 같은 프레임워크인 만큼
동일한 동시성 메커니즘을 스펙으로 고정한다"**이다.

이 문서는 **초안**이다. 확정되면 `spec/server/01-execution/06-state-ownership-and-lanes`에
반영한다. 근거는 실측 3건이다:

- [executor-layer-survey.ko.md](executor-layer-survey.ko.md) — 4언어 실행기 계층
- [socket-lock-reclassification.ko.md](socket-lock-reclassification.ko.md) — socket lock 371 재분류
- [spot-hotpath-bridge-survey.ko.md](spot-hotpath-bridge-survey.ko.md) — **메시지 1건당 브리지 통과 수**

---

## 0. 이 초안의 초판에서 정정된 것

초판은 조사 전에 쓰여 사실관계가 여럿 틀렸다. 기록해 둔다.

| 초판 서술 | 실측 |
|---|---|
| "java는 Actor별 큐가 없다" | **있다.** `ZLinkActorDispatchSerials.queues` (Actor 계층) |
| "java는 Spot 큐로 다시 넣지 않는다 — 중첩 없음" | **넣는다.** `sharedSpotGate() ? dispatchQueue.enqueue(...)` — dotnet과 동일 구조 |
| "dotnet만 계층별 전용 실행기를 갖췄다" | 개념은 4언어에 있고 **배치만 다르다** |
| "cpp는 java 방식이 구조적으로 불가능" | 언어 제약이 아니다. 세 언어 모두 직렬 실행기를 이미 갖고 있다 |
| "Actor는 순서를 둘 지켜야 해서 큐를 겹쳐 탄다" | 순서가 아니라 **용량 회계** 때문이다(java 주석이 명시) |
| "java 형태 통일 = 진입 경로 217곳 재설계" | **아니다.** 조회를 스냅샷으로 묶는 것이고 규모가 훨씬 작다 |

---

## 1. 무엇을 고정하는가

**"메커니즘"이 아니라 "계약과 소유 관계"를 고정한다.** node는 단일 스레드라 lock 개념이
없으므로 언어 관용구까지 같게 만들 수는 없다. 다음 넷은 이미 공통이거나 공통이어야 한다.

| # | 고정 대상 | 현재 |
|---|---|---|
| 1 | state lane 계약 | **이미 4언어 동일** — 기술만 하면 된다 |
| 2 | 상태 분류 (C1/C2/C3) | 스펙 06 §4에 있음 — 유지 |
| 3 | 실행기 계층 소유 관계 | 개념은 공통, **배치가 갈림** |
| 4 | **turn 경계 — 한 turn에 무엇을 함께 잡는가** | **갈림. 성능 병목의 실체** |

---

## 2. state lane 계약 (확정 — 4언어 실측 동일)

| 계약 | 의미 | dotnet | java | cpp | node |
|---|---|---|---|---|---|
| `current` | 현재 실행 중인 lane | `Current` | `current()` | `current()` | `current` |
| `isOnLane` | 이 lane 위인가 | `IsOnLane` | `isOnLane()` | `is_on_lane()` | `isOnLane` |
| `run` | lane turn에서 실행 | `RunAsync()` | `runAsync()` | `run()` | `run()` |
| `tryPost` | 대기 없이 게시 | `TryPost()` | `tryPost()` | `try_post()` | `tryPost()` |
| **`throwIfReentrant`** | **재진입 시 예외** | `ThrowIfReentrant()` | `throwIfReentrant()` | `throw_if_reentrant()` | `throwIfReentrant()` |
| `close` | 종료 | `DisposeAsync()` | — | `close()` | `closed` |

**R1.** lane이 소유하는 상태는 잠그지 않는다. 컬렉션은 언어의 평범한 컨테이너를 쓴다.
동시성 자료구조로 치환하지 않는다 — C2의 원자성이 컨테이너 단위로 쪼개져 불변식이 깨진다.

**R2.** **재진입은 예외로 실패한다.** `throwIfReentrant`는 선택이 아니라 필수 계약이다.
4언어 모두 이미 갖고 있으므로 새로 만들 것이 없다.

**R3.** lane turn 안에서 외부 await·transport I/O·application callback을 실행하지 않는다.
그런 구간은 상태 보호가 아니라 **작업 프로토콜 직렬화**이므로 별도 gate로 둔다(발견 7).

---

## 3. 상태 분류 (확정 — 스펙 06 §4 유지)

| 분류 | 정의 | 처방 |
|---|---|---|
| **C1** | 순수 조회 레지스트리(교차 불변식 없음) | lane 소유 + 평범한 컨테이너 |
| **C2** | 여러 컬렉션에 걸친 불변식 | **lane 소유. 그룹을 통째로 옮긴다** |
| **C3** | 원자 카운터·플래그 | atomic |

**R4.** C2를 부분만 전환하면 lane과 lock이 같은 불변식을 나눠 지켜 **전환 전보다 나쁘다.**
패스 단위는 "취득 몇 개"가 아니라 "C2 그룹 하나 통째로"다.
(실측: 부분 전환 허용 3패스에 4취득 → 그룹 단위 3패스에 58취득)

**R5.** C2 그룹이 서로 독립이면 **한 클래스가 lane을 여럿 가져도 된다.**
(실증: dotnet `ZLinkManagedMeshNode` lane 5개, cpp `public_host_runtime` lane 6개)

---

## 4. 실행기 계층 소유 관계

### 4.1 실측 — 개념은 공통, 배치가 다르다

| 계층 | dotnet | java | cpp |
|---|---|---|---|
| 범용 직렬 큐 | `ZLinkSerialExecutionQueue` 1,118 | `ZLinkAsyncSerialQueue` 1,682 | `serial_execution_queue` 1,451 |
| Spot 조율자 | `ZLinkSpotSerialExecutor` 1,320 | (없음 — context가 직접) | (없음) |
| Spot 큐 | `_queue` | `dispatchQueue` + `infrastructureQueue` | `serial_queue` |
| Actor별 큐 | `_actorLanes` (Spot 계층) | `queues` (**Actor 계층**) | 이름 맵 |
| Timer별 큐 | `_timerLanes` | `timerQueues` | `timer_lanes` |

**분류 축이 다르다** — dotnet은 *소유자 축*(Spot/Actor/Timer), java는 *성격 축*
(application/infrastructure) + Actor는 별도 계층에서 소유자 축.

### 4.2 실행 모드 — dotnet과 java가 동일하다

| | `PER_ACTOR` | `SPOT_WIDE` |
|---|---|---|
| **Actor** | Actor 큐 | Actor 큐 → **Spot 큐** (2단) |
| **Timer** | Timer 큐 | Spot 큐만 (Timer 큐 건너뜀) |

**두 언어가 독립적으로 같은 비대칭을 구현했다.** 이유는 java 주석이 명시한다:

> *"The Actor queue owns payload admission. The shared Spot gate reserves only its fixed
> turn cost here."*

**Actor 큐가 payload 용량 승인을 소유**하므로 `SPOT_WIDE`에서도 필요하다. Timer는 payload가
없어 용량 회계할 것이 없고, Spot 큐가 이미 전체를 한 줄로 세우므로 Timer 큐가 순수 낭비다.
**직렬화는 두 경로 모두 보장된다** — 겹치는 큐 수만 다르다.

### 4.3 규범

**R6.** Spot·Actor·Timer는 각각 자기 직렬 실행 단위를 갖는다. Spot은 자신의 큐에 더해
**Actor별·Timer별 큐의 수명을 소유**한다.

**R7.** 실행 모드는 **용량 회계 기준으로** 큐 중첩을 결정한다. payload 승인을 소유하는
큐는 상위 큐로 넘길 때도 유지하고, payload가 없는 경로는 상위 큐로 직행한다.

**R8.** 그 소유 관계를 **전용 조율자 하나가 관리한다.** 여러 계층·파일에 흩어 두지 않는다.

### 4.4 확정된 구조 — (A) dotnet 조율 + java primitive (2026-08-28 사용자 확정)

각 언어에서 나은 쪽을 취한다. **조율은 dotnet 형태, primitive 책임은 java 형태.**

**R8-1 조율자 (dotnet 형태를 정본으로)**

Spot마다 전용 조율자를 둔다. 그 조율자가 **세 종류 큐의 수명과 라우팅을 모두 소유**한다.

```
SpotSerialExecutor
  ├ Spot 큐          자기 것
  ├ Actor별 큐 맵     actorId → queue
  ├ Timer별 큐 맵     timerName → queue
  └ 실행 모드         SPOT_WIDE | PER_ACTOR
```

진입점은 계층별로 나눈다 — `executeSpot` / `executeActor(actorId)` / `executeTimer(name)`.
**어느 큐로 보낼지는 조율자가 정하고 호출자는 모른다.**

*근거*: 소유 관계가 타입으로 드러난다. java는 Actor 큐가 다른 계층(`ZLinkActorDispatchSerials`)에
있어 관계가 코드에 안 보이고, 실제로 이 조사에서 **"java에는 Actor별 큐가 없다"고 두 번
오판**했다. 읽는 사람이 틀리는 구조는 스펙이 될 수 없다.

**R8-2 primitive 책임 (java 형태를 정본으로)**

범용 직렬 큐가 **실행뿐 아니라 다음을 계약으로** 갖는다:

| 책임 | 계약 |
|---|---|
| backpressure | 메시지 수 상한 · 바이트 상한 (application / lifecycle 별도) |
| 우선순위 | lifecycle burst limit — lifecycle 작업이 application 뒤에 갇히지 않게 |
| **공정성** | **owner time budget** — 한 소유자가 오래 점유하면 양보 |
| 정책 주입 | 위 값을 정책 객체로 받는다. 하드코딩하지 않는다 |

*근거*: backpressure와 공정성이 호출자에 흩어지면 실시간성이 보장되지 않는다.
dotnet에는 owner time budget 대응물이 **아예 없다**. 고성능 실시간 메시징에서 한 작업이
큐를 무한정 점유할 수 있다는 뜻이다.

**R8-3 용량 회계 분담**

Actor 큐가 **payload admission을 소유**한다. 상위 Spot 큐로 넘길 때는 **고정 turn 비용만**
예약한다. 이중 예약하지 않는다. Timer는 payload가 없으므로 `SPOT_WIDE`에서 Timer 큐를
만들지 않는다(§4.2).

**언어별 작업량**

| 언어 | R8-1 조율자 | R8-2 primitive |
|---|---|---|
| dotnet | **이미 있음** (`ZLinkSpotSerialExecutor`) | 용량·우선순위·**공정성 추가 필요** |
| java | **신설 필요** — Actor 큐를 Spot 조율자로 이관 | **이미 있음** (`ZLinkAsyncSerialQueue`) |
| cpp | **신설 필요** — 이름 맵을 조율자로 | 용량·우선순위·공정성 **추가 필요** |
| node | 조율자 있음(`spot-serial-executor` 314줄) — 큐 맵 보유 여부 확인 | 확인 필요 |

---

## 5. turn 경계 — **성능 병목의 실체**

### 5.1 실측 — 메시지 1건이 통과하는 블로킹 브리지

cpp 기준, handler 호출까지:

| 경로 | `[매번]` 통과 |
|---|---:|
| **원격 Actor request** | **13** |
| **원격 Actor send** | **11** |
| Actor join | 7 |
| Spot 간 send/request | 5 |
| Spot timer (spot-wide) | 2 |
| Spot timer (per-actor) | **1** |

원격 Actor send 한 건이 **노드 상태 lane 9회 + Spot callback lane 2회**를 동기 대기한다.

### 5.2 13곳의 성격 — 대부분 조회다

| 종류 | 수 | 예 |
|---|---:|---|
| **단순 조회** | **7~8** | actor type · callback 묶음 · authority fence · parking RID · instance projection · generation/context projection |
| **중복 조회** | 1 | `:9617`과 `:9707`이 **같은 fence를 두 번** 읽는다 |
| 상태 변경 | 3 | handoff reply 보관 · pending request 증가 · callback depth 증가 |
| 큐 전달 | 1 | Spot queue snapshot (spot-wide) |

**즉 브리지의 다수는 "구조상 필요한 경계"가 아니라 쪼개진 조회다.**

### 5.3 java는 어떻게 다른가

java는 필요한 값을 **한 turn에서 스냅샷으로 묶어 받고 그 뒤로 lane을 기다리지 않는다**:

```java
ActorStateSnapshot state = inStateLane(() -> {
    ZLinkActor actor = actorRegistry.actor(actorId);
    return new ActorStateSnapshot(
        actor,
        actorRegistry.context(actor),       // ← 셋을
        actorRegistry.actorType(actorId));  // ← 한 turn에서 함께
});
```

cpp는 같은 정보를 **조회마다 별도 turn**으로 기다린다.

**따라서 "java 형태로 통일"의 실체는 진입 경로 재설계가 아니라 조회를 스냅샷으로 묶는
것이다.** 그리고 이것은 이미 성문화된 규칙이다 — **발견 10**: *"연속된 read가 하나의 파생
값을 만들 때 각각 별개 turn으로 쪼개면 캡처 블록이 찢어진다."* cpp는 lane 전환 중 이 규칙을
어겼고, java는 지켰다.

### 5.4 규범

**R9.** **한 파생 값을 만드는 read 묶음은 한 turn 안에서 함께 잡는다.** 연속된 조회를
각각 별개 turn으로 쪼개지 않는다. (= 발견 10)

**R10.** 같은 값을 한 메시지 경로에서 **두 번 이상 읽지 않는다.** 첫 조회 결과를
immutable projection으로 들고 다닌다.

**R11.** 상위 직렬 큐가 진입을 소유하는 형태를 택할 수 있다(java `ZLinkSpotRuntime`은
`synchronized` 0 · state lane 0이다). **단, 소유를 전제하는 곳은 반드시 단언한다** —
진입 경계에서 `isOnLane`, 재진입 가능 지점에서 `throwIfReentrant`.
**단언 없는 상위 소유는 금지한다.**

> 실증: java `ZLinkStreamRuntime`의 pending-session이 상위 직렬 소유를 전제했다가 재진입이
> 뚫려 **런타임 데드락**에 빠졌다.
> `getOrCreateSessionState(:1379) → createSessionState(:1478) → getOrCreateSessionState(:1371)`
> lane이었으면 `throwIfReentrant`가 즉시 예외로 잡았을 것이다.

---

## 6. socket 경계 — core와의 분담

### 6.1 core 계약 (`core/doc/guide/11-thread-safety.ko.md`)

| 경로 | core 보장 |
|---|---|
| send/publish/send_rid | **여러 thread 허용**. 단 논리적 multipart sequence를 나누지 않는다 |
| receive | **single-consumer** |
| control(설정·endpoint) | **core가 내부 직렬화** |
| close | lifecycle gate. 충돌 시 `EBUSY` |

### 6.2 재분류 실측 (371개)

| 분류 | java | cpp | dotnet | 합계 |
|---|---:|---:|---:|---:|
| **[중복-제거가능]** | **31** | 0 | 0 | **31** |
| [결함-multipart누적] | 0 | 0 | 0 | **0** |
| [필요-receive] | 5 | 4 | 0 | 9 |
| [필요-프레임워크상태] | 22 | 84 | 33 | 139 |
| [필요-lifecycle] | 35 | 126 | 15 | 176 |
| [판정불가] | 0 | 16 | 0 | 16 |

**"소켓을 감쌌다"가 아니라 대부분 프레임워크 상태와 close gate를 지키고 있었다.**

### 6.3 규범

**R12.** **core가 보장하는 것을 프레임워크가 다시 감싸지 않는다.** send 단일 호출,
option 설정, endpoint operation은 core가 이미 처리한다. 그 위의 lock은 순수 비용이다.

**R13.** **multipart는 한 호출 안에서 조립하고 전송한다.** 조각을 필드에 남겨 다음 호출로
잇지 않는다. 그런 구조는 lock으로 가릴 것이 아니라 **그 자체가 설계 결함**이다.
(실측 0건 — 4언어 전부 단일 호출 로컬 조립이다. 0인 것이 정상이다)

**R14.** 다음은 프레임워크가 지킨다: receive single-consumer · close/dispose와 프레임워크
상태 전이의 원자성 · pending map·핸들러 등록·routing 캐시.

---

## 7. 즉시 조치 대상 (근거가 확정된 것)

| # | 대상 | 규모 | 근거 |
|---|---|---|---|
| 1 | **cpp Spot 핫패스 조회 묶기** | send 11→4~5 · request 13→5~6 | R9·R10. `:9617`/`:9707` 중복 fence, `:9845`/`:10024` projection 통합 |
| 2 | **java binding wrapper 중복 lock 31** | hot path 7 | R12. Dealer는 한 호출에 monitor 2번 중첩 |
| 3 | java `AsyncSerialQueue` 임계 구역 축소 | 할당 4개/enqueue | `BigInteger` 4할당을 `long` 뺄셈으로. 같은 파일 lifecycle 분기가 이미 그 형태 |

**1번이 최우선이다.** 메시지마다 드는 비용이고, 재설계 없이 이미 정해진 규칙(발견 10)을
지키는 것만으로 절반 이상 줄어든다.

---

## 8. 미결 — 사용자 판단 대상

① ~~실행기 계층을 어디까지 고정할 것인가~~ → **확정 (2026-08-28): (A) dotnet 조율 +
   java primitive.** §4.4 R8-1~R8-3 참조. **구현은 별도 세션에서 진행한다.**

② **정본 언어 — 계층별로 나눈다** (①의 귀결):

   | 계층 | 정본 | 근거 |
   |---|---|---|
   | state lane primitive | **.NET** | 4언어 이미 동일 |
   | 실행기 조율자 | **.NET** | R8-1 |
   | 직렬 큐 primitive | **java** | R8-2 — 용량·우선순위·공정성 |
   | turn 경계 (조회 묶기) | **java** | R8-4 = §5 `ActorStateSnapshot` |

   [[reference-first-porting-policy]]의 ".NET 단일 정본"을 **계층별 정본으로 개정**해야 한다.

③ **범위** — Spot·Actor·Session을 함께 볼 것을 권한다. Channel은 성격이 달라(transport) 별도.

④ **호환 경계 회수** — dotnet `AwaitStateLane` 664 · cpp `.get()` 456. §7-1이 끝난 뒤
   남는 수를 다시 재고 판단한다. 지금 전량 회수는 근거가 없다.

⑤ node의 Actor별·Timer별 큐 — 검색으로 못 찾았다. 단일 스레드라 불필요한지 확인 필요.
