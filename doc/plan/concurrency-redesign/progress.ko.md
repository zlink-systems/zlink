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
| **L2** 클래스 전환 (§2 상세) | **완료** (대형 10+소형 22배치) | **완료** (전환 15+영역 판정 종결 — 잔여 113취득=작업 순서 제외, 보류 2 구조 판정) | **완료** (소진 — 소켓 2종·primitive 4종 제외 판정, kotlin 승계) | **완료** (소진 — STOP 9=executor 순서 제외군, 불요 5, m6b 회귀 수정 포함) |
| **마무리** 호환 경계 회수 | **판정 완료** — 실질 스냅샷 0 달성(cp3-audit), 경계 656은 §5 조건 충족 시 사유 기록 잔존·핫패스 회수는 이월 | 대기 | 대기 | **해당없음** |
| **마무리** posddd ② 잔여 | **최우선 완료**(OperationLease 0B 검증) — 나머지는 이월 후보 | 대기 | 대기 | 대기 |
| **마일스톤 게이트** (CP3) | **대부분 완료** — unit 1900/0·6샘플·cross-language e2e 그린, 스냅샷 실질 0, sol 감사·스펙 06 개정(`078d1e22b6`) 완료. §8 판정: Z2 부분 하위증상 입증(0/3→3/5) | 대기 | 대기 | 대기 |
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
| 8 | ZLinkActorRuntimeState (35) | **완료** | `b78c555cf7` | semaphore=작업 직렬화 유지(발견7), 재진입 다수 OnLane core 분리, SuppressFlow+Task.Run 4, 호환 경계 38. 반려 1(ExecuteLockedAsync 안 public 재호출 데드락 — 행 덤프로 진단). unit 1893/0·행 없음 |
| CP2-2 | 배치 2 게이트 (#7·#8) | **통과** | unit 1893/0 · 6샘플 placement OK (ZoneWorld=기존 Z2 증상, 제외) | 2026-08-26. 지표 재측정 690/54/9/128 |
| 9 | ZLinkManagedMeshNode (130) | **완료** | `b3497532a6` | 4잡 연속. lane 5개 분리+ManagedActor별 lane, _socketGate·_disposeGate 유지(발견7), 호환 경계 25. unit 1893/0·행 없음 |
| CP2-3 | 배치 3 게이트 (#9·소형1) | **통과** | unit 1893/0 · 7샘플 전부 placement OK (ZoneWorld 포함 — Z2 간헐성 증거) | 2026-08-26 |
| 10 | ZLinkFrameworkRuntime + FrameworkComponentState + ChannelRuntimeManager SyncRoot | **완료** | `ecbe9dfcb6` | 2잡+반려 1(부정확 보고 검출 — 발견9 fence·deadline 재확인). 호환 경계 58. unit 1899/0 |
| — | 소형 배치 1 (4클래스 — C3·C1 기계 전환) | **완료** | `960be4c785` | 재진입 0, 호환 경계 0 |
| — | 소형 배치 2~5 (15클래스 전환 + CompletionDispatcher 제외 재분류) | **완료** | 커밋 3건 | CP2-4 unit 1896/0·6샘플 OK. 반려 2(NRE 테스트·무할당 계약 위반→원복) |
| — | 소형 배치 6~9 (17클래스) | **완료** | 합본 커밋 | CP2-5 unit 1896/0·6샘플 OK. 반려 1(dispose Task memoization) |
| — | 소형 배치 10~15 (23클래스) | **완료** | `135c134b2c` | CP2-6 unit 1896/0·6샘플 OK. 반려 1(Bundle dispose 발견7 역방향 데드락). ChannelRuntimeManager·FrameworkComponentState는 #10 범위 재분류 |
| — | 소형 배치 16~19 (11클래스 + 제외 4) | **완료** | `aed9ee8d05` | CP2-7 unit 1899/0·7샘플 exit 0. 반려 3(발견9 계열 2·false 관측 보존 1). 제외: MeshCompletionTable·WorkerPool·SerialWorkItem·SpotSerialExecutor(primitive) |
| — | 소형 배치 20~22 (+MonitorHub 이월, 12클래스) | **완료** | `90ecdaedc0` | CP2-8 unit 1899/0·6샘플 OK. 반려 2. 잔여: SpotRetireTargetRuntime outer registry(#10 부근 정리) |
| 제외 | ZLinkSerialExecutionQueue (25) | 제외 | | lane primitive 자체 (스펙 06 §9) |
| 제외 | ZLinkCompletionDispatcher (5) | 제외 | | dispatch primitive — 무할당 hot path 계약(0→408 실측)이 lane 클로저와 양립 불가 (CP2-4 판정) |

### cpp · java · node

L2 개방 시(dotnet 마일스톤 후) 각 survey의 후보 순서대로 요청 단위 행을 여기에 만든다.
후보 목록: cpp = l2-survey-cpp.ko.md(19개), java = l2-survey-java.ko.md(23+9개),
node = l2-survey-node.ko.md. kotlin은 java 승계로 상세 없음.

## 3. 지표 (CP2마다 재측정 — 명령은 rules §6. 마지막 2026-08-26)

| 지표 | 시작 | 현재 | 목표 |
|---|---:|---:|---|
| dotnet `lock` 문 | 999 | **118** (−88%) | ~60 (lane·큐 내부만) |
| `_gate` 보유 클래스 | 61 | **6** | 0 |
| state lane 사용 파일 | 0 | **77** | — |
| **async 경계 스냅샷** *(진짜 지표)* | ~465 | 미측정 | **0** |
| 호환 경계 `AwaitStateLane` *(부채)* | 0 | **656** | 0 (마무리에서 회수) |
