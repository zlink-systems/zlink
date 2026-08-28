# 구현 플랜 — 직렬 실행기 계층 정렬

[작업 폴더 목차](README.ko.md) · [계약: 스펙 07](../../../framework/doc/framework/common/spec/server/01-execution/07-serial-executor-layers.ko.md) · [이름 계약서](executor-naming-contract.ko.md)

이 문서는 **네 언어 runtime의 실행기 코드를 스펙 07에 맞추는 순서**를 정한다. 무엇이 옳은지는
스펙 07이 소유한다 — 이 문서는 그것을 여기에 다시 적지 않고, 어떤 순서로 어느 파일을 고치고
무엇으로 완료를 판정하는지만 담는다.

구현 세션이 읽을 것은 셋이다.

| # | 문서 | 역할 |
|---|---|---|
| 1 | [스펙 07 직렬 실행기 계층](../../../framework/doc/framework/common/spec/server/01-execution/07-serial-executor-layers.ko.md) | **계약.** 코드가 여기에 맞춰진다 |
| 2 | [스펙 06 상태 소유와 state lane](../../../framework/doc/framework/common/spec/server/01-execution/06-state-ownership-and-lanes.ko.md) | queue map을 무엇으로 지키는가(C1·C2 판별) |
| 3 | [executor-naming-contract.ko.md](executor-naming-contract.ko.md) | 현행 코드 실측과 언어별 대비 작업 |

---

## 0. 새 세션 부트스트랩 (2026-08-28 인계)

이 캠페인은 **구현 세션**이다. 계약은 확정돼 있고 여기서 다시 정하지 않는다 — 스펙과
어긋나 보이면 코드를 스펙에 맞추고, 스펙 자체가 틀렸다고 판단되면 **멈추고 사용자에게
에스컬레이션**한다([[spec-change-policy]] — 오류·개선만 허용, 구현 편의 완화 금지).

### 0.1 읽기 순서 — 소유 문서 먼저

직전 세션이 소유 문서를 안 읽고 구현 코드에서 계약을 추론하다 Critical 2건을 만들고
되돌렸다. 같은 실수를 반복하지 않으려면 **판단 전에 아래를 이 순서로 정독한다.**

| # | 문서 | 소유하는 것 |
|---|---|---|
| 1 | `framework/doc/framework/common/spec/server/01-execution/07-serial-executor-layers.ko.md` | **이 캠페인의 계약** — 계층·진입점·queue 경로·primitive·turn 구동 |
| 2 | 같은 트리 `06-state-ownership-and-lanes.ko.md` | state lane 계약, C1/C2/C3 판별 |
| 3 | 같은 트리 `04-application-job-queue-and-backpressure.ko.md` §1·§3·**§8「Owner 예약의 이관」** | permit 순서, owner FIFO, **예약 이관(2026-08-28 신설)** |
| 4 | 같은 트리 `02-handler-turn-and-execution-gate.ko.md` §1·§3·§7·§10 | queue/gate 분리, `Yield` claim, lane 기본값, 공유 실행 자원 |
| 5 | `00-foundation/06-framework-api.ko.md` §11 | mailbox 두 축·반환 시점·scheduler |
| 6 | [executor-naming-contract.ko.md](executor-naming-contract.ko.md) → 이 플랜 §1~§9 | 이름 계약 실측, 작업 순서 |

### 0.2 진행 방식 — 역할 분담 (2026-08-28 사용자 지정)

| 역할 | 담당 |
|---|---|
| 감독·판정·**기본 리뷰**·스펙 수정 | **Claude 감독관 본체** (위임 금지 — 특히 스펙 문장은 에이전트가 쓰지 않는다) |
| 작업(구현·조사) | **codex 서브에이전트** — `gpt-5.6-terra`(구현 기본) · `gpt-5.6-luna`(구현 — 감독관이 난이도·가용성으로 선택) · `gpt-5.6-sol`(조사) |
| **최종 리뷰** (P 단계 마감마다) | **codex sol** |

- 기동은 직접 실행 경로를 쓴다(플러그인 sandbox는 TCP bind·cmake를 막는다):
  `codex exec -m gpt-5.6-terra -c model_reasoning_effort="high" -s danger-full-access --skip-git-repo-check "<프롬프트>" < /dev/null` 을 Bash run_in_background로.
  **`< /dev/null` 필수**(stdin 파이프면 무한 대기), 기동 직후 로그 수백 B 성장 검증(좀비 방지).
