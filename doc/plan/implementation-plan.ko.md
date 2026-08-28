# 구현 플랜 — 직렬 실행기 계층 정렬

[계약: 스펙 07](../../framework/doc/framework/common/spec/server/01-execution/07-serial-executor-layers.ko.md) · 이 폴더의 유일한 문서다 — 이전 조사·초안은 git 이력(`e5c38ff111` 이전)에 있다

이 문서는 **네 언어 runtime의 실행기 코드를 스펙 07에 맞추는 순서**를 정한다. 무엇이 옳은지는
스펙 07이 소유한다 — 이 문서는 그것을 여기에 다시 적지 않고, 어떤 순서로 어느 파일을 고치고
무엇으로 완료를 판정하는지만 담는다.

구현 세션이 읽을 것은 셋이다.

| # | 문서 | 역할 |
|---|---|---|
| 1 | [스펙 07 직렬 실행기 계층](../../framework/doc/framework/common/spec/server/01-execution/07-serial-executor-layers.ko.md) | **계약.** 코드가 여기에 맞춰진다 |
| 2 | [스펙 06 상태 소유와 state lane](../../framework/doc/framework/common/spec/server/01-execution/06-state-ownership-and-lanes.ko.md) | queue map을 무엇으로 지키는가(C1·C2 판별) |
| 3 | 스펙 07 | 현행 코드 실측과 언어별 대비 작업 |

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
| 6 | `03-spot-actor/04-actor-model.ko.md` §3 | Actor queue 불변 — "Actor payload는 항상 그 Actor queue에, Spot application queue에 넣지 않는다" |
| 7 | 이 플랜 **부록 B·C** | 게이트 매트릭스와 알려진 기존 실패(B), **lane 전환 절차**(C — P1-4가 따른다) |


용어집(`00-foundation/02-glossary.ko.md`)의 `User Spot execution mode`·`Spot turn`·`Owner`는
위 문서들이 링크하는 자리에서 따라 읽으면 된다 — 통독 대상은 아니다.

### 0.1a 이 폴더의 문서는 이것 하나다

이름·라우팅·정책 계약은 전부 정식 스펙(07 §2·§3·§6·§9, 04 §8)이 소유한다. P0-1의 콜사이트
근거는 **부록 A**에 있다. 이전 조사·초안·계약서 문서는 정식 스펙으로 승격을 마쳐 삭제했다 —
필요하면 git 이력에서 본다.

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
| node | `npx tsc -b tsconfig.build.json` 후 `npm test` | contract 기존 실패 23건 등재(부록 B) — 회귀 판정은 실패 이름 집합 대조로. lint 157건(0.14.0 타입 적응)은 별도 수정 진행 중 |

### 0.5 진행표 — 새 세션이 이 표를 갱신한다

| 항목 | 상태 | 비고 |
|---|---|---|
| P0-1 cpp 조회 묶기 | **부분 완료** `4681eb7931` | warm 경로 materialization·dispatch projection 통합. **fence 이중 조회(:9618·:9708)는 유지 판정** — backlog 선택 직전의 의도된 재판정(finish_move_replay liveness, 02 §3). 병합 시 host_lifecycle 실패 실증 |
| P0-2 java wrapper lock | **보류** | 에이전트 [의심] 승인 — wrapper lock 39곳(31 아님), 상위 배타성 증거 없음(receive/transport lock 병행 경로). 별도 sol 조사로 이월 |
| P0-3 java BigInteger | **완료** `33410f2172` | long 포화 검사로 대체, §11 표현 범위 초과 거절 계약 보존 + 회귀 test |
| P0-4 두 축 회계 조사 | **완료** | §2.1 — 판정은 04 §8로 스펙 확정, 잔여는 P1-6·P2-6·P3-6·P4-8 |
| P1 dotnet | P1-1~3 **완료** `e877bfff37` · P1-4 진행 중 | 세션 소유 확인 완료(2026-08-28 사용자 — 다른 작업 없음) |
| P2 java | P2-4 **완료** `fc44a59d32` · P2-6·P2-7 진행 중 | P2-1~2-3·2-5는 다음 배치 |
| P3 cpp | P3-1 진행 중 | 참조: 조율자=dotnet·큐/turn=java (§9 ② 채택) |
| P4 node | P4-1·4-6·4-7 **완료** `bd83b30db5` · P4-2~4-5 진행 중 | wrapper 새 이름 `ZLinkSpotSerialTurnExecutor` |
| P5 계약 test | 대기 | P1~P4 뒤 |
| 마감 게이트 (unit·cross-language e2e·샘플 6종 ZoneWorld 제외) | 대기 | §1.1 |

§9 ② 판정(2026-08-28 감독관): 계층별 참조 분리 채택 — lane primitive·조율자는 dotnet,
큐 primitive·turn 경계는 java. §9 ①은 P4 범위 제외 유지(현행 보존).

---

## 1. 착수 순서 — 언어별 병렬 (2026-08-28 사용자 지정)

**P1~P4는 언어별로 병렬로 진행한다.** 트리가 서로 다르고(각 `framework/languages/<lang>`),
이름·라우팅은 스펙 07(§2·§3·§9)에
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

명령과 판정 기준은 **부록 B**를 그대로 쓴다(알려진 기존 실패 목록 포함). 샘플·harness 실행은 codex
sandbox가 loopback bind를 막으므로 **감독관이 직접 실행**한다(§0.2).

## 2. P0 — 구조와 독립인 선행 작업

