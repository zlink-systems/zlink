# 동시성 모델 스펙 — 작업 폴더

**목표**: zlink는 고성능 실시간 메시징 프레임워크다. lock을 최대한 없애고,
같은 프레임워크인 만큼 **동일한 동시성 메커니즘을 스펙으로 고정**한다.
(2026-08-27 사용자 제기)

이 폴더는 그 스펙 작업만 담는다. 앞선 lane 전환 캠페인의 실행 기록은
[`../concurrency-redesign/`](../concurrency-redesign/)에 있고 여기서 섞지 않는다.

---

## 읽는 순서

| # | 문서 | 무엇을 답하는가 |
|---|---|---|
| 1 | **[spec-draft-concurrency-model.ko.md](spec-draft-concurrency-model.ko.md)** | **스펙 초안 본문.** 고정할 규범 R1~R14, 즉시 조치 3건, 미결 |
| 2 | **[executor-naming-contract.ko.md](executor-naming-contract.ko.md)** | **구현 계약서.** 클래스·메서드·필드 이름과 라우팅 규칙 R-N1~R-N14. **구현 세션은 이 문서를 따른다** |
| 3 | **[implementation-plan.ko.md](implementation-plan.ko.md)** | **구현 플랜.** 착수 순서 P0~P5, 언어별 파일과 완료 판정, 실패 방식, 미결 |
| 4 | [executor-layer-survey.ko.md](executor-layer-survey.ko.md) | 4언어 실행기 계층이 어떻게 생겼나 |
| 5 | [spot-hotpath-bridge-survey.ko.md](spot-hotpath-bridge-survey.ko.md) | **메시지 1건이 블로킹 브리지를 몇 번 통과하나** |
| 6 | [socket-lock-reclassification.ko.md](socket-lock-reclassification.ko.md) | socket lock 371개 중 무엇이 중복인가 |

**구현 세션은 1~3을 읽는다.** 계약은 정식 스펙 07이 소유하고, 4~6은 그 근거다.

---

## 확정된 핵심 사실

**state lane 계약은 이미 4언어 동일하다.** `current`·`isOnLane`·`run`·`tryPost`·
`throwIfReentrant`·`close`가 1:1 대응한다. 재진입 검출도 네 언어 모두 갖고 있다 —
새로 만들 것이 없고 **기술만 하면 된다.**

**성능 병목은 lock 개수가 아니라 turn 경계다.** cpp에서 메시지 1건이 블로킹 브리지를
**원격 Actor request 13회 · send 11회** 통과한다. 그중 **7~8개가 단순 조회**이고
1개는 같은 fence를 두 번 읽는 중복이다.

**java는 그 조회를 한 turn에 묶는다.**

```java
ActorStateSnapshot state = inStateLane(() -> new ActorStateSnapshot(
    actorRegistry.actor(actorId),
    actorRegistry.context(actor),
    actorRegistry.actorType(actorId)));   // 셋을 한 turn에서
```

즉 **"java 형태로 통일"의 실체는 진입 경로 재설계가 아니라 조회를 스냅샷으로 묶는 것**이고,
이는 이미 성문화된 **발견 10**을 지키는 일이다. cpp가 lane 전환 중 그 규칙을 어겼다.

**dotnet과 java의 Spot 실행 구조는 거의 같다.** Actor는 `SPOT_WIDE`에서 Actor 큐 → Spot 큐로
2단, Timer는 Spot 큐로 직행 — 양 언어 동일하다. 그 비대칭의 이유는 순서가 아니라
**용량 회계**다(Actor 큐가 payload admission을 소유하고 Timer는 payload가 없다).

---

## 확정 사항 (2026-08-28)

**실행기 구조 = (A) dotnet 조율 + java primitive.** 각 언어에서 나은 쪽을 취한다.
상세는 초안 §4.4 (R8-1 ~ R8-3).

| | 정본 | 내용 |
|---|---|---|
| **조율자** | **dotnet** | Spot마다 전용 조율자가 Spot 큐 · Actor별 큐 · Timer별 큐를 **모두 소유**. 진입점이 큐를 고르고 호출자는 모른다 |
| **큐 primitive** | **java** | 실행뿐 아니라 **backpressure · lifecycle burst · owner time budget · 정책 주입**을 계약으로 |
| 용량 회계 | — | Actor 큐가 payload admission 소유. 상위 Spot 큐는 고정 turn 비용만 예약 (이중 예약 금지) |

**언어별 작업량**

