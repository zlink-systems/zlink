# Relocation 캠페인 통합 체크리스트

작성 2026-08-19. 이 문서가 진행 중인 모든 작업의 단일 추적처다. 항목 완료 시 체크하고
커밋 해시를 병기한다. 새 사용자 지시는 이 문서에 먼저 반영한 뒤 착수한다.

## 진행 규칙

- 그린 마일스톤마다 즉시 커밋·push. 커밋 스테이징은 **파일 명시 나열**(공유 worktree
  오염 방지 — 광역 `git add` 금지).
- 커밋 묶음마다 Codex sol 리뷰. 발견 결함은 이 문서에 항목으로 추가 후 해소.
- 스펙 변경은 오류·개선만. 상호 운용(cpp·java·dotnet·node 동일 프로토콜)은 프로젝트
  근간 — 충돌하는 스펙 조항이 오류다.
- 완료 조건: 이 문서의 A·B·C 전 항목 체크 + 모든 테스트 그린 + zoneworld 제외
  6샘플×언어 성공 + sol 전 문서 spec-gap 리뷰 통과.

## A. base/delta capture 제거 (사용자 결정 2026-08-19)

- [x] 정식 문서 제거 (spec 15·01·06, 언어 interface, 내부 51·52, guide 재생성) — `8b12fbf62f`
- [x] node 구현 제거 — `b05791682b`
- [x] java 구현 제거 — `14de957df8`
- [ ] cpp 구현 제거 (에이전트 진행 중)
- [ ] dotnet 구현 제거 (에이전트 진행 중)
- [ ] **wire 원자 커밋**: schema에서 40 `baseChecksumCrc32c`·52 `payloadStage` 필드
      제거 + golden 재계산 + validator self-test + 4언어 codec 동시 정리
      (`TODO(schema-atomic)` 마커 추적) — 4언어 게이트를 모두 돌린 뒤 한 커밋
- [ ] draft 파일 재삭제 (`spec/draft/relocation-interruption-improvements.ko.md` —
      사용자 지시: 제거 완료 후)

## B. 원 캠페인(M9) 잔여 검증 게이트

- [ ] cpp e2e ST-C4: 작성된 identity-conflict variant를 안정화된 트리에서 실행·그린
      확인 후 커밋 (checksum variant는 test seam 부재로 정직한 부분 — C-4와 연계)
- [ ] cpp e2e Track F 신설: SF-F2·F3·F7·F11을 DiscoveryRegistryHa에 구현
      (relocation-capable 노드 역할 추가, SpotActorTransfer fail-injection 패턴 이식;
      SF-F3는 별도 relocation store 컨테이너 다운, SF-F7은 chunk/budget 옵션 축소)
- [ ] Bingo 재검증: node·kotlin에서 base/delta 제거 랜딩 후 재실행 — relocation 인접
      동일 지문(observe timeout / READY stage unavailable) 재현 시 terra 심층 조사
- [ ] dotnet 샘플 6종 실행 (Bingo, DeliveryDispatch, GameQuest, ShoppingMall,
      SupportChat, TicTacToe — 종료 코드 로그)
- [ ] java/kotlin 샘플 집계 게이트: SampleReleaseGateContractTest,
      CurrentManagerFakeBackendTest, FORBIDDEN_SAMPLE_PATTERN rg 스윕 (run_samples.sh 상단부)
- [ ] RelocationBehaviorConformanceTests(dotnet) batch hang 근본 원인·처분
- [ ] dotnet TicTacToe JoinGameNotify timeout(exit 134) 조용한 머신에서 재현 조사
      (샘플 게이트 1차는 PASS였음 — 재현 안 되면 종결 기록)
- [ ] harness 기본 `all` 스테이지 깨끗한 단독 재실행 (동시 에이전트 경합으로 1차
      판정불가; message-follow "Raw MeshNode requires the host Application Job Queue"
      사전 실패 주장 포함 확인)
- [ ] cpp bind-session 재시도 소진 분류(deadline_exceeded로 갱신됨, `46ef4b0f03`)의
      교차 언어 parity 확인 — java/node/dotnet의 동일 시나리오 분류 대조
- [ ] **sol 전 문서 spec-gap 리뷰**: 스펙·guide·e2e·언어 interface 전체 vs 구현 대조,
      gap 0 확인 (완료 조건)
- [ ] 최종 게이트 일괄: 4언어 unittest + 6샘플×언어 + doc 게이트 + 최종 보고

## C. 상호 운용 확장 (M10, 사용자 확정 2026-08-19 — 근간·양보 불가)

- [ ] C-1 store 규범 계약 설계 비준: 레코드별 canonical 키공간·redis 자료형·값 인코딩,
      버전 판별자, 마이그레이션 방식 (설계 제안 에이전트 진행 중 → 코디네이터 비준)
- [ ] C-2 스펙 개정: 21-location-runtime·23-relocation-store-redis의 "provider 내부"
      조항을 규범 계약으로 개정 (28:589-591 수용 기준은 수정 없이 유지)
- [ ] C-3 store 레코드 golden fixture 신설 (키 문자열·값 byte 벡터) + 4언어 소비 테스트
- [ ] C-4 4언어 store 구현 수렴 (기준 후보: dotnet/java 공유 {zlink-location-v3}+SHA256;
      cpp = 키공간+인코딩 전면 이동, node = 키공간 이동, dotnet↔java = 값 인코딩 통일)
- [ ] C-5 cpp actorJoin 발신 연산 신설 (service-wire cross-node join 요청/응답,
      binary tail 탑재 — codec은 `7ed3992ccd`로 기구현, node 발신 경로 참조)
- [ ] C-6 dotnet actorJoin 발신 연산 신설 (BeginJoin의 local-or-NotConnected 한계 해소,
      codec은 `327b58a3d2`로 기구현)
- [ ] C-7 harness 교차 언어 relocation stage 전 쌍 그린 (JoinEntrySpot 경로 우선,
      C-5·C-6 후 일반 join 경로 — 기존 opt-in 스테이지 `2907df293f`/`c43758fc05` 기반)
- [ ] C-8 교차 언어 스테이지를 harness 기본 `all` 게이트에 편입 (회귀 구조 차단)
- [ ] C-9 상호 운용 신규 코드 sol 리뷰 + POSDDD 패스

## D. 이월·후속 (완료 조건 아님, 기록)

- [ ] dotnet 성능 후속(문서화됨, `062c0bbd1e` 메시지): message-follow 중복 lock 수동
      해체, ToHex 정렬 comparer, HeldRecords O(N²), 중복 participant 스캔, codec
      삼중 복사, LINQ 재해석 캐시
- [ ] cpp complete_relocation_assembly 구조 분해 (등가성 테스트 동반 전용 패스)
- [ ] cpp aggregate 오케스트레이션 reason 전파 (task_t<bool> 고정 인터페이스)
- [ ] cpp StageInboundSpotAggregateAsync급 대형 메서드(dotnet) participant-restore 추출
- [ ] node relocationFailed row-collapse 세분화 (wire 후보 15/33/34 선택) — 전용 wire
      어휘 확장(ShuttingDown 등)과 함께, 필요 실증 시
- [ ] cpp standalone actor 직접 relocation 복원 갭 parity 판정 (materialize의
      target_spot 강제 — 타 언어 동작과 대조)
- [ ] m6b M-c mismatched-identity rejection 테스트 (aggregate identity-fencing 조사)
- [ ] e2e_inventory 기존 backlog 168건 (캠페인 외 문서 부채 — 별도 계획)