- **3분 주기로 출력을 읽어 진행을 확인한다** — 프로세스 생존 확인은 진행 확인이 아니다.
  3분간 로그 성장 0이면 좀비로 판정하고 재기동한다(직전 세션에서 2시간 좀비 실증).
- 에이전트 프롬프트에 반드시 넣는다: 대상 스펙 절 인용, 동작 보존, git reset/stash 금지,
  커밋 금지(검토·커밋은 감독관), 모호하면 [의심]으로 보고하고 판정은 감독관.
- 에이전트 결과는 **감독관이 diff를 직접 읽고 재검증**한 뒤에만 커밋한다(맹신 금지).
- **적절한 시점마다 커밋·푸시한다** (2026-08-28 사용자 지정) — 항목(P*-N) 단위 검증 통과가
  그 시점이다. `git add`는 파일 명시 목록으로만, push 전 fetch+분기 확인.
- **진행하면서 리팩토링을 함께 한다** (2026-08-28 사용자 지정) — 손대는 파일에서
  ① `doc/principal/dev/posddd.ko.md` 원칙 위반, ② 성능 병목(할당·복사·경합),
  ③ 불필요한 코드를 발견하면 정리한다. 단 **동작 보존 전환과 리팩토링은 커밋을 분리**한다
  — 한 커밋에 섞으면 되돌림 검증이 불가능해진다.

### 0.3 규율 — 직전 세션 교훈 (위반이 실제 사고를 냈다)

1. **계약 판정은 소유 문서 인용으로만.** 구현 코드에서 계약을 추론하지 않는다.
2. **"없다/잔재다" 판정은 저장소 전체 검색으로만.** 쓴 검색 명령을 근거에 남긴다
   (한 파일 grep 판정으로 네 번 틀렸다 — node claim wrapper, cpp 정책 등).
3. **지적받으면 재추론 전에 증거부터 넓힌다** — 안 읽은 소유 문서 후보를 나열하고 읽는다.
4. **스펙을 고치는 대형 판단은 반영 전 codex sol 리뷰를 거친다.**
5. git: `git add`는 파일 명시 목록으로만(-A·디렉터리 add 금지), push 전 fetch+분기 확인.
6. 같은 빌드 트리에 작업 2개 금지(유령 실패). P2·P3·P4는 트리가 달라 병렬 가능.

### 0.4 게이트와 기준선 (0.14.0, 2026-08-28 실측)

bindings 0.14.0 전환 완료(릴리스·로컬 패키지·참조 모두). 아래가 그 위의 그린 기준선이다.

| 언어 | 게이트 명령 (트리 루트: `framework/languages/<lang>`) | 기준선 |
|---|---|---|
| dotnet | `dotnet test tests/Zlink.Framework.UnitTests -c Release` | **1901/1901** |
| java | `./gradlew cleanTest test` (`cleanTest` 필수 — UP-TO-DATE 함정) | 전체 그린. full-run 한정 flake: `ZLinkMicrometerMetricSinkTest`·`RawMeshNodeM6ATest`·`AsyncSerialQueueTest…YieldRegistration` — 단독 재실행으로 판정 |
| cpp | `cmake --build build -j 14` 후 `ctest --test-dir build -L 'framework-(unit\|contract)' -LE 'e2e\|sample\|perf'` | **44/45** — `layout_contract` 1건은 기존 샘플 결함(`OrderWorkflow` blocking `result()`). exit 86/134는 1회 재실행 |
| node | `npx tsc -b tsconfig.build.json` 후 `npm test` | contract 기존 실패 23건 등재(`../concurrency-redesign/rules.ko.md` §4) — 회귀 판정은 실패 이름 집합 대조로. lint 157건(0.14.0 타입 적응)은 별도 수정 진행 중 |

### 0.5 진행표 — 새 세션이 이 표를 갱신한다