| 언어 | 조율자 | 큐 primitive |
|---|---|---|
| dotnet | 있음 · **Actor·Session 조율자 개명 필요** | **공정성(owner time budget) 추가 필요** |
| java | **3계층 모두 신설** — Actor 큐를 Spot 조율자로 이관 | 이미 있음 |
| cpp | **3계층 모두 신설** — 이름 맵을 조율자로 | 용량·우선순위·공정성 **추가** |
| node | **Spot·Actor 조율자 신설** — 현행 `SpotSerialExecutor`는 직렬 단위다 · Session은 개명 | `BoundedSerialScheduler` 개명·정책 정렬 |

**구현은 별도 세션에서 진행한다.** 계약은 정식 스펙
[07. 직렬 실행기 계층](../../../framework/doc/framework/common/spec/server/01-execution/07-serial-executor-layers.ko.md)이
소유하고, 착수 순서와 언어별 작업은 [implementation-plan.ko.md](implementation-plan.ko.md)에 있다.

---

## 다음 할 일

**착수 순서는 초안 §7-1부터다.**

| # | 작업 | 기대 효과 | 재설계 필요? |
|---|---|---|---|
| 1 | cpp Spot 핫패스 조회 묶기 | send 11→4~5 · request 13→5~6 | **아니오** — 발견 10 준수 |
| 2 | java binding wrapper 중복 lock 31 제거 | hot path 7 | 아니오 |
| 3 | `AsyncSerialQueue` 임계 구역 축소 | enqueue마다 `BigInteger` 4할당 제거 | 아니오 |

이 셋은 **실행기 구조 변경과 독립**이다. 재설계 없이 이미 정해진 규칙을 지키는 것이므로
구조 작업과 병행하거나 먼저 해도 된다.

**node는 성격이 다르다.** JS turn이 원자적이라 turn 경계 문제(§5)도 lock 중복(§6)도 없다.
대신 **메시지당 `Buffer` 복사**가 후보다(CP3 감사 §6에 상위 10 기록). 다만 방어 복사가
소유권 계약일 수 있어 **측정이 선행돼야 한다** — cpp·java와 달리 잘못 걷어내면 데이터 손상이다.

---

## 미결 (사용자 판단)

① ~~실행기 계층~~ → **확정: (A) dotnet 조율 + java primitive** (위 확정 사항)
② 정본 언어 — **계층별로 나눈다**: lane primitive·조율자는 .NET, 큐 primitive·turn 경계는 java.
   [[reference-first-porting-policy]]의 ".NET 단일 정본"을 계층별 정본으로 **개정 필요**
③ ~~범위~~ → **확정: Spot·Actor·Session 셋 함께.** Channel은 transport 성격이라 별도.
   단 셋의 소유 구조가 다르다 — **Spot만 큐 맵을 갖고 Actor·Session은 인스턴스당 큐 하나**다(계약서 §1.5 R-N1a). Spot 형태를 기계적으로 복사하지 않는다
④ 호환 경계 회수(dotnet 664 · cpp 456) — §7-1 이후 재측정
⑤ ~~node의 Actor별·Timer별 큐~~ → **정정: 있다.** `actorSerials`·`timerSerials`·
   `ZLinkActorDispatchMailboxSet` 세 맵이 조율자 밖에 흩어져 있다. `await`가 turn을 양보하므로
   Actor별 직렬 단위는 node에서도 필요하다. 그 맵을 담을 Spot 조율자도 node에는 없다 —
   현행 `ZLinkSpotSerialExecutor`는 직렬 단위 wrapper다(계약서 §7 · 플랜 §6)
⑥ ~~`SpotWide`에서 Actor 큐를 거칠 이유~~ → **해소: 계약이 이미 정하고 있었다.** 스펙 02 §3의
   `Yield` claim — `SpotWide` member Actor가 `Yield`하면 gate만 반납하고 Actor queue claim은
   유지한다. 02 §1은 queue를 합치는 것을 잘못된 구조로 명시한다. 2단 유지
⑦ ~~owner queue byte 계상~~ → **해소: 잔재가 아니라 현행 계약이다.** Framework API §11이
   건수·byte 두 축을 강제하고("건수만 두면 같은 건수가 payload 크기에 따라 수천 배의 memory를
   점유한다"), 네 언어 exact interface에 `mailboxByteBudget`이 있다. dotnet 실행 큐에만 없어
   어디서 만족하는지 조사 필요(플랜 P0-4)
