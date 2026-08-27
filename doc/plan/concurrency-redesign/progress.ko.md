# 진행표 — lane 소유 동시성 전환

> **이 문서는 진행표만 담는다. 상태 갱신은 여기서만 한다.**
> 규칙·프로토콜·요청 템플릿·발견 로그는 [rules.ko.md](rules.ko.md),
> 배경·이어받기는 [handoff.ko.md](handoff.ko.md), 순서 근거는
> [conversion-order-survey.ko.md](conversion-order-survey.ko.md)와 l1/l2-survey-*.ko.md.

브랜치 `refactor/lane-ownership-concurrency` · 시작 2026-08-26

상태 값: `대기` → `요청됨`(잡 실행 중) → `보고됨`(CP1 대기) → **`완료`**(리뷰 승인·커밋) / `반려`(재요청) / `제외`·`해당없음`(사유 명시)
갱신 시점: 잡 기동=요청됨 · 보고 수신=보고됨 · CP1 승인·커밋=완료 · CP2 게이트=지표 표 재측정. 해당 작업 커밋과 같은 커밋에서 갱신한다.

## 0. 전체 진행률 (2026-08-27 세션 마감 시점 집계)

§1 매트릭스의 언어별 셀(언어당 15항목 — L0·L1·L2조사·L2전환·호환 경계·posddd·CP3·Z0·샘플 6·ZW,
node는 호환 경계 해당없음이라 14항목)을 직접 집계했다. 부분 완료(판정만 남음·수정 커밋됐으나
측정 미종결·감사 보고됨)는 0.5로 계산했다.

| 언어 | 완료/전체 | 진행률 | 완료된 것 | 남은 것 |
|---|---:|---:|---|---|
| **dotnet** | 14/15 | **93%** | 본체 전환 전부, 호환 경계·posddd 판정, 샘플 6종 그린, Z0, Z2 수정 | CP3 최종 판정(0.5)·**ZW Z4 이분 판정**(0.5 — 조용한 ×8=1/8, 기준선 worktree 준비됨) |
| **node** | 12/14 | **86%** | 본체 전환 전부, 샘플 6종 그린, Z0, CP3 감사 보고됨(NOT CLEAN) | **결함 의심 5건(H2·M3) 검증·수정** → CP3 판정, ZW 검증 |
| **cpp** | 6/15 | **40%** | 본체 전환 전부, Z0, TTT 기존 결함 판정, Z1·Z3 수정 | CP3 감사(재기동 필요), 샘플 5종 스위트 재실행(T5), ZW Z4 ×8 측정 |
| **jvm** (java·kotlin) | 5/15 | **33%** | 본체 전환 전부, Z0, teardown 수정 커밋(`e4944785a1`) | CP3 감사(재기동 필요), 샘플 6종 스위트 재실행(T5), T4 import 분류, ZW 검증 |
| **합계** | 37/59 | **약 63%** | | |

언어 간 차이의 원인: 본체 전환(공수의 대부분)은 4언어 모두 100%이고, 차이는 전부 **검증
꼬리**다. dotnet·node는 샘플 스위트가 이미 그린이고 감사까지 끝났지만, cpp·jvm은 감사와
스위트 재실행이 세션 종료로 미착수라 항목 수가 깎였다 — 실제 남은 공수는 cpp·jvm 각각
감사 1잡 + 스위트 1회 수준이다.

공수 기준으로는 다르다: 리팩토링 본체(전 언어 전환·스펙 06 개정·지표 목표 달성 — 캠페인
공수의 대부분)는 끝났고, 남은 것은 전부 **검증·판정·기록 작업**이다. 공수 가중으로는
**약 85~90% 지점**이며, 남은 작업 중 결과를 예측할 수 없는 것은 ZoneWorld Z4 이분 판정
(회귀로 판명되면 수정 작업이 추가된다)과 node 결함 의심 5건뿐이다.

## 1. 전체 매트릭스 — 행=작업 항목, 열=언어

jvm 열은 java·kotlin을 함께 적는다(빌드 트리 공유). 언어별 클래스 전환의 요청 단위 상세는 §2.

