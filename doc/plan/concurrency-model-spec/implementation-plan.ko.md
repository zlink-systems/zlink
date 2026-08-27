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

## 1. 착수 순서

**P0은 나머지와 독립이다.** 구조를 바꾸지 않고 이미 정해진 규칙(스펙 07 §7)을 지키는 일이므로
P1~P4와 병행해도 되고 먼저 해도 된다.

**P1을 P2·P3보다 먼저 한다.** dotnet이 세 계층 조율자를 모두 갖고 있는 유일한 언어이고, java와
cpp는 조율자를 신설해야 한다 — 신설할 때 볼 참조 구현이 P1에서 스펙 이름으로 정리된 상태여야
한다. 참조 구현 없이 신설을 맡기면 이번 캠페인에서 이미 네 번 실패한 자리다.

| 단계 | 내용 | 선행 | 병렬 |
|---|---|---|---|
| **P0** | cpp 핫패스 조회 묶기 · java binding wrapper 중복 lock · 큐 임계 구역 축소 | 없음 | P1~P4와 병행 가능 |
| **P1** | dotnet을 스펙 이름으로 정렬한다 (개명 · `_laneGate` 제거 · `ownerTimeBudget` 추가) | 없음 | 단독 |
| **P2** | java 세 계층 조율자 신설 | P1 | P3와 병렬 |
| **P3** | cpp 세 계층 조율자 신설 | P1 | P2와 병렬 |
| **P4** | node 큐 맵 3개를 조율자로 이관 | P1 | P2·P3와 병렬 |
| **P5** | 스펙 07 §10 검증 요구를 언어별 계약 test로 옮긴다 | P1~P4 | 언어별 병렬 |

P2·P3·P4는 서로 다른 빌드 트리를 쓰므로 동시에 돌린다. 같은 빌드 트리에 두 작업을 붙이지
않는다 — 유령 실패로 측정이 무효가 된다
([`../concurrency-redesign/rules.ko.md`](../concurrency-redesign/rules.ko.md) §5.2).

---

## 2. P0 — 구조와 독립인 선행 작업

| # | 작업 | 대상 | 기대 효과 |
|---|---|---|---|
| P0-1 | Spot 핫패스에서 연속된 단순 조회를 한 turn으로 묶는다 | cpp `runtime/spots/spot_runtime.cpp` | 블로킹 브리지 send 11→4~5 · request 13→5~6 |
| P0-2 | binding wrapper의 중복 lock 제거 | java `runtime/` binding wrapper 31곳 | hot path 7곳 |
| P0-3 | 큐 임계 구역에서 `BigInteger` 할당을 걷어낸다 | java `execution/ZLinkAsyncSerialQueue.java` | enqueue마다 4할당 제거 |

근거는 [spot-hotpath-bridge-survey.ko.md](spot-hotpath-bridge-survey.ko.md)에 있다. P0-1은
새 설계가 아니라 스펙 07 §7을 지키는 일이다 — cpp가 lane 전환 중 그 규칙을 어긴 자리를
되돌린다.

---

## 3. P1 — dotnet

dotnet은 세 계층 조율자와 큐 primitive를 모두 갖고 있다. 신설할 것이 없고 **이름을 스펙에
맞추고 빠진 계약 둘을 채운다.**

| # | 작업 | 파일 | 완료 판정 |
|---|---|---|---|
| P1-1 | `ZLinkActorDispatchMailbox` → `ZLinkActorSerialExecutor` | `Runtime/Actors/ZLinkActorDispatchMailbox.cs` | 이전 이름이 저장소에서 0건 |
| P1-2 | `ZLinkStreamSessionSerialExecutor` → `ZLinkSessionSerialExecutor` | `Runtime/Streams/ZLinkStreamSessionSerialExecutor.cs` | 이전 이름 0건 |
| P1-3 | Session 진입점 동사 `Enqueue*` → `Execute*` 넷 | 위 파일 | 스펙 07 §3 표와 일치 |
| P1-4 | `_laneGate` lock을 state lane 소유로 바꾼다 | `Runtime/Spots/ZLinkSpotSerialExecutor.cs:12,69,87,1108` | 그 파일에 `lock (` 0건 |
| P1-5 | `ownerTimeBudget` 정책과 양보 동작을 추가한다 | `Runtime/Execution/ZLinkSerialExecutionQueue.cs` | 스펙 07 §10 "공정성" test 통과 |

**P1-4는 세 곳이 아니라 네 곳이다.** `_laneGate`는 queue map을 지키는 collection lock이므로
스펙 06 §4의 C1이다 — 그대로 state lane 소유로 옮긴다. `close` 경로(`:87`)에서 map을 비우는
자리가 함께 움직여야 하는 값을 다루면 C2이므로, 그 turn 안에서 원자적으로 처리한다.

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