| 항목 | 상태 | 비고 |
|---|---|---|
| P0-1 cpp 조회 묶기 | 대기 | |
| P0-2 java wrapper lock | 대기 | |
| P0-3 java BigInteger | 대기 | |
| P0-4 두 축 회계 조사 | **완료** | §2.1 — 판정은 04 §8로 스펙 확정, 잔여는 P1-6·P2-6·P3-6·P4-8 |
| P1 dotnet (P1-1~P1-6) | 대기 | 착수 전 dotnet 세션 소유 확인(§3) |
| P2 java (P2-1~P2-7) | 대기 | P1~P4 병렬 |
| P3 cpp (P3-1~P3-6) | 대기 | P1~P4 병렬 |
| P4 node (P4-1~P4-8) | 대기 | P1~P4 병렬 |
| P5 계약 test | 대기 | P1~P4 뒤 |
| 마감 게이트 (unit·cross-language e2e·샘플 6종 ZoneWorld 제외) | 대기 | §1.1 |

---

## 1. 착수 순서 — 언어별 병렬 (2026-08-28 사용자 지정)

**P1~P4는 언어별로 병렬로 진행한다.** 트리가 서로 다르고(각 `framework/languages/<lang>`),
이름·라우팅은 [executor-naming-contract.ko.md](executor-naming-contract.ko.md)와 스펙 07에
이미 고정돼 있어 서로를 기다릴 이유가 없다 — P2·P3의 참조 구현은 "P1이 끝난 dotnet"이
아니라 **현행 dotnet 구조 + 스펙 07**이다(개명 전 이름이어도 구조 참조에는 지장 없다).

| 단계 | 내용 | 병렬성 |
|---|---|---|
| **P0** | 구조와 독립인 선행 작업(§2) — 대상 언어별로 해당 P와 같은 에이전트에 묶어도 된다 | P1~P4와 병행 |
| **P1 dotnet · P2 java · P3 cpp · P4 node** | 언어별 정렬 작업(§3~§6) | **4개 동시** — 언어당 codex 에이전트 1개, 같은 빌드 트리에 2개 금지 |
| **P5** | 스펙 07 §10 → 언어별 계약 test | P1~P4 뒤, 언어별 병렬 |

단 P1 착수 전 **dotnet 세션 소유 확인**(§3)만 예외로 먼저 한다 — 다른 세션이 같은 파일을
잡고 있으면 dotnet만 뒤로 미루고 나머지 셋을 먼저 돌린다.

### 1.1 마감 게이트 — 전 단계 완료 후 (2026-08-28 사용자 지정)

P1~P5가 끝나면 다음 세 가지를 이 순서로 확인해야 캠페인이 끝난다.

1. **언어별 unit test 전부** — §0.4의 게이트·기준선으로 판정
2. **cross-language e2e**
3. **샘플 6종 동작 확인 — ZoneWorld 제외**

명령과 판정 기준은 [`../concurrency-redesign/rules.ko.md`](../concurrency-redesign/rules.ko.md)
§4 게이트 매트릭스를 그대로 쓴다(알려진 기존 실패 목록 포함). 샘플·harness 실행은 codex
sandbox가 loopback bind를 막으므로 **감독관이 직접 실행**한다(§0.2).

## 2. P0 — 구조와 독립인 선행 작업

| # | 작업 | 대상 | 기대 효과 |
|---|---|---|---|
| P0-1 | Spot 핫패스에서 연속된 단순 조회를 한 turn으로 묶는다 | cpp `runtime/spots/spot_runtime.cpp` | 블로킹 브리지 send 11→4~5 · request 13→5~6 |
| P0-2 | binding wrapper의 중복 lock 제거 | java `runtime/` binding wrapper 31곳 | hot path 7곳 |
| P0-3 | 큐 임계 구역에서 `BigInteger` 할당을 걷어낸다 | java `execution/ZLinkAsyncSerialQueue.java` | enqueue마다 4할당 제거 |
| P0-4 | **조사** — mailbox 두 축 계약(Framework API §11)을 네 언어가 어디서 만족하는지 확인한다 | 아래 §2.1 | 언어별로 "만족 / 이탈 / 미구현" 판정과 근거 |

P0-1~P0-3의 근거는 [spot-hotpath-bridge-survey.ko.md](spot-hotpath-bridge-survey.ko.md)에
있다. P0-1은 새 설계가 아니라 스펙 07 §7을 지키는 일이다 — cpp가 lane 전환 중 그 규칙을
어긴 자리를 되돌린다.