| 작업 항목 | dotnet | cpp | jvm (java·kotlin) | node |
|---|---|---|---|---|
| **L0** primitive + golden 14 | **완료** | **완료** (14/14) | java **완료**(+발견5 보완) · kotlin **완료**(14/14) | **완료** (12, 동시성 2 제외·사유 주석) |
| **L1** 표본 전환 | **완료** `3cc6f5f615` | **완료** `f4d0df006b` (registry 31→0, 반려 1) | java **완료**(SessionActorsRuntime 22→0) · kotlin **해당없음**(자체 C2 상태 0) | **완료** `ca32669168` (registry, 13파일 전파) |
| **L2** 순서 조사 | **완료** (conversion-order-survey) | **완료** (후보 19 — l2-survey-cpp) | **완료** (후보 23+9 — l2-survey-java) | **완료** (l2-survey-node) |
| **L2** 클래스 전환 (§2 상세) | **완료** (대형 10+소형 22배치) | **재판정 필요** — "잔여 113취득"은 위 계수 오류에 근거한 수치다. cp3-audit-cpp 실측: 상태 보호 잔존 **469**·실행 primitive 205·socket/dispose 프로토콜 230. 다만 **진짜 지표(경계 스냅샷)는 119곳 중 117 정당화·2 결함 의심**이라 전환 자체의 성패는 별개다(handoff §6: lock 수는 성공 지표가 아님) | **완료** (소진 — 소켓 2종·primitive 4종 제외 판정, kotlin 승계) | **완료** (소진 — STOP 9=executor 순서 제외군, 불요 5, m6b 회귀 수정 포함) |
| **마무리** 호환 경계 회수 | **판정 완료** — 실질 스냅샷 0 달성(cp3-audit), 경계 656은 §5 조건 충족 시 사유 기록 잔존·핫패스 회수는 이월 | **감사 완료** — cp3-audit-cpp: 블로킹 브리지 `lane.run(...).get()` **456곳**, 스펙 06 §5 조건은 정적 충족하나 동기 public 표면·return-before 계약 때문에 잔존. 전환으로 더 늘어남(예: location_lifecycle 13) — 핫패스 회수는 이월 | **감사 완료** — cp3-audit-jvm: 동기 호환 경계 **47곳**, 그중 `ZLinkStreamRuntime` pending-session `join()` 1건은 **결함으로 판정돼 수정**(자기 pending future join = 데드락, 스택 실증). 나머지 46은 사유 기록 잔존 | **완료** — 실증 5건 전부 수정·중앙 게이트 통과. 기준선 대조로 **회귀 0** 확정(base 1533/1510/fail 23 vs 현행 1538/1515/fail 23, 실패 이름 집합 동일 → +5 테스트 전부 통과). tsc `--force` rc=0, verify m6a 41/41·m6b 104/104·m6c 113/115(기존 2건). 계약 23건은 **기존 실패로 rules §4 등재**(캠페인 무관) |
| **마무리** posddd ② 잔여 | **최우선 완료**(OperationLease 0B 검증) — 나머지는 이월 후보 | **보고됨** (cp3-audit-cpp §7 — 상위 10 후보 기록, 프로파일 전이라 성능 결함으로 확정하지 않음. 측정은 이월) | **보고됨** (cp3-audit-jvm — 상위 10 후보 기록, 측정은 이월) | **보고됨** (cp3-audit-node §6 — 상위 10 후보 기록, 측정은 이월) |
| **마일스톤 게이트** (CP3) | **대부분 완료** — unit 1900/0·6샘플·cross-language e2e 그린, 스냅샷 실질 0, sol 감사·스펙 06 개정(`078d1e22b6`) 완료. §8 판정: Z2 부분 하위증상 입증(0/3→3/5) | **완료** — 결함 6건 전건 [실증] 수정 + 재진입 회귀 테스트 6건(`c7aab4fb57`). 게이트(cleanTest 강제·JUnit XML 직접 집계): core 1149/0 · kotlin 67/0 · connector 123/0 · redis 27/0. **되돌림 검증 통과** — 수정만 되돌리면 publishTarget·SpotRetire가 AssertionFailedError로 실패하고, pending-session은 **데드락**(스택 실증: getOrCreateSessionState:1379→createSessionState:1478→getOrCreateSessionState:1371, creator가 자기 pending future를 join). 정적 의심이 런타임으로 실증됨 | 감사 보고 후 판정 (스위트 재실행 T5와 합산) | **완료** — 결함 5건 전건 [실증] 수정, **기준선 대조로 회귀 0 확정**(base 1533/1510/fail 23 vs 현행 1538/1515/fail 23, 실패 이름 집합 동일 → 새 focused 테스트 5건 전부 통과). 게이트: tsc `--force` rc=0 · contract 1538(기존 실패 23) · m6a 41/41 · m6b 104/104 · m6c 113/115(기존 2). 계약 23건은 rules §4 기존 실패로 등재 |
| **Z0** cross-language e2e 그린 (샘플 작업 선행) | **완료** — 전쌍 통과 (2026-08-27) | ← | ← | ← |
| **샘플** TicTacToe | **완료** (CP2-7 스위트 그린) | 대기 — TTT JoinGameNotify 유실은 **기존 결함 판정**(worktree 이분 실증 `d0bb9d95a3`, rules §4) | java 재실행 대기(T5) | **완료** (T2 후 스위트 그린) |
| **샘플** Bingo | **완료** (CP2-7 스위트 그린) | 재실행 대기 (flake ~1/5 주의) | java 재실행 대기(T5) | **완료** (스위트 그린) |
| **샘플** SupportChat | **완료** (CP2-7 스위트 그린) | 재실행 대기 | java 재실행 대기(T5) | **완료** (스위트 그린) |
| **샘플** ShoppingMall | **완료** (CP2-7 스위트 그린) | 재실행 대기 (layout_contract 기존 실패 — rules §4) | java 재실행 대기(T5) | **완료** (스위트 그린) |
| **샘플** DeliveryDispatch | **완료** (CP2-7 스위트 그린) | 재실행 대기 | java 재실행 대기(T5) | **완료** (스위트 그린) |
| **샘플** GameQuest | **완료** (CP2-7 스위트 그린) | 재실행 대기 | java 재실행 대기(T5) | **완료** (스위트 그린) |
| **샘플** ZoneWorld (Z1~Z4) | Z2 수정 커밋 · **Z4 조용한 측정 진행 중 — 4런 연속 실패(모드 상이, T6)** | Z1·Z3 수정 커밋 · Z4 대기 | 대기 | 대기 |

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

