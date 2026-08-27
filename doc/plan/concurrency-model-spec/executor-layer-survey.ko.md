# 실행기 계층 실측 — 4언어 동시성 메커니즘 조사

작성: 2026-08-27. 목적은 **"동시성 메커니즘을 스펙으로 고정한다"**는 사용자 목표(rules §9)의
근거를 만드는 것이다. 지금 4언어가 실제로 어떻게 설계돼 있고, 무엇이 이미 같고 무엇이
갈라져 있는지를 실측으로 확정한다.

## 1. 결론

**아래층(state lane)은 이미 4언어 동일하다. 갈라진 것은 위층(직렬 실행기)의 배치다.**

그리고 갈라짐의 성격이 처음 추정과 달랐다. dotnet이 "불필요한 전용 클래스를 더 만든 것"이
아니라 **dotnet만 그 책임을 한 클래스로 모았고 나머지는 흩어져 있다.**

## 2. state lane primitive — 4언어 계약 동일

| 계약 | dotnet | java | cpp | node |
|---|---|---|---|---|
| 현재 lane 조회 | `Current` | `current()` | `current()` | `current` |
| 소속 확인 | `IsOnLane` | `isOnLane()` | `is_on_lane()` | `isOnLane` |
| 실행 | `RunAsync()` | `runAsync()` | `run()` | `run()` |
| 비대기 게시 | `TryPost()` | `tryPost()` | `try_post()` | `tryPost()` |
| **재진입 검출** | `ThrowIfReentrant()` | `throwIfReentrant()` | `throw_if_reentrant()` | `throwIfReentrant()` |
| 종료 | `DisposeAsync()` | — | `close()` | `closed` |

| 언어 | 파일 | 줄 |
|---|---|---:|
| dotnet | `Runtime/Execution/ZLinkStateLane.cs` | 184 |
| java | `runtime/internal/execution/ZLinkStateLane.java` | 223 |
| cpp | `runtime/execution/state_lane.{hpp,cpp}` | 110+166 |
| node | `runtime/execution/state-lane.ts` | 116 |

**재진입 검출이 4언어 모두에 있다.** 즉 "자기 강제"는 이미 primitive 수준에 구현돼 있고,
새로 만들 것이 없다. 정본-우선 포팅([[reference-first-porting-policy]])이 이 계층에서는
제대로 작동했다.

## 3. 직렬 실행기 — 계층별 배치가 갈린다

### 3.1 범용 직렬 큐 (4언어 모두 보유)

| 언어 | 클래스 | 줄 |
|---|---|---:|
| dotnet | `Runtime/Execution/ZLinkSerialExecutionQueue.cs` | 1,118 |
| java | `execution/ZLinkAsyncSerialQueue.java` | 1,682 |
| cpp | `runtime/execution/serial_execution_queue.{hpp,cpp}` | 320+1,131 |
| node | `runtime/execution/serial-scheduler.ts` | 374 |

java 것은 capacity·byte budget·lifecycle burst·owner time budget 같은 정책을 직접 들고 있어
가장 크다(상수 7개가 public).

### 3.2 계층별 전용 실행기 — **dotnet만 갖췄다**

| 계층 | dotnet | java | cpp | node |
|---|---|---|---|---|
| Spot | `ZLinkSpotSerialExecutor` **1,320** | 범용 큐 직접 사용 | 범용 큐 직접 사용 | `spot-serial-executor.ts` 314 |
| Actor | `ZLinkActorDispatchMailbox` 341 | — | — | — |
| Stream Session | `ZLinkStreamSessionSerialExecutor` 111 | — | `stream_runtime` 안 | `session-serial-executor.ts` |
| Timer | — | — | `timer_runtime` 안 | — |

### 3.3 Spot 상태를 실제로 누가 지키나

| 언어 | 파일 | 동기화 | state lane |
|---|---|---:|---:|
| java | `ZLinkSpotRuntime.java` (5,284줄) | `synchronized` **0** | **0** — 상위 큐 참조 12 |
| dotnet | `ZLinkSpotNodeCatalog.cs` | `lock` **0** | **10** |
| cpp | `spot_runtime.cpp` | 취득 34(실행 primitive 31) | 26 |
| node | — | (JS turn 원자성) | — |

## 3.4 정정 (2026-08-27, 추가 조사)

이 문서 초판의 다음 서술은 **틀렸다**. `runtime/spots/` 아래만 조사한 결과였다.

- ~~"java는 Actor별 큐가 없다"~~ → **있다.** `runtime/actors/ZLinkActorDispatchSerials.java`의
  `Map<String, ZLinkAsyncSerialQueue> queues`. 맵은 `ZLinkStateLane`이 소유한다.
- ~~"java는 Spot 큐로 다시 넣지 않는다"~~ → **넣는다.**
  `ZLinkDefaultSpotContext`가 `sharedSpotGate() ? dispatchQueue.enqueue(...) : ...`로
  dotnet `_actorLanes → _queue`와 **동일한 2단 구조**를 만든다.
- Timer 비대칭도 **양 언어 동일**하다. `SPOT_WIDE`면 Timer 큐를 건너뛰고 Spot 큐로 직행한다.
  이유는 순서가 아니라 **용량 회계**다 — java 주석: *"The Actor queue owns payload admission.
  The shared Spot gate reserves only its fixed turn cost here."* Timer는 payload가 없다.