### 2.1 P0-4 — mailbox 두 축 회계 (자체 조사 2026-08-28, 부분 완료)

**계약.** owner mailbox는 건수·byte 두 축을 하나의 작업으로 예약하고, **반환은 handler가
끝난 뒤**다([Framework API §11](../../../framework/doc/framework/common/spec/server/00-foundation/06-framework-api.ko.md#11-handler-실행-객체와-dependency-수명) ·
[02 §7](../../../framework/doc/framework/common/spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md#7-lane-분리와-우선순위-구현)).
`mailboxMessageBudget`·`mailboxByteBudget`은 **MeshNode socket 설정**이다(4언어 exact
interface 공통).

**실측 — 회계는 두 계층에 있다.** 앞선 "dotnet 미구현" 표는 serial queue 계층만 보고 내린
판정이라 틀렸다.

| 계층 | dotnet | java | cpp | node |
|---|---|---|---|---|
| **mesh/service mailbox** (owner별 admission, socket 설정이 여기로 감) | `ZLinkManagedMeshNode.EnqueueOwned` → `OwnedMailbox.TryEnqueue(…, MailboxMessageBudget, MailboxByteBudget)` (`:10530`) | `ZLinkServiceMailbox.tryEnqueue` (`:39`, 고정비 record 96+part 16) | `service_mailbox_t::try_enqueue` (`service_mailbox.cpp:46`, 4개 budget 필수) | `service-mailbox.ts` enqueue에서 `queue.bytes += retainedBytes` (`:102-124`) |
| **owner serial queue** (02 §7의 application·lifecycle lane, 기본값 1,024/64 MiB·128/4 MiB·고정 256) | **없음** — admission이 seal·stopping만 본다 | `ZLinkAsyncSerialQueue` 두 축 | `serial_execution_queue` 두 축 (`hpp:119-130`) | `ZLinkBoundedSerialScheduler` 두 축 |

**반환 시점 실측** — 02 §7은 "handler terminal completion에서만 반환"을 요구한다.

| 언어 | serial queue 계층 | mesh mailbox 계층 |
|---|---|---|
| java | **handler 종료 시** — `finish(entry)`→`release(entry)` (`:670-679`), javadoc "released when the turn reaches its terminal boundary" (`:370`) | 확인 필요 |
| node | **handler 종료 시** — record settle에서 `release()` (`serial-scheduler.ts:226,237`) | **dequeue 시** — drain에서 `queue.bytes -= nextBytes` (`service-mailbox.ts:158`) |
| cpp | 완료 경로에서 `lane.bytes -= _active_bytes` (`:1094`) — terminal 의미 **확인 필요** | 확인 필요 |
| dotnet | 계층 자체가 없음 | **dequeue 시** — `TryDequeue`→`onRecordDequeued(PendingBytes)` (`ZLinkMeshOwnedMailbox.cs:122`) |

**확정 (2026-08-28, 스펙 04 §8 「Owner 예약의 이관」).** 한 record의 예약은 receive 수락부터
handler terminal completion까지 끊기지 않는다 — mailbox가 claim까지, 실행 queue가 claim부터
terminal까지 지고, claim 경계에서 이관한다. 이관은 재판정이 아니다(용량 거절은 로컬 제출에만).
이 계약 기준으로 언어별 정합 작업은:

| 언어 | 판정 | 작업 |
|---|---|---|
| dotnet | **이탈** — 실행 queue에 회계가 없어 dequeue~terminal 구간이 계상되지 않는다 | P1-6: `ZLinkSerialExecutionQueue`에 두 축 계상 신설, claim 이관은 계상만(재판정 금지) |
| node | **이탈** — `submitPreAdmitted`가 계상까지 건너뛴다(재판정 회피는 옳았으나 이관 계상이 빠짐) | P4-8: preAdmitted 경로를 "계상하되 거절하지 않는" 이관으로 바꾼다 (`serial-scheduler.ts:145`) |
| java | serial 쪽 반환 시점은 정합(terminal). **이관 record를 용량 거절하는지** 확인 | P2-6: `enqueueWithPayloadBytes`의 ingress 경로가 capacity 거절을 하면 이관 계상으로 바꾼다 |
| cpp | 두 축 있음. 반환 시점(`:1094`)과 이관 의미 확인 | P3-6: terminal 반환·이관 계상 검증, 어긋나면 정합 |

02 §7이 두 계층 중 실행 queue 쪽의 반환 시점을 소유한다는 것도 04 §8에 명시했다 —
spec-gap이 아니라 확정이다.

P0-1~P0-3의 근거는 [spot-hotpath-bridge-survey.ko.md](spot-hotpath-bridge-survey.ko.md)에
있다. P0-1은 새 설계가 아니라 스펙 07 §7을 지키는 일이다 — cpp가 lane 전환 중 그 규칙을
어긴 자리를 되돌린다.

## 3. P1 — dotnet

dotnet은 세 계층 조율자를 모두 갖고 있다. **조율자는 이름을 스펙에 맞추고, 큐 primitive에는
정책 주입만 채운다.**

**dotnet 실행 큐에는 mailbox의 count·byte 회계가 없다(실측 2026-08-28).** admission이
relocation seal과 stopping 상태만 본다. java·cpp·node는 두 축을 갖고 있으므로
[Framework API §11](../../../framework/doc/framework/common/spec/server/00-foundation/06-framework-api.ko.md#11-handler-실행-객체와-dependency-수명)의
두 축 계약을 dotnet이 어디서 만족하는지 확인이 필요하다 — `ZLinkManagedMeshNode`의
`SetMailboxBudgets` 경로가 그 자리일 수 있다. **P0-4로 조사한 뒤 P1 범위를 정한다.**

| # | 작업 | 파일 | 완료 판정 |
|---|---|---|---|
| P1-1 | `ZLinkActorDispatchMailbox` → `ZLinkActorSerialExecutor` | `Runtime/Actors/ZLinkActorDispatchMailbox.cs` | 이전 이름이 저장소에서 0건 |
| P1-2 | `ZLinkStreamSessionSerialExecutor` → `ZLinkSessionSerialExecutor` | `Runtime/Streams/ZLinkStreamSessionSerialExecutor.cs` | 이전 이름 0건 |
| P1-3 | Session 진입점 동사 `Enqueue*` → `Execute*` 넷 | 위 파일 | 스펙 07 §3 표와 일치 |
| P1-4 | `_laneGate` lock을 state lane 소유로 바꾼다 | `Runtime/Spots/ZLinkSpotSerialExecutor.cs:12,69,87,1108` | 그 파일에 `lock (` 0건 |
| P1-5 | 상수로 박힌 `OwnerTimeSliceMilliseconds`·`LifecycleTurnLimit`을 정책 주입으로 바꾼다 | `Runtime/Execution/ZLinkSerialExecutionQueue.cs:7,8` | `ZLinkExecutionLanePolicy` 일곱 값이 주입된다 |
| P1-6 | 큐에 count·byte 두 축 계상을 신설한다 — claim 이관 record는 계상만 하고 재판정하지 않는다(04 §8) | `Runtime/Execution/ZLinkSerialExecutionQueue.cs` | 스펙 07 §10 "수용량과 backpressure" + 04 §8 내부 확인 조건 |

**양보 동작 자체는 dotnet에 이미 있다.** `DrainAsync`가 `OwnerTimeSliceMilliseconds`(10ms)마다
slice를 끊고, `LifecycleTurnLimit`(8)로 lifecycle 연속 선점을 막는다. 빠진 것은 동작이 아니라
**정책 주입**이다 — 두 값이 `internal const`로 박혀 있어 Spot·Actor·session이 서로 다른 값을
쓸 수 없다. P1-5는 그 주입만 한다.

**P1 착수 전에 dotnet 세션과 소유를 확인한다.** 사용자가 앞선 캠페인에서 dotnet을 다른
세션에 맡겼고, P1-4가 건드리는 `ZLinkSpotSerialExecutor.cs`는 그 세션의 작업 대상일 수 있다.
P2·P3가 P1에 걸려 있으므로 충돌하면 전체가 밀린다.

**P1-4는 세 곳이 아니라 네 곳이다.** `_laneGate`가 지키는 queue map은 스펙 07 §2가 **C2**로
판정한다 — 조율자를 닫을 때 map 비우기와 그 안의 queue 완료가 함께 움직이기 때문이다.
state lane 소유로 옮기고, `close` 경로(`:87`)의 그 두 동작은 한 turn 안에서 원자적으로
처리한다.

---

## 4. P2 — java

java는 큐 primitive가 정본이고 **조율자가 셋 다 없다.** 런타임 클래스가 큐를 직접 들고 있다.

| # | 작업 | 파일 | 완료 판정 |
|---|---|---|---|
| P2-1 | `ZLinkSpotSerialExecutor` 신설 · `ZLinkActorDispatchSerials.queues`를 그 안으로 옮긴다 | `runtime/actors/ZLinkActorDispatchSerials.java` → `runtime/spots/` | 조율자 밖에서 Actor 큐를 드는 곳 0건 |
| P2-2 | `ZLinkActorSerialExecutor` 신설 | `runtime/actors/` | 큐 맵 없이 인스턴스당 큐 하나 |
| P2-3 | `ZLinkSessionSerialExecutor` 신설 · `ZLinkStreamRuntime.stateLane`에서 실행 책임을 분리 | `runtime/streams/ZLinkStreamRuntime.java` | 진입점 넷이 스펙 07 §3과 일치 |
| P2-4 | `ZLinkAsyncSerialQueue` → `ZLinkSerialExecutionQueue` 개명 | `execution/ZLinkAsyncSerialQueue.java` | 이전 이름 0건 |
| P2-5 | Actor 경로의 `sharedSpotGate()` 분기를 조율자 안으로 넣는다 | `runtime/spots/ZLinkDefaultSpotContext.java` | 호출자가 큐를 고르는 자리 0건 |
| P2-6 | **(04 §8)** 이관 record의 용량 거절 여부를 확인하고, 거절하면 "계상만 하는 이관"으로 바꾼다 | `execution/ZLinkAsyncSerialQueue.java` (`enqueueWithPayloadBytes` ingress 경로) | permit 이관 record의 용량 거절 0건 |
| P2-7 | **(07 §6.5)** 첫 turn의 inline 시작을 공유 실행 자원 게시로 바꾼다 — 지금은 `enqueueAccepted`가 `synchronized` 구간 안에서 `startNext()`를 직접 불러 제출자 스택·queue monitor 아래에서 handler 동기 구간이 실행된다 | `execution/ZLinkAsyncSerialQueue.java` (enqueueAccepted→startNext) | drain이 제출 스택에서 시작되는 자리 0건 |

**P2-3은 분리이지 이동이 아니다.** `ZLinkStreamRuntime.stateLane`은 상태 소유와 작업 실행을
함께 지고 있다. 상태 소유는 그 자리에 남기고(스펙 06), 작업 실행만 새 조율자로 옮긴다 —
둘을 같은 객체에 두면 스펙 07 §1이 구분하는 두 문제가 다시 섞인다.

---

## 5. P3 — cpp

cpp는 조율자가 셋 다 없다. **큐 primitive는 이미 완성돼 있다(실측 2026-08-28)** —
`serial_execution_queue.hpp:119-130`에 정책 일곱(건수·byte 두 축 × application·lifecycle,
`owner_time_budget`, `lifecycle_burst_limit`)이 모두 있고 우선순위와 lifecycle debt도
구현돼 있다. 따라서 P3는 **조율자 신설이 전부**다.

| # | 작업 | 파일 | 완료 판정 |
|---|---|---|---|
| P3-1 | `spot_serial_executor_t` 신설 · `spot_runtime`의 이름 맵을 그 안으로 옮긴다 | `runtime/spots/spot_runtime.{hpp,cpp}` | 조율자 밖에서 Actor·timer 큐를 드는 곳 0건 |
| P3-2 | `actor_serial_executor_t` 신설 | `runtime/actors/` | 큐 맵 없이 인스턴스당 큐 하나 |
| P3-3 | `session_serial_executor_t` 신설 · `stream_runtime.dispatch_queue`에서 분리 | `runtime/streams/stream_runtime.{hpp,cpp}` | 진입점 넷이 스펙 07 §3과 일치 |
| P3-5 | 조회 스냅샷 묶기 (P0-1과 같은 작업 — 먼저 끝났으면 생략) | `runtime/spots/spot_runtime.cpp` | 같은 값을 두 번 읽는 자리 0건 |
| P3-6 | **(04 §8)** 반환 시점이 handler terminal인지, claim 이관이 재판정 없이 계상되는지 검증하고 어긋나면 정합 | `runtime/execution/serial_execution_queue.cpp:1094` · `runtime/mesh/service_mailbox.cpp` | 04 §8 내부 확인 조건 통과 |

**참조 구현을 반드시 명시해 맡긴다.** cpp `spot_runtime` 작업은 이번 캠페인에서 참조 없이
맡겼을 때 네 번 실패했고, dotnet·java 구현을 참조로 지정한 뒤에야 통과했다.

---

## 6. P4 — node

**node에는 Spot 조율자가 없다.** 현행 `ZLinkSpotSerialExecutor`(314줄)는 조율자가 아니라
`ZLinkBoundedSerialScheduler` 하나를 감싸 turn·yield·barrier 의미를 붙인 **직렬 단위
wrapper**다 — 맵도 라우팅도 없고, Spot 하나(`spot-activation.ts:468`)·Actor
하나(`spot-activation-state.ts:411`)·timer 하나(`spot-activation.ts:485`)마다 각각 생성된다.
라우팅은 `actorSerial()`과 timer registry closure에 흩어져 있다.

| 현행 클래스 | 실제 역할 | 스펙 07의 이름 |
|---|---|---|
| `ZLinkBoundedSerialScheduler` (374줄) | 수용량·lane을 가진 직렬 큐 | `ZLinkSerialExecutionQueue` |
| `ZLinkSpotSerialExecutor` (314줄) | 직렬 단위 하나 + turn 의미 | 조율자가 아니다 — **개명 대상** |
| 없음 | Spot 조율자 | `ZLinkSpotSerialExecutor` |
| `ZLinkStreamSessionSerialExecutor` (73줄) | 큐 하나를 가진 session 실행기 | `ZLinkSessionSerialExecutor` — 개명만 |

**이름이 충돌한다.** 스펙이 조율자에 주는 이름을 현행 wrapper가 이미 쓰고 있다. 먼저
wrapper를 비우고(P4-1) 그 이름을 조율자에 준다 — 순서를 뒤집으면 같은 이름의 두 클래스가
한동안 공존한다.

| # | 작업 | 파일 | 완료 판정 |
|---|---|---|---|
| P4-1 | 현행 `ZLinkSpotSerialExecutor`를 직렬 단위 이름으로 개명해 `ZLinkSpotSerialExecutor`를 비운다 | `runtime/spots/spot-serial-executor.ts` | 이 이름을 쓰는 클래스가 0건 |
| P4-2 | Spot 조율자 `ZLinkSpotSerialExecutor` **신설** | `runtime/spots/` | 진입점 넷이 스펙 07 §3과 일치 |
| P4-3 | `actorSerials` 맵을 조율자로 옮긴다 | `spots/spot-activation-state.ts:93,406` | 조율자 밖 0건 |
| P4-4 | `timerSerials` 맵을 조율자로 옮긴다 | `spots/spot-activation.ts:471` | closure로 잡는 자리 0건 |
| P4-5 | `ZLinkActorDispatchMailboxSet`을 조율자로 옮기고, `ZLinkActorDispatchMailbox`를 `ZLinkActorSerialExecutor`로 개명한다 | `actors/actor-mailbox.ts` · `spot-entry-activation.ts` · `spot-activation-state.ts` | 소유처가 조율자 하나 |
| P4-6 | `ZLinkStreamSessionSerialExecutor` → `ZLinkSessionSerialExecutor` · 진입점 넷을 §3과 맞춘다 | `streams/session-serial-executor.ts` | 이전 이름 0건 |
| P4-7 | `ZLinkBoundedSerialScheduler` → `ZLinkSerialExecutionQueue` · 정책 이름 일곱을 §6.1과 맞춘다 | `runtime/execution/serial-scheduler.ts` | 정책 이름 일곱이 스펙과 일치 |
| P4-8 | **(04 §8)** `submitPreAdmitted`를 "계상하되 거절하지 않는" 이관으로 바꾼다 — 지금은 계상까지 건너뛴다 | `runtime/execution/serial-scheduler.ts:145` · mesh 반환은 `foundation/service-mailbox.ts:158` | dequeue~terminal 구간 무계상 0 |

**P4는 java·cpp와 같은 규모다.** 앞서 "node는 조율자가 있으니 이관만"이라고 적었던 것은
`spot-serial-executor.ts`의 클래스 이름만 보고 역할을 판정한 결과이며 틀렸다.

**P4에 넣지 않는 것** — `SpotWide`에서 Actor 작업을 Actor 큐에 거치게 하는 변경. node는 지금
공용 직렬 단위를 그대로 돌려주어(`spot-activation-state.ts:407`) Actor별 payload 상한이 걸리지
않는다. 그 상한이 `SpotWide`에서 필요한 보장인지가 아직 정해지지 않았으므로(스펙 07 §9),
정해지기 전에는 현행을 유지한다.

---

## 7. P5 — 검증

스펙 07 §10의 검증 요구를 언어별 계약 test로 옮긴다. **항목마다 test 하나**이고, §10에 없는
항목을 새로 만들지 않는다.

각 규칙 문단의 "내부 확인 조건"은 test가 아니라 **정적 계수**로 확인한다.

| 확인 | 명령 형태 | 기대 |
|---|---|---|
| 조율자 밖 Actor·timer 큐 소유 | 언어별 큐 타입 이름 grep − 조율자 파일 | 0 |
| Actor·Session 조율자의 큐 맵 | `Map<`·`Dictionary<`·`map<` grep | 0 |
| `SpotWide` 상위 큐의 payload 인자 | 상위 제출 호출의 인자 확인 | payload 없음 |
| 같은 값 이중 조회 | 처리 경로별 registry 호출 계수 | 0 |
| 소유 전제 단언 | `isOnLane`·`throwIfReentrant` 호출 위치 | 전제 자리마다 1 |

게이트는 언어별 unit·계약 + Z0 + 6샘플이다
([`../concurrency-redesign/rules.ko.md`](../concurrency-redesign/rules.ko.md) §4).

---

## 8. 이 플랜이 실패하는 방식

이번 캠페인에서 실제로 일어난 것만 적는다.

- **참조 구현 없이 신설을 맡긴다.** cpp `spot_runtime` 4회 실패. P3의 각 항목에 dotnet·java의
  해당 파일 경로를 함께 적어 맡긴다.
- **같은 빌드 트리에 두 작업을 붙인다.** 유령 SEGFAULT와 유령 실패 5건이 나와 측정 전체가
  무효가 됐다. P2·P3·P4는 트리가 다르므로 안전하고, 같은 언어 안에서는 한 번에 하나다.
- **빌드 실패를 진행 중으로 읽는다.** 전체 빌드 rc만 보면 무관한 codegen 실패가 통과로
  읽힌다. 필요한 target만 빌드하고 `Built target <name>` 문자열로 판정한다.
- **`git add -A`로 진행 중인 작업을 쓸어 담는다.** 두 번 일어났다. 경로를 명시해 add한다.
- **소유 문서를 안 읽고 구현 코드에서 계약을 추론한다.** SpotWide 2단 제거·byte 축 제거
  두 Critical이 전부 이 실수였다 — 02 §3의 `Yield` claim과 Framework API §11이 정확히
  반대를 규정하고 있었다. §0.1의 읽기 순서가 그 재발 방지다.
- **한 파일만 보고 "없다"고 판정한다.** node Actor 큐를 `spot-serial-executor.ts`만 보고
  없다고 적었으나 다른 세 파일에 있었다. 없다는 판정은 저장소 전체 검색으로만 내린다.

---

## 9. 미결 — 착수 전에 정해야 하는 것

| # | 미결 | 막는 단계 |
|---|---|---|
| ① | node `SpotWide`에서 Actor별 payload 상한이 필요한 보장인가 | P4의 범위 |
| ② | 계층별로 참조할 언어를 나눈다 — lane primitive·조율자는 .NET, 큐 primitive·turn 경계는 java. 지금까지의 ".NET 하나만 참조" 방침을 고쳐야 한다 | P2·P3의 참조 지정 |
| ③ | 호환 경계 회수 — dotnet `AwaitStateLane` 664 · cpp `.get()` 456 | P0-1 후 재측정 |

①·②는 P1과 무관하므로 **P1은 지금 착수할 수 있다.**

---

[작업 폴더 목차](README.ko.md) · [계약: 스펙 07](../../../framework/doc/framework/common/spec/server/01-execution/07-serial-executor-layers.ko.md) · [이름 계약서](executor-naming-contract.ko.md)