## 3. 지표 (마지막 재측정 2026-08-27)

| 지표 | 시작 | 현재 | 판정 |
|---|---:|---:|---|
| **async 경계 스냅샷** *(진짜 지표)* | ~465 | **실질 0** | **목표 달성** — cp3-audit 전수: source 48 = 정당화 45(primitive·프로토콜) + 결함 3(수정 완료) |
| dotnet `lock` 문 | 999 | **113** (−89%) | 잔존 전부 파일별 정당화(실행 primitive·무할당 계약·socket/dispose 프로토콜 — cp3-audit 표). 초기 목표 "~60"은 추정치였고 실측 분류가 대체 |
| dotnet `_gate` 클래스 | 61 | **5** | 전부 판정된 프로토콜 gate |
| dotnet lane 파일 | 0 | **77** | — |
| cpp mutex 취득 | ~1,303 | **673** (진행 중) | **종전 "~204(−84%)"는 계수 오류였다** — 관용구가 `std::lock_guard lock (_mutex);`(변수명+공백)인데 정규식이 `<`/`(` 형태만 잡았다. 정정된 명령은 rules §6. 2026-08-27 세션 시작 시 실측 **890** → 현재 **673**(−217, 전환 진행 중). 내역: 소형 배치 A~D 101 · channel_runtime 37(T7 "미해소 C2" 해소) · public_host_runtime 62(lane 5개 분리, 99→37) · spot_runtime 진행 중. `recursive_mutex` **135 잔존**(spot_runtime 중심 — 재진입 실재 신호). 상위: spot_runtime 151 · stream_host_service 66 · raw_mesh_node_owner 49 · mesh_node_runtime 38 · public_host_runtime 37 |
| java 상위 후보 monitor | 683/94클래스 | **후보 32클래스 0** | 잔존 synchronized=native 작업 프로토콜·primitive 4종(판정) |
| node | await 교차 기준 | **후보 34 전건 판정** | 전환 필요분 완료, 발견8 불요·executor 제외군 판정 |
| 호환 경계(블로킹 브리지) | 0 | dotnet 663 · **cpp 456~460** | §5 조건 충족·사유 기록 잔존(스펙 06 개정 반영). 핫패스 회수는 이월 |

