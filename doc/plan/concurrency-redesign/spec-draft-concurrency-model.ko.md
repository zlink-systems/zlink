# 스펙 초안 — 동시성 모델 고정 (4언어 공통)

작성: 2026-08-27. 사용자 목표는 **"고성능 실시간 메시징이므로 lock을 최대한 없애고,
같은 프레임워크인 만큼 동일한 동시성 메커니즘을 스펙으로 고정한다"**(rules §9)이다.

이 문서는 **초안**이다. 확정되면 `spec/server/01-execution/06-state-ownership-and-lanes`에
반영한다. 미결 항목(§7)은 사용자 판단 대상이다.

근거는 두 실측 문서다:
- [executor-layer-survey.ko.md](executor-layer-survey.ko.md) — 4언어 실행기 계층 실측
- [socket-lock-reclassification.ko.md](socket-lock-reclassification.ko.md) — socket lock 371 재분류

---

## 1. 무엇을 고정하는가

**"메커니즘"이 아니라 "계약과 소유 관계"를 고정한다.** 4언어의 언어 관용구는 다를 수밖에
없지만(node는 단일 스레드라 lock 개념 자체가 없다), 다음 넷은 이미 공통이거나 공통이어야 한다.

| # | 고정 대상 | 현재 상태 |
|---|---|---|
| 1 | state lane 계약 | **이미 4언어 동일** — 기술만 하면 된다 |
| 2 | 실행기 계층 소유 관계 | 개념은 4언어에 있으나 **배치가 갈림** |
| 3 | 진입 소유와 자기 강제 | **갈림** — java는 단언 없이 상위 소유에 의존하다 데드락 발생 |
| 4 | 금지 형태 | 스펙 06 §3에 있음 — socket 계약 대조 결과를 추가 |

---

## 2. state lane 계약 (확정 — 4언어 실측 동일)

모든 언어의 state lane은 다음을 제공한다.

| 계약 | 의미 | dotnet | java | cpp | node |
|---|---|---|---|---|---|
| `current` | 현재 실행 중인 lane | `Current` | `current()` | `current()` | `current` |
| `isOnLane` | 이 lane 위인가 | `IsOnLane` | `isOnLane()` | `is_on_lane()` | `isOnLane` |
| `run` | lane turn에서 실행 | `RunAsync()` | `runAsync()` | `run()` | `run()` |
| `tryPost` | 대기 없이 게시 | `TryPost()` | `tryPost()` | `try_post()` | `tryPost()` |
| **`throwIfReentrant`** | **재진입 시 예외** | `ThrowIfReentrant()` | `throwIfReentrant()` | `throw_if_reentrant()` | `throwIfReentrant()` |
| `close` | 종료 | `DisposeAsync()` | — | `close()` | `closed` |

**규범**

- **R1.** lane이 소유하는 상태는 잠그지 않는다. 컬렉션은 언어의 평범한 컨테이너를 쓴다
  (`Dictionary`/`HashMap`/`std::map`). 동시성 자료구조로 치환하지 않는다.
- **R2.** **재진입은 예외로 실패한다. 조용히 통과시키지 않는다.** `throwIfReentrant`는
  선택이 아니라 필수 계약이다.
- **R3.** lane turn 안에서 외부 await·transport I/O·application callback을 실행하지 않는다
  (발견 7). 그런 구간은 상태 보호가 아니라 작업 프로토콜 직렬화이므로 별도 gate로 둔다.
- **R4.** 한 파생 값을 만드는 read 묶음은 **한 turn 안에서 함께** 잡는다(발견 10).
  연속된 동기 read를 각각 별개 turn으로 쪼개면 캡처 블록이 찢어진다.

---

## 3. 상태 분류 (확정 — 스펙 06 §4 유지)

| 분류 | 정의 | 처방 |
|---|---|---|
| **C1** | 순수 조회 레지스트리(map 하나, 교차 불변식 없음) | lane 소유 + 평범한 컨테이너 |
| **C2** | 여러 컬렉션에 걸친 불변식 | **lane 소유. 그룹을 통째로 옮긴다** |
| **C3** | 원자 카운터·플래그 | atomic |

