# Relocation 캠페인 — 남은 작업 세션별 로드맵

작성 2026-08-21. 이 문서는 기존 통합 체크리스트(1178줄, 이력·커밋해시 포함 —
`doc/plan/archive/relocation-campaign-checklist.ko.md`로 아카이브)의 **미결 23항목만**
뽑아, **새 세션에서 독립적으로 집어 진행할 수 있는 단위**로 재구성한 것이다. 이 문서가
앞으로의 활성 트래커다. 각 세션 카드는 목표·선행조건·구체 단계·완료조건(DoD)·근거
포인터·검증 명령을 담는다. (아래 "체크리스트 line/항목 …" 참조는 아카이브된 그 문서를 가리킨다.)

## 0. 현재 진실 (착수 전 반드시 읽기)

- **제품은 동작한다(JSON/legacy 경로).** 4언어 유닛·6샘플·harness가 green인 것은 사설
  actor-join 전송(JSON dialect)으로 동작하기 때문이다. canonical wire는 아직 **게이트가 닫혀**
  생산 경로가 아니다(체크리스트 C-5/C-6/line185 참조).
- **Track A(canonical 마이그레이션)는 그 legacy를 spec 51 §9 통합 wire로 "교체"하는 작업**이다.
  코드 골격은 있으나 활성화(수신자 wire 연결·발신 게이트 개방·dialect 제거)가 미완이다.
  체크리스트가 직접 H-12/H-15를 "미구현·4언어 공통 대형 트랙"이라 부른다.
- 따라서 **Track B·C·D는 A와 독립으로 진행 가능**(현 JSON 생산 경로 위에서 검증). 단 캠페인
  최종 "완료 조건"(체크리스트 line 24-25)은 A 포함이다.

## 진행 규칙 (요약; 상세는 memory + 체크리스트 §진행규칙)

- 작업 agent = `codex exec -m gpt-5.6-terra -c model_reasoning_effort="high" -s danger-full-access --skip-git-repo-check`,
  리뷰 = `codex exec -m gpt-5.6-sol ...`, 리서치 = codex.
- **Claude가 모든 agent 결과를 독립 검증**(diff 정독 + stash-baseline 재현 + 독립 테스트) **후
  sol 리뷰**, 두 결과 대조 후 판정·커밋. 맹신 금지.
- agent는 `framework/doc/**` 수정 금지(스펙/가이드/인터페이스=Claude 단독). 커밋은 Claude가
  파일 명시 `git add`(광역 `-A` 금지). agent `git reset/checkout/stash` 금지.
- 스펙 변경은 오류·개선만(구현 편의 완화 금지). 판정·전파(4언어)는 Claude 단독.
- u64 규칙: 전범위 opaque equality 토큰(lifecycle/node generation, correlation, replyRouteId,
  OperationId, handoff/transfer/completion id)은 `== 0`/`!= 0`만. bounded counter(Object/
  AuthorityOwner/OwnerLease/DescriptorRevision, 1..2^63-1)만 `<= 0` 허용.
- 디버깅은 기존 message-flow 트레이싱(spec 26)부터. 임시 콘솔 로그 추가-후-재실행 금지.

---

# Track A — Canonical actor-join 마이그레이션 (최대·최심)

목표: 4언어 사설 actor-join dialect를 spec 51 §9 canonical wire(command 28 + Store-backed
수신자)로 통일하고 사설 packet을 제거한다. 근거 스펙: 51 §9(Receiver Stable-Type Resolution,
Admission Lifecycle And Abandonment Cleanup), 15 §4.2, 28 §4.2.

## A1. .NET canonical 28 수신자 wire-ingress 연결  ⟶ (진행 중)

- **문제**: `ZLinkManagedMeshNode.ProcessReceivedAsync`의 수신 whitelist가 ActorJoin(28)을
  제외 → wire로 온 28이 protocol error로 drop. 수신 경로 `PrepareCanonicalActorJoinAsync`
  (`ZLinkFrameworkRuntimeActors.cs:2678`)는 `ZLinkSpotActorJoinDispatcher`에서만 호출돼 raw
  수신 미연결. ("S3 receivers 4/4"가 .NET에선 틀렸음.)
- **단계**: ProcessReceivedAsync가 canonical 28을 decode(`TryDecodeActorJoinRequest`)→admission
  (§9 Store fence 해석→provisional secure→spot admission)→reply tail(receiveChunkLimitBytes)로
  라우팅. entry bool8 존중. 실패는 typed terminal(Unavailable/NotFound/ProtocolError/
  TypeMismatch/Rejected), silent drop 금지. node 형상 미러(`service-stateful-runtime.ts`
  decodeActorJoin28, `remote-actor-join-receiver.ts` prepareCanonicalActorJoin).