**즉 dotnet과 java의 Spot 실행 구조는 거의 같다.** 남는 차이는 ①조율 위치(dotnet 한 곳,
java 두 계층) ②큐 맵 보호(dotnet `_laneGate` lock, java state lane) ③용량 분담 명시 여부다.

## 4. 처음 추정이 틀린 지점 (기록)

**추정**: "java는 상위 큐가 소유해서 내부 동기화 0이고, dotnet은 전용 실행기를 만들어
중복이 생겼다. 범용 큐 직접 사용으로 통일하면 된다."

**실측**: 틀렸다. dotnet `SpotSerialExecutor`는 범용 큐를 감싸는 래퍼가 아니라
**여러 큐의 수명과 라우팅을 관리하는 계층**이다.

```csharp
private readonly ZLinkSerialExecutionQueue _queue;                                        // Spot 자신
private readonly Dictionary<ZLinkActorId, ZLinkSerialExecutionQueue> _actorLanes = [];    // Actor별
private readonly Dictionary<ZLinkTimerName, ZLinkSerialExecutionQueue> _timerLanes = [];  // Timer별
private readonly ZLinkUserSpotExecutionMode _executionMode;                               // 실행 모드 정책
```

즉 **"Spot 1개 = 큐 1개"가 아니라 "Spot 1 + Actor N + Timer M = 큐 1+N+M개"**이고,
`ExecuteActorAsync`·`ExecuteTimerAsync`·`ExecuteRelocationActorAsync`·`ExecuteQuiescentLifecycleAsync`
같은 계층 고유 진입점을 제공한다.

그리고 **같은 개념이 다른 언어에도 있다 — 위치만 다르다**:

| 언어 | Actor별 큐 | Timer별 큐 |
|---|---|---|
| dotnet | `_actorLanes` (SpotSerialExecutor) | `_timerLanes` (SpotSerialExecutor) |
| java | `actorLanes` (`ZLinkUserSpotRetireRuntime` — retire 전용 런타임) | — |
| cpp | — | `timer_lanes` (`spot_runtime.cpp` 안에 흩어짐) |
| node | (미발견) | (미발견) |

**따라서 "dotnet 전용 실행기를 걷어내고 범용 큐로 통일한다"는 방향은 틀렸다.**
걷어내면 그 관리 책임이 다시 흩어진다. dotnet이 더 정리된 형태다.

## 5. 통일 방향 후보

### (A) 계층별 전용 실행기로 통일 — dotnet 형태를 정본으로

- java·cpp·node에 Spot/Actor/Session 전용 실행기를 도입하고, 지금 흩어진 Actor별·Timer별
  큐 관리를 그리로 모은다.
- 장점: 계층별 정책(실행 모드·relocation 예약·quiescent lifecycle)을 명시적으로 표현한다.
  dotnet이 이미 증명한 형태다.
- 비용: 세 언어에 새 계층을 추가한다. java `ZLinkSpotRuntime` 5,284줄이 영향을 받는다.

### (B) 범용 큐 직접 사용으로 통일 — java·cpp 형태를 정본으로

- dotnet의 전용 실행기 3개를 걷어낸다.
- **§4에서 확인했듯 이 방향은 책임을 다시 흩뜨린다. 권장하지 않는다.**

### (C) 계층 구조만 스펙으로 고정하고 클래스 분할은 언어 재량

- "Spot은 자신의 큐 + Actor별 큐 + Timer별 큐를 소유한다"는 **소유 관계**를 고정하고,
  그것을 전용 클래스로 뺄지 런타임 안에 둘지는 언어가 정한다.
- 장점: 지금 동작하는 네 구현을 다 살리면서 소유 관계는 통일된다.
- 단점: "동일 메커니즘"이라기엔 느슨하다.

## 6. 스펙에 고정할 대상 (초안)

스펙 06에 §9「언어별 매핑」이 이미 있으므로 그 절을 확장한다.

1. **state lane 계약** — §2 표가 그대로 스펙이 된다. 이미 4언어 동일하므로 기술만 하면 된다.
   `throwIfReentrant`를 **필수 계약**으로 명시한다(자기 강제).
2. **실행기 계층 구조** — Spot/Actor/Session/Timer 각각이 자기 직렬 실행 단위를 갖는다는
   소유 관계. (A)/(C) 중 무엇을 택하느냐에 따라 클래스 분할까지 고정할지 결정된다.
3. **진입 소유** — 상위 직렬 큐가 진입을 소유하되, 경계에서 `isOnLane`/`throwIfReentrant`로
   단언한다. java `ZLinkStreamRuntime` pending-session 데드락이 단언 없는 소유의 실패 사례다
   (2026-08-27 되돌림 검증에서 스택으로 실증).
4. **금지** — lane 안 외부 await·transport I/O(발견 7), 브리지 신규 생성.

## 7. 미결

- **(A)/(C) 선택** — 사용자 판단 대상.
- **정본 언어** — lane primitive는 .NET 정본이 유효하다(4언어 동일). 실행기 계층은
  dotnet이 유일하게 갖췄으므로 (A)를 택하면 그대로 정본, (C)면 정본 개념이 약해진다.
- node의 Actor별·Timer별 큐 — 검색으로 못 찾았다. 단일 스레드라 불필요한지, 다른 이름인지
  확인이 필요하다.
- **범위** — Spot만인지 Actor·Session·Channel까지인지. 계층 일부만 바꾸면 소유 형태가 또 갈린다.