**R5.** C2를 부분만 전환하면 lane과 lock이 같은 불변식을 나눠 지켜 **전환 전보다 나쁘다.**
패스 단위는 "취득 몇 개"가 아니라 "C2 그룹 하나 통째로"다.
(실측: 부분 전환 허용 3패스에 4취득 → 그룹 단위 3패스에 58취득)

**R6.** C2 그룹이 서로 독립이면 **한 클래스가 lane을 여럿 가져도 된다.** design §3의
"경계를 새로 긋지 않는다"는 클래스 분할 금지이지 다중 lane 금지가 아니다.
(실증: dotnet `ZLinkManagedMeshNode` lane 5개, cpp `public_host_runtime` lane 6개)

---

## 4. 실행기 계층 소유 관계

### 4.1 실측 (executor-layer-survey §3)

| 계층 | dotnet | java | cpp | node |
|---|---|---|---|---|
| 범용 직렬 큐 | `ZLinkSerialExecutionQueue` 1,118 | `ZLinkAsyncSerialQueue` 1,682 | `serial_execution_queue` 1,451 | `serial-scheduler` 374 |
| Spot 전용 | `ZLinkSpotSerialExecutor` **1,320** | (범용 직접) | (범용 직접) | `spot-serial-executor` 314 |
| Actor 전용 | `ZLinkActorDispatchMailbox` 341 | — | — | — |
| Session 전용 | `ZLinkStreamSessionSerialExecutor` 111 | — | `stream_runtime` 안 | `session-serial-executor` |

**dotnet만 계층별 전용 실행기를 갖췄다.** 그리고 이는 중복이 아니다 —
`SpotSerialExecutor`는 범용 큐의 래퍼가 아니라 **여러 큐의 수명·라우팅을 관리하는 계층**이다:

```
Spot 1개 = 자기 큐 1 + Actor별 큐 N + Timer별 큐 M
```

같은 개념이 다른 언어에도 있으나 **흩어져 있다** — java `actorLanes`는
`ZLinkUserSpotRetireRuntime`에, cpp `timer_lanes`는 `spot_runtime.cpp` 안에 있다.

### 4.2 규범 (초안)

- **R7.** Spot·Actor·Session·Timer는 각각 **자기 직렬 실행 단위를 소유한다.**
  Spot은 자신의 큐에 더해 **Actor별·Timer별 큐의 수명을 소유**한다.
- **R8.** 그 소유 관계를 **한 곳에서 관리한다.** 여러 파일에 흩어 두지 않는다.
  (클래스로 뺄지 런타임 안에 둘지는 §7-①의 판단에 달렸다)

---

## 5. 진입 소유와 자기 강제

### 5.1 문제

java `ZLinkSpotRuntime`(5,284줄)은 `synchronized` 0 · state lane 0이다. 상위
`ZLinkAsyncSerialQueue`가 직렬 소유하므로 내부 동기화가 없다. **가장 깨끗한 형태다.**

그러나 이번 세션에 **그 형태의 실패가 실증됐다.** `ZLinkStreamRuntime`의 pending-session이
creator가 자기 pending future를 `join()`해 **런타임 데드락**에 빠졌다(스택 실증):

```
getOrCreateSessionState(:1379) → createSessionState(:1478) → getOrCreateSessionState(:1371)
```

상위 직렬 소유를 **전제**했는데 재진입이 뚫렸고, 검사하는 주체가 없어 조용히 데드락이 됐다.
lane이었으면 `throwIfReentrant`가 즉시 예외로 잡았을 것이다.

### 5.2 규범 (초안)

- **R9.** 상위 직렬 큐가 진입을 소유하는 형태를 택할 수 있다. 그 경우 클래스 내부 동기화는 0이다.
- **R10.** **단, 소유를 전제하는 곳은 반드시 단언한다.** 컴포넌트 진입 경계에서
  `isOnLane`으로 소유를 확인하고, 재진입 가능 지점에서 `throwIfReentrant`로 즉시 실패시킨다.
  **단언 없는 상위 소유는 금지한다** — java pending-session 데드락이 그 실패다.
- **R11.** 단언은 디버그 빌드 필수, 릴리스 빌드는 선택으로 둘 수 있다. 다만 재진입이
  실재하는 컴포넌트(cpp `recursive_mutex` 사용 이력이 있던 곳)는 릴리스에서도 유지한다.