**P2-3은 분리이지 이동이 아니다.** `ZLinkStreamRuntime.stateLane`은 상태 소유와 작업 실행을
함께 지고 있다. 상태 소유는 그 자리에 남기고(스펙 06), 작업 실행만 새 조율자로 옮긴다 —
둘을 같은 객체에 두면 스펙 07 §1이 구분하는 두 문제가 다시 섞인다.

---

## 5. P3 — cpp

cpp도 조율자가 셋 다 없고, 큐 primitive에 수용량·우선순위·공정성이 없다. **P2보다 크다.**

| # | 작업 | 파일 | 완료 판정 |
|---|---|---|---|
| P3-1 | `spot_serial_executor_t` 신설 · `spot_runtime`의 이름 맵을 그 안으로 옮긴다 | `runtime/spots/spot_runtime.{hpp,cpp}` | 조율자 밖에서 Actor·timer 큐를 드는 곳 0건 |
| P3-2 | `actor_serial_executor_t` 신설 | `runtime/actors/` | 큐 맵 없이 인스턴스당 큐 하나 |
| P3-3 | `session_serial_executor_t` 신설 · `stream_runtime.dispatch_queue`에서 분리 | `runtime/streams/stream_runtime.{hpp,cpp}` | 진입점 넷이 스펙 07 §3과 일치 |
| P3-4 | 큐 primitive에 정책 주입 · lifecycle lane · `ownerTimeBudget`을 추가한다 | `runtime/execution/` | 스펙 07 §10 "수용량과 backpressure"·"공정성" test 통과 |
| P3-5 | 조회 스냅샷 묶기 (P0-1과 같은 작업 — 먼저 끝났으면 생략) | `runtime/spots/spot_runtime.cpp` | 같은 값을 두 번 읽는 자리 0건 |

**P3-4를 P3-1보다 먼저 한다.** 조율자가 큐를 소유하려면 그 큐가 정책을 받을 수 있어야 한다.
순서를 뒤집으면 조율자를 만든 뒤 큐 시그니처를 다시 바꾸게 된다.

**참조 구현을 반드시 명시해 맡긴다.** cpp `spot_runtime` 작업은 이번 캠페인에서 참조 없이
맡겼을 때 네 번 실패했고, dotnet·java 구현을 참조로 지정한 뒤에야 통과했다.

---

## 6. P4 — node

node는 Spot·Session 조율자가 있다. **문제는 없는 것이 아니라 흩어져 있는 것이다.**

| # | 작업 | 파일 | 완료 판정 |
|---|---|---|---|
| P4-1 | `actorSerials` 맵을 Spot 조율자로 옮긴다 | `runtime/spots/spot-activation-state.ts:93,406` | 조율자 밖 0건 |
| P4-2 | `timerSerials` 맵을 Spot 조율자로 옮긴다 | `runtime/spots/spot-activation.ts:471` | closure로 잡는 자리 0건 |
| P4-3 | `ZLinkActorDispatchMailboxSet`을 Spot 조율자로 옮기고 `ZLinkActorSerialExecutor`로 개명한다 | `runtime/actors/actor-mailbox.ts` · `spot-entry-activation.ts` · `spot-activation-state.ts` | 소유처가 조율자 하나 |
| P4-4 | `ZLinkBoundedSerialScheduler`의 정책·진입점을 스펙 07 §6과 맞춘다 | `runtime/execution/serial-scheduler.ts` | 정책 이름 일곱이 스펙과 일치 |

**P4에 넣지 않는 것** — `SpotWide`에서 Actor 작업을 Actor 큐에 거치게 하는 변경. node는 지금
공용 `serial`을 그대로 돌려주어(`spot-activation-state.ts:407`) Actor별 payload 상한이 걸리지
않는다. 그 상한이 `SpotWide`에서 필요한 보장인지가 미결이므로(스펙 07 §9), 정해지기 전에는
현행을 유지한다.

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
- **한 파일만 보고 "없다"고 판정한다.** node Actor 큐를 `spot-serial-executor.ts`만 보고
  없다고 적었으나 다른 세 파일에 있었다. 없다는 판정은 저장소 전체 검색으로만 내린다.

---

## 9. 미결 — 착수 전에 정해야 하는 것

| # | 미결 | 막는 단계 |
|---|---|---|
| ① | node `SpotWide`에서 Actor별 payload 상한이 필요한 보장인가 | P4의 범위 |
| ② | 계층별 정본 언어([[reference-first-porting-policy]]의 ".NET 단일 정본" 개정) | P2·P3의 참조 지정 |
| ③ | 호환 경계 회수 — dotnet `AwaitStateLane` 664 · cpp `.get()` 456 | P0-1 후 재측정 |

①·②는 P1과 무관하므로 **P1은 지금 착수할 수 있다.**

---

[작업 폴더 목차](README.ko.md) · [계약: 스펙 07](../../../framework/doc/framework/common/spec/server/01-execution/07-serial-executor-layers.ko.md) · [이름 계약서](executor-naming-contract.ko.md)