- **DoD**: wire로 온 28이 실제로 admit되는 신규 in-process/loopback 테스트 + mismatched-fence가
  typed terminal. 전체 dotnet 게이트 무회귀(sanctioned 3건만). sol CLEAN.
- **선행**: 없음(clean subset 커밋 `ab26a6527b` 기반).

## A2. C++ canonical 28 수신자 완성 (H-12) + chunk limit (H-14)

- **문제**: cpp canonical 경로가 (a) Store Authority row에서 stableType 해석 미연결
  (`actor_type_from_authority`를 `admit_wire_actor_join`에 연결, sync local-map→async Store),
  (b) Accepted 후 seal/capture/Restore/relay/cutover continuation 미연결, (c) public Accepted가
  target owner CAS+queue-open 후에만. chunk limit(H-14)은 `mesh_node_runtime.cpp:2562`에서 수신
  경로에 미적용(origination revert `ab0b4b39a4`로 dormant).
- **DoD**: cpp가 canonical 28 수신·admission·chunk limit 적용, focused + 관련 e2e 무회귀, sol CLEAN.
- **선행**: A1(스펙 §9 해석 형상 참조), spec 결정 `8d8e5cdffd`(이미 확정).
- **주의**: 체크리스트 slice2(line 126) "canonical-accepted를 `seal_remote_application_actor_join()`
  경유"는 §9 "admission-time seal/commit 금지"와 충돌 — **spec §9에 맞춰 재작성 후 착수**.

## A3. .NET canonical 28 발신 (재작업 — sol 6건 반영)

- **배경**: 1차 발신 시도는 sol NOT-CLEAN 6건으로 clean subset(decouple+바인딩 제거)만 landing,
  origination(발신+gate+수신 dispatch+completion) revert. 6건:
  (1) 수신 dispatch 미연결 = A1이 해소, (2) 28-accept 시 membership commit(=node와 동일 형상,
  moving 게이트로 dispatch 차단 확인 → ruling 위반 아님), (3) empty recovery로 public Join
  completion 유실, (4) entry 하드코딩 false, (5) bound-session을 admission 제외로 처리(ruling은
  seal/route leg만), (6) production invariant 미테스트.
- **DoD**: A1 위에서 발신 재구현, (3)(4)(5)(6) 해소, 전체 게이트 무회귀, sol CLEAN.
- **선행**: A1 완료(수신자 없으면 .NET↔.NET 왕복 검증 불가).
- **근거**: `doc/plan/canon-s4c-s4d-sender-integration-plan.ko.md`(발신 통합 상세·검증 이력).

## A4. C++ canonical 28 발신 (S4c-cpp)

- **문제**: cpp canonical send 활성화(`mesh_node_runtime.cpp:2317` gate)가 Bingo·TicTacToe
  샘플 회귀(baseline 통과, S4c 실패 — cpp completion 경로 손상). S4d(relocation/completion
  state 통합) 선행 필요.
- **DoD**: cpp 발신 게이트 개방, Bingo·TicTacToe stash-baseline 대조 무회귀, sol CLEAN.
- **선행**: A2(cpp 수신자), A5(seam).

## A5. Attempt-lifecycle / multi-attempt 통합 (S4d attempt-binding, S4d-b bound Session)

- **배경**: 28→40 identity 바인딩은 **불필요**로 판정됨(branch A: canonical admission은 40을
  기다리지 않고, 40 reservation은 relocation identity로 별도 트랙). 단 multi-attempt
  later-attempt-wins 재park 정확성은 node/java에서도 미검증(canon-node-java-multiattempt-repark).
- **단계**: (a) node/java multi-attempt 재park 테스트로 branch A/B 확정(코드상 바인딩 없음
  확인됨), (b) 필요 시 target-local supersession 규칙 spec 명문화, (c) bound Session은 28 전
  42/43 seal + target commit 후 44 route update(28 sideband 금지).
- **DoD**: 재park 정확성 테스트 존재, bound Session 분리 동작, sol CLEAN.
- **선행**: A1/A3(수신·발신 활성화 후라야 실경로 검증).

## A6. 크로스랭 harness canonical 매트릭스 (S4e)

- **단계**: User Spot JoinSpot stage를 canonical 28 pairwise 매트릭스(Node↔.NET↔Java↔C++)로,
  JoinEntrySpot 의미 보존. harness 기본 `all`에 편입(무음 편입 금지, flake는 명시 스킵).
- **DoD**: 4언어 pairwise canonical 28 결정적 그린, 매트릭스 편입.
- **선행**: A1~A4(4언어 수신·발신 활성화).