---

## 6. socket 경계 — core 계약과의 분담

### 6.1 core 계약 (`core/doc/guide/11-thread-safety.ko.md`)

| 경로 | core 보장 |
|---|---|
| send/publish/send_rid | **여러 thread 허용**. 단 논리적 multipart sequence를 나누지 않는다 |
| receive | **single-consumer** |
| control(설정·endpoint) | **core가 내부 직렬화** |
| close | lifecycle gate. 충돌 시 `EBUSY` |
| callback | receive callback은 그 socket의 I/O thread에서 실행 |

### 6.2 재분류 실측 (socket-lock-reclassification §1)

| 분류 | java | cpp | dotnet | 합계 |
|---|---:|---:|---:|---:|
| **[중복-제거가능]** | **31** | 0 | 0 | **31** |
| [결함-multipart누적] | 0 | 0 | 0 | **0** |
| [필요-receive] | 5 | 4 | 0 | 9 |
| [필요-프레임워크상태] | 22 | 84 | 33 | 139 |
| [필요-lifecycle] | 35 | 126 | 15 | 176 |
| [판정불가] | 0 | 16 | 0 | 16 |

**multipart 누적 구조는 0건이다.** 4언어 전부 한 호출 안에서 조립·submit을 끝낸다.
header/body 조각을 필드에 보관해 다음 호출로 잇는 구조는 없다. **있으면 그 자체가 설계
결함이므로, 0건인 것이 정상이다.**

### 6.3 규범 (초안)

- **R12.** **core가 보장하는 것을 프레임워크가 다시 감싸지 않는다.**
  `send` 단일 호출, option 설정, endpoint operation은 core가 이미 직렬화하거나 다중 호출을
  허용한다. 그 위의 lock은 순수 비용이다.
- **R13.** **multipart는 한 호출 안에서 조립하고 전송한다.** 조각을 필드에 남겨 다음 호출로
  잇지 않는다. 그런 구조가 필요해 보이면 설계를 고친다 — lock으로 가리지 않는다.
- **R14.** 다음은 프레임워크가 지킨다(core가 안 해 준다):
  receive single-consumer · close/dispose와 프레임워크 상태 전이의 원자성 ·
  pending map·핸들러 등록·routing 캐시 같은 프레임워크 상태.

### 6.4 즉시 조치 대상

java binding wrapper **31개**가 R12 위반이다. 그중 **hot path 7개**:

| 위치 | 수 | 성격 |
|---|---:|---|
| `ZLinkJavaDealerSocket` send/request | 4 | **한 호출에 monitor 2번 중첩** |
| `ZLinkJavaRouterSocket` send/request/reply | 3 | message submit마다 monitor 진입 |
| control(bind/connect/option/subscription) | 24 | 저빈도 |

message submit마다 드는 비용이라 **고성능 실시간 목표에 직접 닿는다.**

---

## 7. 미결 — 사용자 판단 대상

① **실행기 계층을 어디까지 고정할 것인가**
   - (A) dotnet 형태(계층별 전용 클래스)를 정본으로. java·cpp·node에 도입 — 비용 큼
   - (C) 소유 관계만 고정하고 클래스 분할은 언어 재량 — 지금 네 구현을 다 살림
   - (B) 범용 큐 직접 사용으로 통일 — **권장하지 않음.** 관리 책임이 다시 흩어진다

② **정본 언어** — lane primitive는 .NET 정본이 유효하다(4언어 동일). 실행기 계층은
   dotnet만 갖췄으므로 ①에서 (A)를 택하면 그대로 정본, (C)면 정본 개념이 약해진다.

③ **범위** — Spot만인가, Actor·Session·Channel까지인가. 일부만 바꾸면 계층 간 소유 형태가
   또 갈린다.

④ **브리지 회수** — 상위 직렬 소유로 완전히 가면 호환 경계(dotnet `AwaitStateLane` 664 ·
   cpp `.get()` 456)가 불필요해진다. 이것을 이번에 함께 없앨지, 별도 캠페인으로 둘지.

⑤ node의 Actor별·Timer별 큐 — 검색으로 못 찾았다. 단일 스레드라 불필요한지 확인 필요.
