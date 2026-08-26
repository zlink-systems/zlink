# 진행표 — lane 소유 동시성 전환

> **이 문서는 진행표만 담는다. 상태 갱신은 여기서만 한다.**
> 규칙·프로토콜·요청 템플릿·발견 로그는 [rules.ko.md](rules.ko.md),
> 배경·이어받기는 [handoff.ko.md](handoff.ko.md), 순서 근거는
> [conversion-order-survey.ko.md](conversion-order-survey.ko.md)와 l1/l2-survey-*.ko.md.

브랜치 `refactor/lane-ownership-concurrency` · 시작 2026-08-26

상태 값: `대기` → `요청됨`(잡 실행 중) → `보고됨`(CP1 대기) → **`완료`**(리뷰 승인·커밋) / `반려`(재요청) / `제외`·`해당없음`(사유 명시)
갱신 시점: 잡 기동=요청됨 · 보고 수신=보고됨 · CP1 승인·커밋=완료 · CP2 게이트=지표 표 재측정. 해당 작업 커밋과 같은 커밋에서 갱신한다.

## 1. 전체 매트릭스 — 행=작업 항목, 열=언어

jvm 열은 java·kotlin을 함께 적는다(빌드 트리 공유). 언어별 클래스 전환의 요청 단위 상세는 §2.

| 작업 항목 | dotnet | cpp | jvm (java·kotlin) | node |
|---|---|---|---|---|
| **L0** primitive + golden 14 | **완료** | **완료** (14/14) | java **완료**(+발견5 보완) · kotlin **완료**(14/14) | **완료** (12, 동시성 2 제외·사유 주석) |
| **L1** 표본 전환 | **완료** `3cc6f5f615` | **완료** `f4d0df006b` (registry 31→0, 반려 1) | java **완료**(SessionActorsRuntime 22→0) · kotlin **해당없음**(자체 C2 상태 0) | **완료** `ca32669168` (registry, 13파일 전파) |
| **L2** 순서 조사 | **완료** (conversion-order-survey) | **완료** (후보 19 — l2-survey-cpp) | **완료** (후보 23+9 — l2-survey-java) | **완료** (l2-survey-node) |
| **L2** 클래스 전환 (§2 상세) | **진행 중** — 완료 7 · 요청 1 · 대기 ~52 | 대기* | 대기* (kotlin은 java 승계) | 대기* |
| **마무리** 호환 경계 회수 | 대기 (AwaitStateLane 121+) | 대기 (.get 사유 기록) | 대기 (inStateLane join) | **해당없음** (L1에서 완전 전파) |
| **마무리** posddd ② 잔여 | 대기 (최우선: OperationLease 할당) | 대기 | 대기 | 대기 |
| **마일스톤 게이트** (CP3 — unit·계약+7샘플+스냅샷 재측정+sol 리뷰) | 대기 (+§8 결함 판정·스펙 06 개정) | 대기 | 대기 | 대기 |
| **Z0** cross-language e2e 그린 (샘플 작업 선행) | 대기 — 전 언어 락 단독, 공동 1회 | ← | ← | ← |
| **샘플** TicTacToe | 대기 | 대기 | 대기 | 대기 |
| **샘플** Bingo | 대기 | 대기 (flake ~1/5 주의) | 대기 | 대기 |
| **샘플** SupportChat | 대기 | 대기 | 대기 | 대기 |
| **샘플** ShoppingMall | 대기 | 대기 (layout_contract 기존 실패 — rules §4) | 대기 | 대기 |
| **샘플** DeliveryDispatch | 대기 | 대기 | 대기 | 대기 |
| **샘플** GameQuest | 대기 | 대기 | 대기 | 대기 |
| **샘플** ZoneWorld (Z1~Z4) | 대기 — Z2 mesh admission 수정 | 대기 — Z1 split-brain·Z3 fast_mutex 수정 | 대기 | 대기 |

\* L2 클래스 전환 진입 조건: 해당 언어 L1 완료 + dotnet 마일스톤 게이트 통과 (rules §2).
샘플 행은 최종 단계에서 **1개 완료마다 감독관 리뷰** 후 다음으로(rules §3.1). ZoneWorld 외
6샘플은 CP2/CP3 일괄 게이트로는 이미 검증 중이며, 이 행들은 최종 단계의 샘플별 확인 작업이다.

## 2. 언어별 L2 클래스 전환 상세 (요청 단위)

### dotnet

| # | 클래스 (lock) | 상태 | 커밋 | 비고 |
|---|---|---|---|---|
| 표본 | ZLinkSessionActorBindingTable (30) | **완료** | `3cc6f5f615` | sol. 재진입 2, 호환 경계 25 |
| 1 | ZLinkInMemoryLocationStore (27) | **완료** | `2bdf463ee3` | 재진입 0, posddd ② 생략(근거 커밋에) |
| 2 | ZLinkActorHandoffAdmissions (25) | **완료** | `326133ca03` | SuppressFlow 1, 호환 경계 4 |
| 3 | ZLinkActorOwnershipCoordinator (26) | **완료** | `e1f5ade349` | 재진입 8 해소, 호환 경계 3, Task.Yield 보강(발견4) |
| 4 | ZLinkSpotNodeCatalog (48) | **완료** | `0fef22fa62` | SuppressFlow 5, 호환 경계 9, dispose fix `1bd23dc1ff` |
| 5·6 | ZLinkClientServerClientRuntime + Connection (20+44) | **완료** | `39d71acd6c` | lane 2, SuppressFlow 7, 호환 경계 4, _socketLifecycleGate 유지 |
| CP2-1 | 배치 1 게이트 (#3~#6) | **통과** | unit 1893/0 · 7샘플 exit 0 | 2026-08-26 |
| 7 | ZLinkActorHandoffState (58) | **완료** | `b49b1ef55d` | 재진입 3(유형③ — 발견6), 호환 경계 63, 반려 2회. unit 1893/0 |
| 8 | ZLinkActorRuntimeState (35) | **요청됨** | | codex terra (2026-08-26). semaphore=작업 직렬화 유지 판정(발견7), 재진입 ~20 |
| 9 | ZLinkManagedMeshNode (130) | 대기 | | 4×+, 정독 리뷰 필수 |
| 10 | ZLinkFrameworkRuntime (27) | 대기(최종) | | 사용 45파일, 재진입 ~57, 정독 리뷰 필수 |
| — | 소형 잔여 ~50클래스 (lock<20, ~300) | 대기 | | 3~5개 배치, C1/C3 다수 예상 |
| 제외 | ZLinkSerialExecutionQueue (25) | 제외 | | lane primitive 자체 (스펙 06 §9) |

### cpp · java · node

L2 개방 시(dotnet 마일스톤 후) 각 survey의 후보 순서대로 요청 단위 행을 여기에 만든다.
후보 목록: cpp = l2-survey-cpp.ko.md(19개), java = l2-survey-java.ko.md(23+9개),
node = l2-survey-node.ko.md. kotlin은 java 승계로 상세 없음.

## 3. 지표 (CP2마다 재측정 — 명령은 rules §6. 마지막 2026-08-26)

| 지표 | 시작 | 현재 | 목표 |
|---|---:|---:|---|
| dotnet `lock` 문 | 999 | **783** | ~60 (lane·큐 내부만) |
| `_gate` 보유 클래스 | 61 | **55** | 0 |
| state lane 사용 파일 | 0 | **7** | — |
| **async 경계 스냅샷** *(진짜 지표)* | ~465 | 미측정 | **0** |
| 호환 경계 `AwaitStateLane` *(부채)* | 0 | **58** | 0 (마무리에서 회수) |
