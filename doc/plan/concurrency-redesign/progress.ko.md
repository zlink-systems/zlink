# 진행표 — lane 소유 동시성 전환

> **이 문서는 진행표 하나만 담는다. 상태 갱신은 아래 표에서만 한다.**
> 규칙·프로토콜·요청 템플릿·발견 목록은 [rules.ko.md](rules.ko.md),
> 배경·이어받기는 [handoff.ko.md](handoff.ko.md), 순서 근거는
> [conversion-order-survey.ko.md](conversion-order-survey.ko.md)와 l1/l2-survey-*.ko.md.

브랜치 `refactor/lane-ownership-concurrency` · 시작 2026-08-26

상태 값: `대기` → `요청됨`(잡 실행 중) → `보고됨`(CP1 대기) → `완료`(리뷰 승인·커밋) / `반려`(재요청) / `제외`(사유 명시)
갱신 시점: 잡 기동=요청됨, 보고 수신=보고됨, CP1 승인·커밋=완료(+해시), CP2 게이트=지표 행 재측정. 해당 작업 커밋과 같은 커밋에서 갱신한다.

| 언어 | 단계 | 항목 | 상태 | 커밋/게이트 | 비고 |
|---|---|---|---|---|---|
| 공통 | 설계 | 스펙 06 + `ZLinkStateLane` + golden 14 | **완료** | `0fe461fc6c` | 테스트가 곧 명세 |
| 공통 | 지표 | dotnet lock 999→**783**, `_gate` 61→**55**, lane 파일 **7**, AwaitStateLane **58**(부채), async 경계 스냅샷 ~465→미측정 | CP2마다 재측정 | 마지막 2026-08-26 | 진짜 지표는 async 경계 스냅샷=0. 재측정 명령은 rules §6 |
| dotnet | L0 | primitive + golden 14 | **완료** | — | |
| dotnet | L1 표본 | ZLinkSessionActorBindingTable (30) | **완료** | `3cc6f5f615` | sol. 재진입 2, 호환 경계 25 |
| dotnet | L2 #1 | ZLinkInMemoryLocationStore (27) | **완료** | `2bdf463ee3` | 재진입 0, posddd ② 생략(근거 커밋에) |
| dotnet | L2 #2 | ZLinkActorHandoffAdmissions (25) | **완료** | `326133ca03` | SuppressFlow 1, 호환 경계 4 |
| dotnet | L2 #3 | ZLinkActorOwnershipCoordinator (26) | **완료** | `e1f5ade349` | 재진입 8 해소, 호환 경계 3, Task.Yield 보강(발견4) |
| dotnet | L2 #4 | ZLinkSpotNodeCatalog (48) | **완료** | `0fef22fa62` | SuppressFlow 5, 호환 경계 9, dispose fix `1bd23dc1ff` |
| dotnet | L2 #5·6 | ZLinkClientServerClientRuntime + Connection (20+44) | **완료** | `39d71acd6c` | lane 2, SuppressFlow 7, 호환 경계 4, _socketLifecycleGate 유지 |
| dotnet | L2 CP2-1 | 배치 1 게이트 (#3~#6) | **통과** | unit 1893/0 · 7샘플 exit 0 | 2026-08-26 |
| dotnet | L2 #7 | ZLinkActorHandoffState (58) | **완료** | `b49b1ef55d` | 재진입 3(유형③ 신규 — 발견6), 호환 경계 63, 반려 2회. unit 1893/0 |
| dotnet | L2 #8 | ZLinkActorRuntimeState (35) | **요청됨** | | codex terra (2026-08-26). semaphore=작업 직렬화로 유지 판정(발견7 예정), 재진입 ~20 |
| dotnet | L2 #9 | ZLinkManagedMeshNode (130) | 대기 | | 4×+, 정독 리뷰 필수 |
| dotnet | L2 #10 | ZLinkFrameworkRuntime (27) | 대기(최종) | | 사용 45파일, 재진입 ~57, 정독 리뷰 필수 |
| dotnet | L2 소형 | 잔여 ~50클래스 (lock<20, ~300) | 대기 | | 3~5개 배치, C1/C3 다수 예상 |
| dotnet | 제외 | ZLinkSerialExecutionQueue (25) | 제외 | | lane primitive 자체 (스펙 06 §9) |
| dotnet | 마무리 | 호환 경계 회수 (AwaitStateLane 121+) | 대기 | | rules §3 |
| dotnet | 마무리 | posddd ② 잔여 (최우선: OperationLease 할당) | 대기 | | |
| dotnet | 마무리 | 마일스톤 게이트 (CP3 + §8 결함 판정 + 스펙 06 개정) | 대기 | | |
| cpp | L0 | primitive + golden 14/14 | **완료** | — | close·try_post 가드는 정본보다 엄격 |
| cpp | L1 표본 | stream_session_registry_t (31→0) | **완료** | `f4d0df006b` | 시그니처 무변경, admit_inbound 대기 분리+세대화, 반려 1(lost wakeup). unit·contract 그린(layout_contract 제외—기존) |
| cpp | L2 | 확산 (후보 19 — l2-survey-cpp.ko.md) | 대기 | | 진입: dotnet 마일스톤 후 |
| cpp | 마무리 | 호환 경계(.get 사유 기록)·posddd ②·마일스톤 | 대기 | | |
| java | L0 | primitive + golden 14/14 + 보완(발견5 fix·internal 이동) | **완료** | — | completeAsync, runtime.internal.execution |
| java | L1 표본 | ZLinkSessionActorsRuntime (22→0) | **완료** | 커밋됨 | 인터페이스 불변·kotlin 파급 0. core 1,157 중 알려진 flake 1 |
| java | L2 | 확산 (후보 23+9 — l2-survey-java.ko.md) | 대기 | | 진입: dotnet 마일스톤 후 |
| java | 마무리 | 호환 경계(inStateLane join)·posddd ②·마일스톤 | 대기 | | |
| kotlin | L0 | primitive + golden 14/14 | **완료** | — | CoroutineContext.Element 전파 |
| kotlin | L1·L2 | 해당 없음 — 자체 C2 상태 0 (l1-survey-kotlin.ko.md) | **판정 완료** | | java 전환을 인터페이스 뒤에서 승계. kotlin 67/0 |
| node | L0 | primitive + golden 12 (동시성 2 제외, 사유 주석) | **완료** | — | |
| node | L1 표본 | ZLinkActorSessionBindingRegistry | **완료** | `ca32669168` | 재진입 19 분리, runtime 13파일 async 전파, d.ts 불변. m6a/b·stream-runtime 그린(m6c 2건 기존) |
| node | L2 | 확산 (순서 조사 완료 — l2-survey-node.ko.md) | 대기 | | 진입: dotnet 마일스톤 후 |
| node | 마무리 | posddd ②·마일스톤 (호환 경계 해당 없음) | 대기 | | |
| 공통 | Z0 | cross-language e2e 그린 (샘플 작업 선행) | 대기 | | 전 언어 락 단독 점유. rules §3.1 |
| 공통 | Z1 | cpp ZoneWorld 세션 split-brain 수정 | 대기 | | 재현: 7샘플 일괄 |
| 공통 | Z2 | dotnet ZoneWorld mesh admission Hello 반복 수정 | 대기 | | 직접 3회 0/3 |
| 공통 | Z3 | fast_mutex.hpp:76 abort 수정 | 대기 | | Z1 계열 추정 |
| 공통 | Z4 | ZoneWorld 7샘플 게이트 복귀 (30런급 확인) | 대기 | | 샘플 1개 완료마다 감독관 리뷰 (rules §3.1) |