## A7. 사설 dialect 제거 (H-15 / S5)

- **단계**: 4언어(cpp typed-JSON·.NET envelope·java newline·node JSON) 상위 경로를 canonical
  28+Store-backed 수신자로 교체 완료 후, 사설 actor-join packet 삭제.
- **DoD**: 사설 dialect 코드 제거, 전 게이트+샘플+harness 그린, sol CLEAN.
- **선행**: A1~A6 완료.

---

# Track B — Config / F e2e authoring (A와 독립)

## B1. H-7 config-6 e2e authoring (14 시나리오)

- **범위**: config6 SF-B3/C3/C4/C5/C5A/F1/F4~F10/G1/G2 등 14건 — 클라이언트 시나리오 코드
  **미구현**(문서 헤딩만 존재). correctness 아닌 coverage authoring.
- **근거/계획**: `doc/plan/config-6-10-authoring-plan.ko.md`(스코핑 완료).
- **DoD**: 14 시나리오 authoring + dispatch 연결 + green.

## B2. H-8 config-10 Track E/G/H/I authoring (~28 시나리오)

- **범위**: config10 Track E/G/H/I 미구현(실제 22 시나리오 dispatch는 orphan 없음 확인).
  ST-F3A orphan은 H-1로.
- **근거/계획**: 동상(config-6-10-authoring-plan.ko.md).
- **DoD**: Track E/G/H/I 시나리오 authoring + green.

## B3. H-11 F: e2e_inventory backlog

- **근거/계획**: `doc/plan/f-e2e-inventory-plan.ko.md`(스코핑 완료).
- **DoD**: inventory backlog 항목 authoring + green.

---

# Track C — W-3 생성 코덱 전면 스왑 (A와 독립, 대형)

## C1. H-6 W-3 4언어 손 코덱 → 생성 코덱 스왑

- **범위**: 4언어 surface별 hand 코덱을 생성 코덱으로 교체, surface별 byte-동치 게이트.
  파일럿 생성기 존재(`framework/runtime/protocol/generate-service-wire-pilot-codecs.mjs`,
  `--check`). C-7 최종 수용(W-4)이 이 위에서 매트릭스 그린으로 판정.
- **DoD**: 4언어 생성 코덱 채택, byte-동치 게이트 그린, 교차 매트릭스 그린.

---

# Track D — 최종 게이트 (A/B/C 결과 수용)

## D1. H-1 cpp e2e ST-C4 fault-injection variant

- **범위**: config-10이 계약한 "동일 relocation identity의 checksum/길이 불일치" assembly
  충돌을 계약 fault point 경유로 재작성(현 variant는 독립 두 public Join 경합이라 부적합).
- **DoD**: 계약 fault point variant 그린.

## D2. G-2 전체 샘플 실행 (zoneworld 제외 6샘플×언어)

- **범위**: Bingo, DeliveryDispatch, GameQuest, ShoppingMall, SupportChat, TicTacToe × 4언어.
  **flake 안정화 포함**(TicTacToe 간헐 — baseline에서도 4회 중 1회, node G-2 결정적 6/6 아님).
- **DoD**: 6샘플×언어 결정적 성공(최종 HEAD 재확인).

## D3. G-3 java/kotlin 집계 게이트 · doc 게이트 · harness all 최종 확인

- **DoD**: 집계 게이트 + doc 게이트 + harness `all` 스테이지 최종 그린.

## D4. G-4 최종 보고 매트릭스

- **범위**: G-1~G-3 결과를 최종 보고에 매트릭스로 첨부 + clean-break 마이그레이션(Redis 상태
  drain) 명기.
- **DoD**: 최종 보고 완성.

## D5. 최종 게이트 일괄 (완료 판정)

- **범위**: 4언어 unittest + 6샘플×언어 + doc 게이트 + sol 전 문서 spec-gap 리뷰 통과.
- **DoD**: 캠페인 완료 조건(체크리스트 line 24-25) 전부 충족.

---

# 권장 세션 순서 (의존성 기반)

1. **A1**(진행 중) → **A3** → **A2** → **A4** — canonical 4언어 수신·발신 활성화
2. **A5** → **A6** → **A7** — attempt-lifecycle·매트릭스·dialect 제거
3. **B1/B2/B3**, **C1** — A와 병렬 착수 가능(독립 세션)
4. **D1~D5** — A·B·C 수용 후 최종

병렬 가능 조합: (A 트랙 1세션) + (B 트랙 1세션) + (C 트랙 1세션)을 서로 다른 세션에서 동시 진행.
각 세션은 위 카드의 선행조건만 만족하면 독립 착수 가능.