## 4. 잔여 마감 추적 (2026-08-27 — 캠페인 꼬리)

| # | 항목 | 상태 | 비고 |
|---|---|---|---|
| T1 | cpp actor_gateway 회귀 | **종결** `49053b99a9` | batch 10 잠복 2건(closure 수명·stale disconnect) 수정 + 테스트 추종 잔여 정리 |
| T2 | node TicTacToe self-deadlock | **종결** `d575cb4d0c` | 수정 후 m6a/b·stream-runtime 전부 그린, node 샘플 스위트 exit 0 |
| T3 | java 샘플 릴리스 게이트 지문 5건 | **종결** | 기존 드리프트 확정·지문 갱신 22/22, 커밋됨 |
| T4 | java FrameworkModuleBoundaryTest ReceiveFlowState import 1건 | **종결 — 기존 드리프트 판정(캠페인 무관)** | offender는 `runtime/internal/dispatch/ZLinkApplicationJobReceiveFlowController.java:13`(+`internal/backend/ZLinkBackend{Router,Dealer}Socket.java`)의 `import systems.zlink.contracts.sockets.ReceiveFlowState;`이고, 테스트의 `allowedImports` 목록에 이 심볼이 빠져 있다. 근거: ①해당 import를 도입한 커밋은 `22949bcedb`(2026-08-25, "align job backpressure with 0.13.2")이고 **브랜치 base `3cbfbde4f9`의 조상**이다 (`git merge-base --is-ancestor` 확인). ②offender 파일들도 테스트 파일도 base 대비 **diff 0**(`git diff --stat 3cbfbde4f9 -- ...` 공백). 즉 캠페인 회귀가 아니라 `22949bcedb`에서 allowlist 갱신이 누락된 기존 드리프트다. 수정(allowlist에 1줄 추가)은 **테스트 기대값 변경**이라 rules §5.1 금지 패턴에 걸리고 캠페인 범위 밖이므로 T8 이월 목록으로 넘긴다. |
| T5 | 언어별 샘플 스위트 | **보류 — CP3 마무리 뒤(순서 정정)** | rules §3 순서: 호환 경계 → posddd → **마일스톤 게이트(CP3 = 샘플 스위트 포함)**. 수정 전 실행은 무효라 중단했다. **중단 전 실측(참고용, 판정 아님)**: cpp TicTacToe 1/1 통과 · cpp Bingo 0/2(후반 'wait observer Entry Spot return') · java TicTacToe 0/3 — **단, 3회 모두 `tictactoe-placement=completed` 뒤 cleanup에서 `Aborted (core dumped)` exit 134**다. 즉 시나리오 검증(placement marker)은 통과하고 teardown에서만 죽는다 → rules §4의 "exit 86/134는 1회 재실행" 계열이며 T5가 확인하려던 java teardown 수정(`e4944785a1`) 영역이다. **정정**: 앞서 `TicTacToeClientScenario.java:100` JoinGameNotify TimeoutException을 근거로 "cpp TTT 유실과 같은 계열, 결정적 실패"라고 적었으나, 그 증상은 **중단된 1차 일괄 스위트**의 것이고 per-sample 3회와 다르다. 두 증상을 섞은 추정이었다. **미판정**으로 두고 CP3 게이트에서 재측정한다 |
| T6 | Z4 — ZoneWorld 조용한 반복 측정(dotnet·cpp 각 8~30런) 후 게이트 복귀 | **dotnet 측정 완료 — 1/8 통과. 이분 준비 완료, 판정은 다음 세션** | 조용한 환경 dotnet ×8 = **pass 1 / fail 7**, 모드 매런 상이: ZW-B8×2(경계 후 재접속 재바인딩 실패)·ZW-D1×2(WorldAnnounceNotify 미기대 도착)·ZW-B4(ZoneChangedNotify 타임아웃)·ZW-G4(WebSocket Aborted)·ZW-C2. **"부하 오염" 가설 기각.** 기준선 worktree `/home/hep7/project/zlink-z4base`(base `3cbfbde4f9`, `.artifacts` 심링크 완료 — dotnet도 심링크 필수) 준비됨. 다음 세션: 기준선에서 같은 ×8 → 모드 분포 비교로 기존 vs 회귀 판정 → 회귀면 .flow 진단. 측정 명령: 샘플 디렉터리에서 `MSBUILDDISABLENODEREUSE=1 flock -w 7200 /tmp/zlink-dotnet-gate.lock timeout 1200 bash run_samples.sh ZoneWorld` 반복 |
| T7 | 구조 보류 판정 (cpp channel_runtime_state·mesh_node_runtime, node STOP 9건, dotnet ChannelRuntimeManager 잔여 확인) | 마일스톤 기록 완료 | 필요 시 후속 캠페인 |
| T8 | 기존 결함 이월 (cpp layout_contract 샘플 blocking result, node m6c 계약 2건, **java FrameworkModuleBoundaryTest allowlist 누락 — T4에서 기존 드리프트 확정**) | 이월 목록 | rules §4 |
| T9 | 최종 문서·메모리 정리 + main 병합 판단(사용자) | 대기 | |
| T11 | **cpp 전환 재개** (2026-08-27 세션) | 진행 중 — **커밋 217취득**(890→673) | 소형 배치 A·B·C·D 101 + `channel_runtime` 37(T7 "미해소 C2" 해소) + `public_host_runtime` 62(**lane 5개 분리**, 99→37) + `spot_runtime` route_client 3. **별도 worktree 이분으로 배치분 무결 증명**(base→HEAD 6단계 × 각 3런 = 18런 전부 통과). **패스 단위 교훈**: "부분 전환도 성과다"로 지시하면 에이전트가 취득 1~3개만 옮기고 미룬다(3패스에 4취득). "C2 그룹 하나 통째로"로 바꾸니 3패스에 58취득 — **14배**. C2를 부분 전환하면 lane과 mutex가 같은 불변식을 나눠 지켜 전환 전보다 나쁘다. 근거: dotnet #9 `ZLinkManagedMeshNode`(130취득)가 한 클래스 안 lane 5개 분리로 완료(`b3497532a6`). design §3 "경계를 새로 긋지 않는다"는 **클래스 분할 금지이지 다중 lane 금지가 아니다** |
| T14 | **`spot_runtime` 전환 — 2회 STOP, 사유 상이** | 합본 패스 진행 중 | ①1차: C1 `route_client`만 전환하고 C2 144를 "다음 패스"로 미룸 → 패스 단위 문제(T11에서 해소). ②2차(g1, Actor registry·실행): **정당한 STOP** — "`Actor registry·실행`을 하나의 lane으로만 옮기면 **관측 가능한 Spot close/idle-eviction과 Actor join/destroy의 원자 경계가 갈라진다**. 이 경계는 `Spot context·생성` 그룹도 같은 결정을 하므로 대상 그룹만의 lock 제거는 C2를 lane과 mutex로 나누어 보호하게 된다"(스펙 06 §8). 부분 전환은 검증 전 전부 되돌려 source 변경 0. **→ 원래 4그룹 분석이 틀렸다. Actor registry·실행 + Spot context·생성은 하나의 C2다.** 합본 lane으로 재편성해 재발행. 난점: `recursive_mutex` 132곳 = **재진입 실재**, lane은 재진입 시 예외를 던지므로 절차 2(전수 제거)가 선행돼야 하고 그 자체가 큰 작업이다 |
| T15 | **`public_host_runtime` 전환 완결 — 99 → 0** | **완료** | 주 `_mutex` **선언 자체가 제거**되고 lane 6개 구조가 됐다: `_route_cache_lane` · `_peer_endpoint_lane` · `_spot_actor_index_lane`(g1) · `_local_dispatch_completion_lane`(g2) · `_lifecycle_configuration_lane`(g3) · `_relocation_session_terminal_lane`(g4). 컬렉션은 평범한 `std::map`/`std::set`. dotnet `ZLinkSpotNodeCatalog`(48→0, lane 10곳, 평범한 Dictionary 6개)와 동일한 결과다. **g4는 중앙 실측으로 찾아낸 누락 그룹**이다 — g1~g3 종료 후 잔여 36의 소유 멤버를 직접 세니 `_relocation_target_attempts` 28 · `_session_seal_terminals` 15 · `_user_spot_terminals` 12 · `_relocation_assemblies` 9 · `_routes` 7로, 원래 3그룹 분석이 relocation·session terminal 계열을 통째로 빠뜨렸다. g3 보고서의 잔여 그룹 이름 나열도 부정확했다. **교훈: 에이전트 보고의 그룹 이름을 믿지 말고 중앙에서 멤버를 직접 세라.** 패스별: 부분전환 3패스 99→95(4) · g1 →75(20) · g2 →67(8) · g3 →37(30) · g4 →**0**(37) |
| T12 | **대형 전환 미커밋 변경이 게이트 파손** | 격리 중 | 본체 트리에 대형 3건 변경이 있는 상태에서 ctest 45개 중 **5개 실패, `test_cpp_framework_execution` SEGFAULT**(+`m6b_runtime`·`m6c_runtime`·`actor_gateway`). 커밋된 배치는 이분으로 무죄 확정이므로 원인은 대형 전환분. 4패치 저장 후 트리 되돌리고 ① 커밋상태 → ② +outbound → ③ +channel_runtime → ④ +public_host → ⑤ +spot_runtime 누적 게이트로 격리 중 |
| T13 | **측정 규율 실패 3회** (기록) | 교훈 | ①이분 스크립트가 빌드 에러를 `/dev/null`로 버리고 전체 빌드 rc로 판정 → 배치와 무관한 protobuf 코드젠 누락(e2e 타겟)에 속아 4단계 전부 BUILD-FAIL, **30분간 "진행 중"으로 오보**. 처방: 필요한 타겟만 빌드하고 `Built target <name>` 확인, 에러 원문 보존, `PIPESTATUS`. ②에이전트가 같은 트리를 빌드하는 중에 중앙 게이트 실행 — 트리 락은 빌드끼리만 직렬화하고 빌드-테스트 사이는 보호하지 않는다. ③잡 실행 중 `git add -A`로 진행 중 작업이 리뷰 없이 커밋에 유입(`spot_runtime` 재진입 제거). 처방: 잡 실행 중에는 경로 명시 커밋 |
| T10 | cpp·jvm·node CP3 감사 → 언어별 CP3 판정 | **node·jvm 종결 · cpp 전환 잔여** | 감사 3건 전부 NOT CLEAN, 결함 **13건 전건 [실증]**(node 5·cpp 2·jvm 6) → 전건 수정. **node**: 기준선 대조로 회귀 0 확정(base 1533/1510/23 vs 현행 1538/1515/23, 실패 이름 집합 동일). 계약 23건은 기존 실패로 rules §4 등재. **jvm**: 게이트 4모듈 실패 0 + 되돌림 검증 통과(`c7aab4fb57`). connector·redis 두 모듈을 게이트 매트릭스에 신규 등재. **cpp**: 결함 2건 수정, 발견 10 재감사 0건, 상태 보호 138취득 전환. 대형 2건은 C2 그룹별 7패스로 진행 중 |
