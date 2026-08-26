# 진행 상황 — lane 소유 동시성 전환

> 이 캠페인의 살아 있는 추적 문서다. 항목이 끝날 때마다 갱신한다.
> 규칙은 [스펙 06](../../../framework/doc/framework/common/spec/server/01-execution/06-state-ownership-and-lanes.ko.md),
> 순서 근거는 [conversion-order-survey.ko.md](conversion-order-survey.ko.md),
> 배경·이어받기는 [handoff.ko.md](handoff.ko.md).

브랜치 `refactor/lane-ownership-concurrency` · 시작 2026-08-26

## 1. 진행 방식 (확정)

- **역할**: 전환 = codex(표본급 난이도만 sol, 이후 terra) · 감독/리뷰/판정/커밋 = Claude.
- **2패스 원칙**: ①동작 보존 lane 전환(본문 무변경) → 커밋 → ②posddd 리팩토링
  (할당·복사·경합·죽은 코드, `doc/principal/dev/posddd.ko.md`) → 별도 커밋.
  ②는 측정 우선순위가 낮으면 근거를 남기고 생략할 수 있다.
- **가속 모드**(2026-08-26 사용자 승인): 서로 다른 파일을 만지는 클래스는 codex 잡을
  병렬로 돌린다. 같은 트리의 빌드·테스트는 `flock /tmp/zlink-dotnet-gate.lock`으로
  직렬화한다. 전체 unit 게이트와 6샘플 게이트는 배치(2~4클래스)마다 중앙(Claude) 1회.
  기계 변환 diff는 금지 패턴 grep+표본 검토, 판단이 들어간 변환과 대형 2건은 전문 정독.
- **게이트**: 스펙 06 §10 — 단위 테스트 실패 0, 6샘플 placement marker 유지
  (ZoneWorld는 기존 결함으로 제외), 관측 동작 불변.
- **푸시**: 2~3클래스 누적마다.

## 2. .NET 전환 현황

| # | 클래스 | lock | 상태 | 커밋 | 비고 |
|---|---|---:|---|---|---|
| 표본 | ZLinkSessionActorBindingTable | 30 | **완료** | `3cc6f5f615` | sol. 재진입 2, 호환 경계 25(부채) |
| 1 | ZLinkInMemoryLocationStore | 27 | **완료** | `2bdf463ee3` | 재진입 0, posddd ② 생략(근거 커밋에) |
| 2 | ZLinkActorHandoffAdmissions | 25 | **완료** | `326133ca03` | SuppressFlow 1, 호환 경계 4 |
| 3 | ZLinkActorOwnershipCoordinator | 26 | 진행 중(병렬) | | 재진입 의심 8 |
| 4 | ZLinkSpotNodeCatalog | 48 | 진행 중(병렬) | | detached 작업 3 |
| 5·6 | ZLinkClientServerClientRuntime + Connection | 20+44 | 진행 중(병렬) | | 장기 작업 5, lane 2개 |
| 7 | ZLinkActorHandoffState | 58 | 대기 | | 2× 난이도 |
| 8 | ZLinkActorRuntimeState | 35 | 대기 | | semaphore + 재진입 ~20 |
| 9 | ZLinkManagedMeshNode | 130 | 대기 | | 4×+, 정독 리뷰 필수 |
| 10 | ZLinkFrameworkRuntime | 27 | 대기(최종) | | 사용 45파일, 재진입 ~57, 정독 리뷰 필수 |
| — | 소형 잔여 (~50클래스, lock<20) | ~300 | 대기 | | 3~5개 배치, C1/C3 다수 예상 |
| 제외 | ZLinkSerialExecutionQueue | 25 | 제외 | | lane primitive 자체 (스펙 06 §9) |

## 3. .NET 마무리 항목 (클래스 전환 후)

- [ ] **호환 경계 제거** — `AwaitStateLane` 잔여(표본 25 + 이후 발생분)를 async 전파로
  해소. 남는 게 있으면 사유 기록.
- [ ] **posddd ②패스 잔여** — survey의 관찰 목록 기준. 최우선: `ZLinkFrameworkRuntime`
  hot path의 `ZLinkRuntimeOperationLease` class 할당.
- [ ] **마일스톤 게이트** — 전체 unit + 6샘플 + cross-language harness 재확인,
  "async 경계 스냅샷" 계열 재측정, **§8 기존 결함 3건 재확인**(cpp ZoneWorld
  split-brain / dotnet ZoneWorld mesh admission / fast_mutex abort — 이 작업의 하위
  증상인지 판정), **codex sol 마일스톤 리뷰**, 스펙 06 보완 필요 여부 판정.

## 4. 언어 병렬 단계 (합의된 시점)

1. **primitive 포팅** — cpp·jvm(java/kotlin)·node의 state lane primitive + 스펙 06 §10
   검증 테스트(golden = .NET `StateLaneTests`). dotnet과 무의존이라 조기 병렬 가능.
   (사용자 승인 대기 중)
2. **언어별 표본 1개** — dotnet 중형 구간 완료 후. 각 언어의 BindingTable 대응부.
3. **전면 확산** — dotnet 마일스톤 게이트 통과 후. cpp 주의: codex 안전 필터가 메모리
   수명 주제를 거부하면 Claude 서브에이전트로 우회.

## 5. 기록 규칙

- 클래스 하나 끝날 때마다 §2 표의 행(상태·커밋·비고)을 갱신한다.
- 규칙이 바뀌는 발견(새 재진입 유형, 호환 경계 판단 등)은 §6에 적고, 스펙 06 개정이
  필요하면 마일스톤 게이트에서 일괄 처리한다.

## 6. 전환 중 발견 (스펙 후보)

- (표본) lane 안에서 시작한 timeout 작업의 AsyncLocal 소유권 상속 → SuppressFlow 처방.
  스펙 06 §6 유형 ②로 반영 완료.
- (2번) `_operationGate` 안에서 lane 블로킹 대기 시 데드락 판정 기준: lane 큐 항목이
  그 gate를 재획득하지 않고, 완료 신호 TCS가 전부 `RunContinuationsAsynchronously`면
  안전. 호환 경계 리뷰 체크리스트로 사용.