| # | 작업 | 대상 | 기대 효과 |
|---|---|---|---|
| P0-1 | Spot 핫패스에서 연속된 단순 조회를 한 turn으로 묶는다 | cpp `runtime/spots/spot_runtime.cpp` | 블로킹 브리지 send 11→4~5 · request 13→5~6 |
| P0-2 | binding wrapper의 중복 lock 제거 | java `runtime/` binding wrapper 31곳 | hot path 7곳 |
| P0-3 | 큐 임계 구역에서 `BigInteger` 할당을 걷어낸다 | java `execution/ZLinkAsyncSerialQueue.java` | enqueue마다 4할당 제거 |
| P0-4 | **조사** — mailbox 두 축 계약(Framework API §11)을 네 언어가 어디서 만족하는지 확인한다 | 아래 §2.1 | 언어별로 "만족 / 이탈 / 미구현" 판정과 근거 |

P0-1의 근거는 부록 A에 있다. P0-1은 새 설계가 아니라 스펙 07 §7을 지키는 일이다 — cpp가 lane 전환 중 그 규칙을
어긴 자리를 되돌린다.

### 2.1 P0-4 — mailbox 두 축 회계 (자체 조사 2026-08-28, 부분 완료)

**계약.** owner mailbox는 건수·byte 두 축을 하나의 작업으로 예약하고, **반환은 handler가
끝난 뒤**다([Framework API §11](../../framework/doc/framework/common/spec/server/00-foundation/06-framework-api.ko.md#11-handler-실행-객체와-dependency-수명) ·
[02 §7](../../framework/doc/framework/common/spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md#7-lane-분리와-우선순위-구현)).
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

P0-1의 근거는 부록 A에 있다. P0-1은 새 설계가 아니라 스펙 07 §7을 지키는 일이다 — cpp가 lane 전환 중 그 규칙을
어긴 자리를 되돌린다.

## 3. P1 — dotnet

dotnet은 세 계층 조율자를 모두 갖고 있다. **조율자는 이름을 스펙에 맞추고, 큐 primitive에는
정책 주입만 채운다.**

**dotnet 실행 큐에는 mailbox의 count·byte 회계가 없다(실측 2026-08-28).** admission이
relocation seal과 stopping 상태만 본다. java·cpp·node는 두 축을 갖고 있으므로
[Framework API §11](../../framework/doc/framework/common/spec/server/00-foundation/06-framework-api.ko.md#11-handler-실행-객체와-dependency-수명)의
두 축 계약을 dotnet이 어디서 만족하는지 확인이 필요하다 — `ZLinkManagedMeshNode`의
`SetMailboxBudgets` 경로가 그 자리일 수 있다. **P0-4로 조사한 뒤 P1 범위를 정한다.**

| # | 작업 | 파일 | 완료 판정 |
|---|---|---|---|
| P1-1 | `ZLinkActorDispatchMailbox` → `ZLinkActorSerialExecutor` | `Runtime/Actors/ZLinkActorDispatchMailbox.cs` | 이전 이름이 저장소에서 0건 |
| P1-2 | `ZLinkStreamSessionSerialExecutor` → `ZLinkSessionSerialExecutor` | `Runtime/Streams/ZLinkStreamSessionSerialExecutor.cs` | 이전 이름 0건 |
| P1-3 | Session 진입점 동사 `Enqueue*` → `Execute*` 넷 | 위 파일 | 스펙 07 §3 표와 일치 |
| P1-4 | `_laneGate` lock을 state lane 소유로 바꾼다 — 절차는 **부록 C** | `Runtime/Spots/ZLinkSpotSerialExecutor.cs:12,69,87,1108` | 그 파일에 `lock (` 0건 |
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
(부록 B).

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

[계약: 스펙 07](../../framework/doc/framework/common/spec/server/01-execution/07-serial-executor-layers.ko.md) · 이 폴더의 유일한 문서다 — 이전 조사·초안은 git 이력(`e5c38ff111` 이전)에 있다

---

## 부록 A — P0-1 콜사이트 조사 (spot-hotpath-bridge-survey에서 이관, 실측 2026-08-27)

P0-1("조회를 한 turn으로 묶는다")의 실행 근거다. 어느 콜사이트를 묶는지가 아래에 행 번호
단위로 있다. 행 번호는 조사 시점 기준이므로 착수 시 재확인한다.

### A.1 결론

정상적으로 활성화된 Spot, warm Actor와 실행 큐, 유효한 route fence를 전제로 하면
**handler 호출까지의 `[매번]` 브리지는 경로별로 다음과 같다.**

| 경로 | 선택한 정상 경로 | `[매번]` 브리지 |
|---|---|---:|
| 원격 Actor send | spot-wide 실행, 유효한 Actor route, relocation·bound Session 없음 | **11** |
| 원격 Actor request | 위 조건 + 첫 hop의 wire operation ID 있음 | **13** |
| Spot 간 send/request | 유효한 Spot route fence가 있는 application packet | **5** |
| Actor join | 이미 활성화된 일반 User Spot, warm Actor instance | **7** |
| Spot timer 발화 | spot-wide 실행, fire batch의 첫 tick | **2** |
| Spot timer 발화 | per-actor 실행, fire batch의 첫 tick | **1** |

가장 큰 비용은 원격 Actor packet이다. send 한 건이 노드 상태 lane 9회와 Spot callback
lane 2회를 기다린다. request는 여기에 handoff reply 보관 lane과 pending-request 계수 lane을
각 1회 더 기다린다. 이 수치는 저장소 전체의 브리지 출현 개수가 아니라, 아래에 고정한
구체적인 성공 경로 한 번의 실제 통과 수다.

#### 계수 경계

- 시작점은 cpp MeshNode 수신 callback 또는 native timer callback이다.
- 끝점은 application handler를 호출하는 문장이다. handler가 반환한 뒤의 terminal 정리와
  reply 전송은 주 계수에 넣지 않았다.
- `state_lane_t::run()`은 `std::future`를 반환한다
  (`framework/languages/cpp/framework/src/runtime/execution/state_lane.hpp:30-62`). 그 결과를
  `.get()`으로 기다리는 호출만 센다.
- `serial_execution_queue_t::try_post*`, coroutine `co_await`, 일반 `task.result()`, mutex 대기,
  optional·smart pointer의 `.get()`은 세지 않았다.
- handler terminal까지 넓히면 Actor packet, Spot route, timer는
  `spot_context_state_t::leave_callback()`의 callback lane 대기 1회가 추가된다
  (`framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:1998-2014`). Actor join은
  handler 승인 뒤 commit 경로가 이어지지만, 요청한 “handler 호출까지” 경계 밖이므로 주 계수에
  넣지 않았다.

### A.2 경로별 상세

#### A.2.1 원격 Actor packet → Spot Actor handler

#### 선택한 경로와 진입점

진입점은 `mesh_node_host_service_t::start()` 안의 `dispatch_ready` receive callback이다
(`framework/languages/cpp/framework/src/runtime/mesh/mesh_node_host_service.cpp:1616`,
`:2213-2217`). 다음 정상 조건을 고정했다.

- `owner_kind=actor`, `record_kind=actor_send` 또는 `actor_request`
- 현재 노드와 Actor incarnation을 정확히 가리키는 `actor_route`가 있음
- bound Session source와 relocation/Message Follow 우회가 없음
- Actor instance와 Actor 실행 큐가 이미 만들어져 있음
- User Spot 실행 모드는 `spot_wide`

#### 호출 체인

1. `mesh_node_host_service_t::start`의 receive callback
   (`mesh_node_host_service.cpp:2213-2217`)
2. Application Job Queue permit을 받은 뒤 application executor에 제출
   (`mesh_node_host_service.cpp:2292-2339`)
3. `spot_node_runtime_t::dispatch_mesh_record`
   (`mesh_node_host_service.cpp:2352-2359`, `spot_runtime.cpp:11985`)
4. Actor record 분기와 route admission
   (`spot_runtime.cpp:12324-12516`)
5. `spot_node_runtime_t::relay_actor_packet`
   (`spot_runtime.cpp:12756-12764`, `:9560`)
6. Actor materialization·현재 Spot dispatch projection
   (`spot_runtime.cpp:9845-9881`, `:10024-10077`)
7. `spot_handler_registry_t::invoke_erased`
   (`spot_runtime.cpp:10187-10194`, `:3681`)
8. 등록된 application handler 호출
   (`spot_runtime.cpp:3953-3967`)

#### 브리지

| # | 지점 | 분류 | 조건과 빈도 |
|---:|---|---|---|
| 1 | Actor type 조회 (`spot_runtime.cpp:12327-12334`) | `[매번]` | 선택한 send/request 모두 통과 |
| 2 | native node·relocation·admission callback 묶음 조회 (`:12385-12390`) | `[매번]` | route admission 전마다 통과 |
| 3 | 현재 Actor authority fence 조회 (`:12416-12420`) | `[매번]` | 선택한 “route 있음, bound Session 없음” 경로마다 통과 |
| 4 | handoff reply token 보관 (`:12576-12599`) | `[매번]` request 전용 | 첫 hop이고 operation ID가 있는 request마다 통과. send에는 없음 |
| 5 | relay parking node RID 조회 (`:12628-12630`) | `[매번]` | send/request 모두 metadata 구성 전에 통과 |
| 6 | retiring Actor 검사 (`:9595`) | `[매번]` | `relay_actor_packet` 진입마다 통과 |
| 7 | 첫 번째 현재 authority fence 조회 (`:9617-9622`) | `[매번]` | 선택한 fenced local-target 경로마다 통과 |
| 8 | 두 번째 현재 authority fence 조회 (`:9707-9712`) | `[매번]` | backlog/Message Follow 방향 결정 전에 같은 fence를 다시 읽음 |
| 9 | factory·location·Actor instance projection (`:9845-9881`) | `[매번]` | warm instance여도 조회 turn은 항상 통과 |
| 10 | 현재 generation·Spot context·실행 모드 projection (`:10024-10077`) | `[매번]` | handler dispatch 직전마다 통과 |
| 11 | pending request 증가 (`:10171-10176`) | `[매번]` request 전용 | request마다 통과. send에는 없음 |
| 12 | Spot serial queue snapshot (`:3873-3895` → `:2110-2114`) | `[매번]` spot-wide | Actor queue turn이 Spot queue에 넘기기 전에 callback lane을 기다림 |
| 13 | callback admission·depth 증가 (`:3953` → `:1988-1995`) | `[매번]` | application handler 호출 직전에 통과 |
| C1 | Actor type cache 채우기 (`:12335-12342`) | `[조건부]` | 이 노드가 Actor ID의 type을 처음 authority에서 찾은 때 1회. Actor별 cold hit |
| C2 | Actor instance 설치 (`:9890-9919`) | `[조건부]` | 이 노드의 첫 materialization 때 1회. factory 호출 자체는 lane 밖 |
| C3 | Actor 실행 큐 생성 (`:3740-3762`) | `[조건부]` | copy-on-write queue snapshot miss 때 1회. 보통 Actor별 첫 packet |

send는 표의 1, 2, 3, 5-10, 12, 13을 통과해 **11회**다. request는 4와 11이
추가되어 **13회**다. type cache·Actor instance·Actor queue가 모두 cold이면 각각 1회씩,
send는 최대 14회, 선택한 request는 최대 16회가 된다.

`per_actor` 실행이면 `invoke_erased`가 Actor queue에서 Spot queue로 한 번 더 넘기지 않으므로
#12가 빠진다. 따라서 같은 steady-state send/request는 각각 10회/12회다. Message Follow나
relocation backlog로 빠지는 분기는 로컬 application handler에 도달하지 않는 다른 terminal
경로라 이 경로의 조건부 가산으로 섞지 않았다.

#### A.2.2 Spot 간 route dispatch → Spot packet handler

#### 선택한 경로와 진입점

진입점은 2.1과 같은 `mesh_node_host_service_t::start()` receive callback
(`mesh_node_host_service.cpp:2213-2217`)이다. `owner_kind=spot`,
`record_kind=spot_send|spot_request`, 유효한 `spot_route`를 가진 일반 application packet을
선택했다. 내부 control packet은 선택하지 않았다.

#### 호출 체인

1. receive callback → application executor → `dispatch_mesh_record`
   (`mesh_node_host_service.cpp:2213-2217`, `:2329-2359`)
2. Spot/node record envelope 및 내부 packet 판별
   (`spot_runtime.cpp:12005-12215`)
3. target Spot context와 route fence admission
   (`spot_runtime.cpp:12216-12250`)
4. `spot_handler_registry_t::invoke_erased`
   (`spot_runtime.cpp:12266-12276`, `:3681`)
5. 등록된 application handler 호출
   (`spot_runtime.cpp:3953-3967`)

#### 브리지

| # | 지점 | 분류 | 조건과 빈도 |
|---:|---|---|---|
| 1 | route client snapshot (`spot_runtime.cpp:12012-12014`) | `[매번]` | application packet이어도 내부 packet 판별 전에 항상 통과 |
| 2 | target Spot context 조회 (`:12228-12230`) | `[매번]` | 선택한 Spot record마다 통과 |
| 3 | Location lifecycle 조회 (`:12236-12241`) | `[매번]` | 선택한 fenced route마다 통과. fence 없는 record에는 없음 |
| 4 | Spot serial queue snapshot (`:3917-3923` → `:2110-2114`) | `[매번]` | 활성화된 Spot의 serial queue에 게시할 때 통과 |
| 5 | callback admission·depth 증가 (`:3953` → `:1988-1995`) | `[매번]` | handler 호출 직전에 통과 |
| C1 | queue 부재 뒤 admission 재검사 (`:2115-2117` → `spot_runtime.hpp:628-632`) | `[조건부]` | 정상 활성화에서는 발생하지 않음. queue가 없거나 teardown 중인 경계 |

따라서 유효한 fenced Spot route는 **5회**다. fence가 없는 legacy/local record는 #3이 없어
4회다. route client를 설정하는 `set_route_client`의 브리지
(`spot_runtime.cpp:11812-11816`)는 `[초기화]`이며 이 메시지 경로에는 없다.

#### A.2.3 Actor join → User Spot `on_actor_join`

#### 선택한 경로와 진입점

진입점은 같은 receive callback에서 받은 `owner_kind=spot`, `record_kind=spot_control`,
`operation_kind=actor_join` record다 (`mesh_node_host_service.cpp:2213-2217`,
`spot_runtime.cpp:12824-12838`). 이미 활성화된 일반 User Spot으로 warm Actor instance가 join하는
경로를 선택했다. entry Spot과 remote relocation prepare 경로는 제외했다.

#### 호출 체인

1. receive callback → application executor → `dispatch_mesh_record`
   (`mesh_node_host_service.cpp:2213-2217`, `:2329-2359`)
2. actor-control join 분기와 target 종류 projection
   (`spot_runtime.cpp:12824-12877`)
3. `join_actor_to_spot_erased`
   (`spot_runtime.cpp:12946-12959`, `:5245`)
4. `actor_join_context` → `actor_factory` → Actor instance lookup → `actor_admission`
   (`spot_runtime.cpp:5258-5268`, `:5318`, `:5353-5356`)
5. application `on_actor_join` callback 호출
   (`spot_runtime.cpp:5359-5363`)

#### 브리지

| # | 지점 | 분류 | 조건과 빈도 |
|---:|---|---|---|
| 1 | Actor type 조회 (`spot_runtime.cpp:12840-12847`) | `[매번]` | join record마다 통과 |
| 2 | entry Spot 여부와 local node RID projection (`:12868-12877`) | `[매번]` | 일반/entry 분기 전에 통과 |
| 3 | target Spot context 선택 (`:4747-4769`) | `[매번]` | 활성 target 확인마다 통과 |
| 4 | Actor factory 조회 (`:4787-4796`) | `[매번]` | join마다 통과 |
| 5 | Spot instance·serializer·source location projection (`:5281-5303`) | `[매번]` | handler 준비마다 통과 |
| 6 | 등록된 Actor instance 조회 (`spot_runtime.hpp:1867-1877`) | `[매번]` | warm instance여도 조회 turn은 통과 |
| 7 | Actor admission callback 조회 (`spot_runtime.cpp:4805-4822`) | `[매번]` | `on_actor_join` 호출 직전에 통과 |
| C1 | Actor type cache 채우기 (`:12848-12855`) | `[조건부]` | 이 노드에서 Actor ID를 처음 관찰한 join. authority 조회 뒤 1회 |
| C2 | Actor instance 설치 (`spot_runtime.hpp:1885-1903`) | `[조건부]` | target node에 instance가 없는 첫 join/materialization 때 1회 |
| C3 | target 동적 생성 후 재선택 (`spot_runtime.cpp:4771-4777`) | `[조건부]` | target Spot이 아직 없고 동적 factory가 하나로 결정될 때. activation 경로이므로 정상 hot path보다 훨씬 낮은 빈도 |

warm join은 **7회**다. C1과 C2가 함께 발생하는 첫 target-node join은 handler 호출 전
**9회**다. C3는 Spot 생성·Location 수명주기 전체로 분기하는 cold activation이므로 “메시지마다”
비용으로 보지 않았다. application `on_actor_join`은 별도 callback lane을 통하지 않고 직접
호출된다 (`spot_runtime.cpp:5361-5363`).

#### A.2.4 native timer fire → Spot timer handler

#### 선택한 경로와 진입점

진입점은 timer 등록 때 설치한 native `on_fire` callback이다
(`framework/languages/cpp/framework/src/runtime/timers/timer_runtime.cpp:121-125`). 정상적으로
활성화된 timer의 fire batch가 첫 application tick을 호출하는 경로를 선택했다.

#### 호출 체인

1. native `on_fire` callback (`timer_runtime.cpp:123-125`)
2. `timer_runtime_t::post_fire_count` (`:146-193`)
3. timer 전용 queue 또는 Spot serial queue에서
   `timer_runtime_t::dispatch_fire_count_async` 실행 (`:167-179`, `:325`)
4. timer application handler 호출 (`:394-398`)

#### 브리지

| # | 지점 | 분류 | 조건과 빈도 |
|---:|---|---|---|
| 1 | Spot serial queue snapshot (`timer_runtime.cpp:181-186` → `spot_runtime.cpp:2110-2114`) | `[매번]` spot-wide | native fire batch를 Spot queue에 게시할 때 통과 |
| 2 | callback admission·depth 증가 (`timer_runtime.cpp:356` → `spot_runtime.cpp:1988-1995`) | `[매번]` | batch의 handler loop 진입 전에 통과 |

spot-wide는 **2회**, timer 전용 queue를 쓰는 `per_actor`는 #1이 없어 **1회**다. bounded
catch-up이 한 fire batch에서 여러 tick을 만들면 #1과 #2는 batch의 첫 handler 전에 한 번만
통과하고, 같은 loop의 후속 tick 호출에는 추가되지 않는다 (`timer_runtime.cpp:380-399`). Timer
등록과 native timer 생성 (`timer_runtime.cpp:53-128`)은 `[초기화]`이며 발화 계수에는 없다.

### A.3 중첩 브리지

선택한 네 경로에서는 **`lane A.run(...).get()`의 lambda 안에서 다시
`lane B.run(...).get()`을 호출하는 중첩 브리지를 찾지 못했다.** 따라서 “lane A 완료를 기다리는
thread가 lane B 완료까지 함께 묶이는” 두 state lane 동기 대기는 0건이다.

다만 다음 실행기 중첩은 있다. 이것은 두 번째 동기 state-lane 대기가 아니므로 주 계수와 중첩
브리지 수에는 넣지 않았다.

- spot-wide Actor packet은 Actor serial queue turn에서 Spot serial queue로 비동기 게시한다
  (`spot_runtime.cpp:3864-3908`). 그 게시 과정이 callback lane snapshot을 동기 대기한다.
- Spot route와 spot-wide timer도 Spot serial queue에 비동기 게시한 뒤, handler turn에서
  callback lane admission을 동기 대기한다 (`spot_runtime.cpp:3917-3967`,
  `timer_runtime.cpp:181-186`, `:356`).

즉 queue turn을 보류하는 계층 중첩은 있지만, outer state lane의 `.get()`이 inner state lane의
`.get()`을 감싸 thread 두 개를 동시에 묶는 형태는 아니다.

### A.4 회수 우선순위

| 우선순위 | 경로 | `[매번]` 수 | 제거 난이도 | 판단 |
|---:|---|---:|---|---|
| 1 | 원격 Actor request/send | 13/11 | 높음 | 최고 빈도 경로이고 같은 authority fence를 두 번 읽는다. 다만 relocation·Message Follow·exactly-once·Actor/Spot 이중 queue 불변식을 함께 보존해야 한다. 먼저 `:9617`과 `:9707` projection 통합, `:9845`와 `:10024`의 materialization/dispatch projection 통합 가능성을 검토할 가치가 크다. |
| 2 | Actor join | 7 | 중간~높음 | handler 전 node lane read가 7회다. context/factory/instance/admission을 한 immutable projection으로 줄일 여지가 크지만 factory와 application callback은 반드시 lane 밖에 남겨야 한다. |
| 3 | Spot route dispatch | 5 | 중간 | route client·context·Location lifecycle을 수신 시점의 검증된 projection으로 묶고, callback admission을 이미 존재하는 Spot serial queue 소유로 옮길 수 있는지 검토한다. close/idle-eviction fence가 난점이다. |
| 4 | timer fire | 2/1 | 낮음~중간 | 별도 node lane이 없고 callback lane만 남는다. timer/Spot serial queue가 callback admission 상태까지 소유하도록 만들 수 있으면 회수 폭은 작지만 위험도도 비교적 낮다. |

회수의 첫 목표는 bridge primitive 자체를 바꾸는 것이 아니라, 이미 같은 메시지에서 반복하는
node-state projection을 한 turn으로 합치고 callback admission을 상위 serial 실행 단위가
소유하게 만드는 것이다. 새 lock이나 별도 cache를 추가하면 경로 비용만 다른 형태로 옮길 수 있다.

---

## 부록 B — 게이트 매트릭스와 알려진 기존 실패 (concurrency-redesign/rules §4에서 이관)

회귀 판정의 기준선이다. 여기 등재된 실패는 회귀로 읽지 않는다.

**게이트는 언제나 Claude가 중앙에서 돌린다.** 에이전트 보고가 여러 번 사실과 달랐다.

| 언어 | 단위·계약 | 7샘플 일괄 | 비고 |
|---|---|---|---|
| dotnet | `dotnet test tests/Zlink.Framework.UnitTests` | `framework/languages/dotnet/samples/run_samples.sh` | 기준 1893+ / 실패 0 |
| cpp | `ctest --test-dir build -L 'framework-(unit\|contract)' -LE 'e2e\|sample\|perf'` | `framework/languages/cpp/samples/run_samples.sh` | flake: Bingo 후반 ~1/5, TTT teardown ~1/15. exit 86/134는 1회 재실행 |
| jvm spring | `:zlink-framework-spring-boot-starter:test`(39) | — | **2026-08-27 캠페인 범위 편입** — jvm R 전환이 `ZLinkRouteMeshRuntimeService`를 건드렸다 |
| java | `./gradlew :zlink-framework-core:test` | `framework/languages/java/samples/run_samples.sh` | 러너가 java→kotlin 순차. **기준 1149/실패 0**(2026-08-27 cleanTest 실측) |
| jvm 추가 모듈 | `:zlink-stream-connector:test`(123) · `:zlink-framework-locations-redis:test`(27) | — | **2026-08-27 캠페인 범위 편입** — jvm 결함 6건 수정이 이 두 모듈을 건드렸다. 종전 매트릭스에 없었다 |
| kotlin | `./gradlew :zlink-framework-kotlin:test` | (같은 러너) | `ZLINK_SAMPLE_LANGUAGES`로 분리 가능 |
| node | `npx tsc -b tsconfig.build.json --force` 후 `node --test test/contract/*.test.js` + verify:m6a/b/c | `framework/languages/node/samples/run_samples.sh` | **`--force` 필수** |

cross-language e2e: `framework/languages/cpp/cross-language/run_cross_language_smoke.sh`
(`ZLINK_CPP_BUILD_DIR=../build`) — 모든 언어 락을 잡고 단독으로. CP3/Z0 전용.

**Z0 함정 (2026-08-27 발견 — 반드시 확인)**

- **java cross-language Host는 소스가 아니라 발행된 maven 산출물에 의존한다.**
  `cross-language/Host/build.gradle.kts`가 `systems.zlink:zlink-framework-core:0.10.0`을 쓰고,
  설치본(`build/install/.../bin/zlink-cross-language-host`)이 그 jar를 물고 있다. 발견 시점의
  설치본은 **2026-08-25자**였다. **`./gradlew publishToMavenLocal`을 선행하지 않으면 java
  스테이지가 수정 이전 코드로 통과 판정을 받는다.** cpp host도 타겟 재빌드가 필요하다.
- **`MSBUILDDISABLENODEREUSE=1`을 반드시 설정한다** (2026-08-27 실증). "dotnet 무관 스테이지만
  고르면 dotnet은 안 돈다"는 가정이 틀렸다 — `relocation` 등 일부 스테이지가 내부적으로 dotnet
  테스트 호스트를 빌드하고, MSBuild 노드가 재사용을 위해 죽지 않고 **언어 락 3개를 계속 붙든다**.
  실측: `relocation`이 rc=0으로 끝났는데 MSBuild 노드 6개가 cpp·jvm·node 락을 9분 넘게 쥐고 있어
  다음 스테이지(`java-cross`)가 진입하지 못했다. 해당 PID를 kill하니 즉시 진입했다.
  다른 세션이 dotnet을 쓰는 중이면 충돌 위험도 있다.
- 스테이지는 `ZLINK_CPP_CROSS_LANGUAGE_STAGE`로 선택한다(기본 `all`). 전체 20개 중 **9개가
  dotnet을 상대**로 한다. dotnet 무관 10개:
  `spot-route` · `message-follow` · `relocation` · `java-cross` ·
  `user-spot-join-{cpp-java, java-cpp, cpp-node, node-cpp, java-node, node-java}`.
  dotnet이 다른 세션 소관이거나 락이 점유된 경우 이 10개로 cpp·node·jvm 간 wire 계약을 덮는다.

체크포인트: CP1(요청 1건)=에이전트 집중 테스트, CP2(배치)=Claude 단위·계약+7샘플,
CP3(마일스톤)=+cross-language e2e+스냅샷 재측정.

#### 알려진 기존 실패 (게이트 판정 제외 — 회귀로 읽지 말 것)

- **ZoneWorld**: cpp split-brain·dotnet mesh admission (Z1·Z2). CP3에서 판정, §3.1에서 수정.
- **jvm `ZLinkMicrometerMetricSinkTest.exportsExactHostCapacityCatalogFromTheSingleStatusProjection`**
  (2026-08-27 이분 실증): `expected: <5.0> but was: <NaN>`로 간헐 실패한다. 메트릭 집계 타이밍
  의존이다. **전환분을 되돌린 기준선에서 실패하고 적용 상태에서 통과**했으며, 적용 상태 3회
  반복도 전부 통과했다. 전환과 무관한 flake다.
- **cpp Bingo 샘플 `wait observer Entry Spot return` 정지** (2026-08-27 대조 실증):
  후반 관측자 Entry Spot 복귀 대기에서 오류 없이 멈춘다(placement marker 미도달).
  **전환 이전부터 있던 실패다** — 소형 배치 커밋 전 11:41 실행과 전환 250여 취득 완료 후
  18:28 실행이 **정확히 같은 step에서 정지**했다. 오늘 4회 중 4회 실패로, rules가 종전에
  적어 둔 "후반 ~1/5 flake"보다 나쁘다. flake가 아니라 결정적 실패로 성격을 재분류한다.
  원인 조사·수정은 캠페인 범위 밖 — 별도 작업으로 이월.
- **cpp `test_cpp_framework_host_lifecycle` 간헐 실패** (2026-08-27 이분 실증): 별도 worktree의
  **기준선(커밋된 배치 상태)에서 ×3 중 1회 실패**했다. 즉 `public_host_runtime`·`spot_runtime`
  전환이 만든 회귀가 아니라 원래 있던 flake다. 중앙 게이트 1차에서 실패해도 개별 재실행에서
  통과하면 flake로 판정한다(실측: 18:03 1차 실패 → 재실행 rc=0).
  **주의**: 이 항목은 세션 중 "격리 ①에서 실패 목록에 없었으니 이후 전환이 원인"으로 오판된
  전례가 있다. 한 번 통과한 것은 flake가 아니라는 증거가 되지 못한다 — 기준선에서 반복 측정해야 한다.
- **cpp `test_cpp_framework_m6b_runtime`**: Subprocess aborted(exit 86/134 계열). 재실행 통과 확인 후 무시.
- **cpp `test_cpp_framework_layout_contract`**: ShoppingMall OrderWorkflow main.cpp L350·L446
  blocking `result()` 지문 — base `3cbfbde4f9`부터. 샘플 수정은 별도 작업.
- **node `test/contract/*` 23건** (2026-08-27 기준선 대조로 확정): 계약 스위트 전수 실행 시
  1533건 중 **23건 실패**. node 결함 5건 수정을 되돌린 base 상태와 **실패 이름 집합이 완전히 동일**
  (기준선에만 있는 것 0·현행에만 있는 것 0, 수정 후에는 +5 테스트/+5 통과/실패 동수).
  즉 전부 캠페인 이전부터 있던 기존 실패다. 계열: 샘플 게이트(Bingo·GameQuest·ShoppingMall·
  ZoneWorld·run_samples 자체검사), stream/session teardown·통지, relocation seal, spot managed timer,
  canonical spec 트리. **이 스위트를 캠페인 중 전수로 돌린 기록이 없어 그동안 계수되지 않았다.**
  개별 판정·수정은 캠페인 범위 밖 — 별도 작업으로 이월.
  **이름 기준선(2026-08-28 전수 실측, 26건 — 종전 23건과 같은 계열, 병렬 부하에 따라 ±수 건 변동)**:
  Bingo rolling replacement·Bingo room leave lifecycle·GameQuest topology·ShoppingMall topology·
  ZoneWorld maintenance/node status/replacements 3건·sample-zoneworld-domain(파일 로드, 단독 통과)·
  Session Actor relay route·Session owner replaces route·Session relocation retain identity·
  ZLinkRoutePacketDispatcher drops route requests(flowCreationEnabled undefined)·
  canonical common spec owns server semantics·command 42 sender deadline·
  common-spec samples entrypoints·run_samples 자체검사·samples readiness sleeps·
  old route disconnect terminal seal·physical stream disconnect 2건·
  remote Actor Join legacy fence ProtocolError·sample wire names·
  spot managed timer 2건(overrun·stopOnUnhandledException — 단독 재실행에서도 실패)·
  stream connector inconsistent frame·stream session cleanup/onDisconnected 2건·
  unbind relocation seal. 회귀 판정은 이 이름 집합과의 대조로만 한다.
- **node `verify:m6c-runtime` 2건** (stash 대조로 baseline 동일 110/112 확인): ① legacy fence
  불완전 시 ProtocolError 기대 vs actorType 경로(m6c-actor-join-store-resolution L126 vs
  remote-actor-join-receiver L63) ② retain identity의 coordinator fence 기대 vs codec 의도적
  제외(m6c-relocation-wire-codec L415 vs service-stateful-wire-codec L354). 계약 판정 필요 —
  별도 작업.
- **java full-run flake**: `ZLinkJavaRawMeshNodeM6ATest.descriptorBackedPeerIntent…`,
  `ZLinkAsyncSerialQueueTest.queuedRelocationIntent…` — 단독 재실행 통과 확인 후 무시.
  **`ZLinkJavaRawSpotNodeM6BTest.remoteSpotSendAndRequestUseTheExactRouteFence` 추가**
  (2026-08-27 실증): jvm 결함 6건 수정 잡의 full-run에서 1회 실패(`ExecutionException:
  ZlinkRequestException`, `ZLinkJavaRawMeshNode.requestSpot:1812`). 같은 트리에서 **단독 ×3
  전부 통과 + 전체 core 스위트 ×2 전부 통과 = 5/5 그린**으로 재현 실패. 수정 6건의 호출
  스택에도 없다. 위 두 건과 같은 full-run 부하 한정 flake로 판정.
- **cpp TicTacToe `JoinGameNotify` 간헐 유실도 기존 결함이다** (2026-08-27 worktree 이분 실증 —
  배치9 이전 시점에서 6회 중 4회 동일 실패): detached one-way bound-session 전달이 remote
  submit 성공을 delivery로 간주, owner 측 stream write 미확인. 근본 수정은 owner-side delivery
  계약 설계 필요(스펙 판정 대상) — 캠페인 범위 밖 이월. 가설 수정 2회는 실측 기각·원복.
- 간헐 실패는 이 목록과 대조 전에는 회귀로도 flake로도 단정하지 않는다.

---

## 부록 C — lane 전환 절차 (스펙 06 → rules §10 → 여기로 이관)

P1-4처럼 lock을 state lane으로 옮기는 작업이 따르는 절차다. 공개 계약이 아니라 작업
지시이므로 스펙이 아닌 이 플랜이 소유한다.

아래는 **전환 작업자용 절차**다. 스펙 06 §7·§8에 있던 것을 여기로 옮겼다 — 공개 계약이
아니라 캠페인 작업 지시이고, 보고 형식까지 담고 있어 스펙에 둘 자리가 아니었다.

영구 규칙은 스펙에 남겼다 — "반환 전 완료 보장을 보존한다"와 "공개 동기 계약을 lane 도입만을
이유로 비동기로 바꾸지 않는다"는 [스펙 06 §5 「반환 전 완료 보장」](../../framework/doc/framework/common/spec/server/01-execution/06-state-ownership-and-lanes.ko.md#반환-전-완료-보장),
교차 불변식이면 한 ownership region으로 합친다는 규칙은 [스펙 06 §4](../../framework/doc/framework/common/spec/server/01-execution/06-state-ownership-and-lanes.ko.md#4-상태-분류와-판별-기준)가 갖는다.

#### C.1.1 시그니처 전환 규칙

상태 접근 메서드의 시그니처는 다음 규칙에 따라 전환한다.

- **동기 반환을 비동기 반환으로 바꾸는 것을 허용한다.** 호출자가 그 값을 이미 비동기
  경로 안에서 쓰고 있었다면 그 값은 애초에 스냅샷이었다 — 반환 방식을 비동기로 맞추는
  것은 기존 관측 가능한 동작을 바꾸지 않는다.
- **out 파라미터는 반환값으로 합친다.** 하나의 반환값에 성공 여부와 결과를 함께
  담는다.
- **실패 시에도 값을 돌려주던 out은 nullable scalar로 기계 치환하지 않는다.** 성공
  여부와 값을 하나의 스칼라로 뭉치면, "실패했지만 그 실패에 딸린 값도 함께 필요한"
  경우를 표현할 수 없다 — 예를 들어 거절되었더라도 그 시점의 현재 high-water 값을
  호출자가 여전히 받아야 하는 경우, nullable scalar 하나로는 성공 값과 실패 시 부가
  값을 동시에 담지 못한다. 이런 경우는 성공 여부와 값(그리고 실패 시 부가 값)을 함께
  담는 결과 타입을 만들어 보존한다. 기계적인 nullable 치환은 관측 가능한 동작을
  바꾸므로 허용하지 않는다.
- **반환 전 완료 보장을 보존한다.** 원본 동기 메서드가 waiter 등록, epoch·generation
  캡처, store 판독 또는 exact ownership claim을 반환 전에 완료했다면, 전환 뒤에도
  caller가 반환을 관찰하기 전에 그 작업이 완료돼 있어야 한다. 비동기 fire-and-forget
  게시로 바꾸지 않는다.
- 이 보장을 유지하기 위해 동기 호환 경계가 필요하면 [스펙 06 §5 「완료 신호와 블로킹 호환 경계」](../../framework/doc/framework/common/spec/server/01-execution/06-state-ownership-and-lanes.ko.md#완료-신호와-블로킹-호환-경계) 조건을 확인하고 사유를 기록한다. 완료 신호를
  기다리는 이후 단계는 비동기로 남길 수 있지만, 등록·캡처 자체를 반환 뒤로 미루지는
  않는다.
- 공개 또는 언어별 exact interface가 동기 계약이면, state lane 도입만을 이유로
  Promise나 Task 반환으로 바꾸지 않는다. 내부 호출자가 이미 비동기이고 관측 계약이
  변하지 않을 때만 async signature를 전파한다.

#### C.1.2 전환 단위와 계수·보고

- **전환 경계는 기존 gate가 소유하던 상태 영역을 그대로 쓴다.** 클래스 하나에 서로
  독립적인 gate가 여러 개 있었다면, 각각을 별도 ownership region으로 옮길 수 있다. 이
  경우 두 영역에 걸친 field·collection 불변식이 없고, 영역 사이의 호출 방향이
  단방향임을 기록한다.
- 교차 불변식이나 양방향 대기가 하나라도 있으면 여러 lane으로 나누지 않고 한
  ownership region으로 합친다. "클래스 하나"는 기본 작업 단위일 뿐, 한 클래스 안에
  근거 없이 여러 state lane을 만드는 허가가 아니다.
- socket·completion·worker 같은 작업 프로토콜 gate는 [스펙 06 §4 「상태 보호와 작업 프로토콜 직렬화를 구분한다」](../../framework/doc/framework/common/spec/server/01-execution/06-state-ownership-and-lanes.ko.md#상태-보호와-작업-프로토콜-직렬화를-구분한다) 조건을 만족할
  때만 state lane 전환 대상에서 제외한다. 제외 사유에는 ownership transfer,
  generation fence, completion 방식과 lock-order를 기록한다.
- **전환마다 검증을 통과해야 다음으로 간다.** 확인할 항목은
  [스펙 06 §8 검증 요구](../../framework/doc/framework/common/spec/server/01-execution/06-state-ownership-and-lanes.ko.md#8-검증-요구)가 소유한다.
- **성공 지표는 lock 개수가 아니다.** 배타적 접근 문의 개수가 줄어든 것은 증거가
  아니다. 줄여야 하는 것은 "async 경계를 넘어 쓰이는 스냅샷"의 수이며, 이 수를 컴포넌트
  단위로 전후 비교한다. 이 비교는 공개 표면이 아니라 내부 계측으로 확인하는 **내부
  확인 조건**이며, 검증 요구 절에는 두지 않는다.

Async 경계 snapshot의 계수 단위는 source의 배타적 접근 위치다. 단순 문자열 검색
결과가 아니라 실제 언어 token을 센다. 각 위치에서 배타적 접근 안에서 산출한 값·참조·
결정이 다음 중 하나를 넘어 쓰이는지 추적한다.

- `await` 또는 Task·Promise·future 반환
- detached task, queue, worker thread 또는 callback dispatcher 제출
- 비동기 continuation을 실행하는 completion signal
- nonblocking transport operation 제출

그 경계를 넘더라도 immutable completion signal, exact token, reservation 또는 단독
ownership transfer로 유효성이 고정되면, primitive/protocol 제외군으로 따로 센다.
Mutable authorization을 그대로 넘겨 쓰면 잔존 결함이다. 최종 보고는 `전체 / 제외군 /
잔존 결함` 세 값을 모두 적는다.

#### C.1.3 전환 검증 (게이트)

- 전환 전후 그 언어의 단위 테스트 전체가 그린이다.
- 전환 전후 샘플 게이트가 유지된다.
- 전환 전후 caller가 관측하는 순서·타임아웃·오류 코드가 바뀌지 않는다.
- async 경계 snapshot 재측정에서 잔존 결함이 0이고, primitive/protocol 제외 위치에는 각각
  유효성 보존 근거가 기록돼 있다.
