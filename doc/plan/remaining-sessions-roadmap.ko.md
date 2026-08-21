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

## 새 세션 온보딩 — 착수 전 읽기 순서

새 세션 작업자는 **이 순서로** 읽고 시작한다:

1. **이 문서**(활성 트래커) — 어떤 세션을 집을지 고르고, 그 카드의 선행조건·DoD·스펙 확인.
2. **`framework/AGENTS.md`** + 해당 언어 **`framework/languages/<lang>/AGENTS.md`** — 빌드·게이트·트리 규칙.
   (현재 존재: `framework/AGENTS.md`, `framework/doc/AGENTS.md`, `framework/languages/dotnet/AGENTS.md`.)
3. **auto-memory** `/home/hep7/.claude/projects/-home-hep7-project-zlink/memory/MEMORY.md`
   — 환경·테스트 quirk, 에이전트 정책, 언어별 게이트 명령, 알려진 flake.
4. 집은 트랙의 **핵심 스펙**(아래 §스펙 인덱스, 각 트랙 카드의 "핵심 스펙" 줄).
5. 필요 시 **세션 근거 plan**(config-6-10 / f-e2e-inventory / canon-s4c-s4d) + **이력**
   `doc/plan/archive/relocation-campaign-checklist.ko.md`(커밋해시·과거 판정).

## 스펙 문서 인덱스 (server spec, `framework/doc/framework/common/spec/server/`)

각 파일은 `NN-name.ko.md` + `NN-name.en.md` 쌍으로 존재한다. **스펙 수정은 Claude 단독**(agent 금지).

| 스펙 | 파일 | 이 캠페인에서의 역할 |
|---|---|---|
| **51** internal-service-wire-protocol | `51-internal-service-wire-protocol.{ko,en}.md` | **canonical actor-join(28) 본체.** §9 "Receiver Stable-Type Resolution", "Admission Lifecycle And Abandonment Cleanup"(2026-08-21 신설), §4 admission. **Track A 1순위.** |
| **15** spot-actor | `15-spot-actor.{ko,en}.md` | §4.2 "다른 node의 spot으로 actor를 join하는 순서", factory stable type=Store Authority row. |
| **14** actor-model | `14-actor-model.{ko,en}.md` | Actor 수명주기·정체성 기반. |
| **21** location-runtime | `21-location-runtime.{ko,en}.md` | §2.3/§2.4 Authority row·descriptor 스키마(canonical key `authority\0actor\0{ActorId}`). |
| **22** location-store-redis | `22-location-store-redis.{ko,en}.md` | §7 store 레코드 규범·counter 3행. |
| **23** relocation-store-redis | `23-relocation-store-redis.{ko,en}.md` | §8 relocation blob 규범(raw STRING PSETEX). |
| **28** relocation-flow | `28-relocation-flow.{ko,en}.md` | §3 assembly identity-fencing, §4.2 direct transfer payload(logical stream), §12 bounded generation. |
| **52** internal-relocation-handoff | `52-internal-relocation-handoff.{ko,en}.md` | relocationState(52) wire·handoff. |
| **44** internal-relocation-continuity | `44-internal-relocation-continuity.{ko,en}.md` | relocation continuity 내부 계약. |
| **18** object-routing | `18-object-routing.{ko,en}.md` | §487 command 44/45(one-way, 무재시도) 라우트 갱신. |
| **20** session-actor-dispatch | `20-session-actor-dispatch.{ko,en}.md` | reply/completion terminal, Join completion. |
| **48** internal-session-binding | `48-internal-session-binding.{ko,en}.md` | bound Session seal(42/43)·route(44) — canon-S4d-b. |
| **26** message-flow-tracing | `26-message-flow-tracing.{ko,en}.md` | **디버깅 1순위**(임시 로그 대신 이 트레이싱). |
| **32** framework-error-model | `32-framework-error-model.{ko,en}.md` | typed terminal·DeadlineExceeded·u64 규칙 근거. |
| **13** mesh-node | `13-mesh-node.{ko,en}.md` | connection admission·generation·중복 선택. |
| **30/31** host-relocation·failover | `30-host-relocation-flow`, `31-failure-failover-policy` | 호스트 relocation·실패 정책(Track B/D). |

## 프로토콜 도구 (`framework/runtime/protocol/`)

- **스키마**: `service-wire-v1.schema.json` (command body 계약).
- **생성기**: `generate-service-wire-pilot-codecs.mjs`(4언어 코덱 생성, `--check` 무결성),
  `generate-service-wire-assets.mjs`.
- **검증기**: `validate-service-wire-schema.mjs`, `verify-service-wire-decoder-fixtures.mjs`.
- **golden 픽스처**: `golden/actor-join-request-v1.json`, `golden/actor-join-reply-v1.json`(수신
  chunk-limit tail 고정), `golden/store-record-v1.json`, `golden/relocation-{control,data-chunk,
  envelope,manifest}-v1.json` 등 — 4언어 byte-동치 conformance 기준.

---

# Track A — Canonical actor-join 마이그레이션 (최대·최심)

**핵심 스펙**: 51 §9(1순위) · 15 §4.2 · 14 · 21 §2.4 · 22 §7 · 28 §3/§4.2/§12 · 52 · 44 ·
18 §487(cmd44) · 20(completion) · 48(bound Session). **golden**: actor-join-request/reply,
store-record, relocation-*. **도구**: service-wire-v1.schema.json + 생성기 `--check`.

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

# Track B — 모든 e2e 테스트 추가·구현·실행 (config/F + 하니스 안정화)

이 트랙은 **"모든 e2e 테스트를 추가하고 실행"**하는 단계다. 성격상 순수 문서 작업이 아니라
**① 시나리오 드라이버 코드 작성 → ② dispatch/aggregate runner 연결(wiring) → ③ 실행·green
확보**이며, authoring 중 문서화된 동작이 실제 미구현이면 그건 결함으로 드러나 framework 수정으로
번질 수 있다(coverage 중심이나 correctness 예외 존재).

**⚠️ 선행조건 — 대상 하니스 안정성**: 현재 e2e는 "일부만 동작"한다. 두 종류로 나뉜다:
- **(a) 미authoring** = config6 14건·config10 Track E/G/H/I ~28건(헤딩만·코드 부재) → **이 트랙의
  작업 대상**(방해물 아님).
- **(b) authoring됐으나 불안정** = 아래 **B0 하니스 안정화**로 별도 선행/병행. 새 시나리오가
  불안정 스테이지(relocation 레이스 등)에 의존하지 않는 영역부터 authoring하되, **최종 게이트
  (Track D)는 전부 green이어야** 하므로 B0는 반드시 해소된다.

**핵심 스펙**: 15(spot-actor) · 18(object-routing) · 28(relocation-flow) · 30(host-relocation) ·
31(failover). **세션 근거 plan**: `doc/plan/config-6-10-authoring-plan.ko.md`(B1/B2),
`doc/plan/f-e2e-inventory-plan.ko.md`(B3). **시나리오 소스**: 각 언어 sample/e2e 하니스 +
`feature-map.ko.md`(구현/blocked 표기).

## B0. 하니스 안정화 (불안정 e2e 해소 — Track B/D 선행)

- **H-10**: dotnet→java relocation 스테이지 저빈도 레이스 → 기본 게이트 미편입 상태. 결정적
  수정 후 harness 기본 `all`에 편입(무음 편입 금지).
- **TicTacToe 간헐 flake**: baseline에서도 4회 중 1회 실패(node G-2 결정적 6/6 아님) → 안정화.
- **ST-C4**(= D1): 계약 fault point variant 재작성.
- **DoD**: 세 항목 결정적 green, harness 기본 매트릭스 편입.

## 실행 방식 (필수) — 항목별 4언어 lockstep + spec-gap 게이트

전체 e2e를 언어별로 몰아서 끝내지 않는다. **e2e 항목(시나리오) 단위로, 다음을 반복**한다:

1. **한 e2e 항목**을 고른다.
2. 그 **동일 항목을 4언어(node · dotnet · java/kotlin · cpp)에서 순차적으로 동작 확인** — 같은
   시나리오가 모든 언어에서 동일하게 통과하는지.
3. **spec-gap 미발생 확인** — 4언어 동작이 단일 스펙과 일치하고, 언어 간 발산(사설 방언·동작
   차이)이 새로 생기지 않았는지. (발산 발견 시 = 스펙 오류/구현 결함 → Claude가 판정·전파,
   agent 임의 스펙 수정 금지.)
4. 그 항목이 **4언어 전부 green + spec-gap clean**이면 **다음 e2e 항목으로** 넘어간다.

이유: 언어별로 몰아서 하면 상호운용 발산이 뒤늦게 무더기로 드러난다(캠페인 내내 반복된 실패
양상). 항목 단위 lockstep은 발산을 즉시 잡아 캠페인 근간(4언어 동일 프로토콜)을 보존한다.

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

**핵심 스펙**: 51(service-wire) · 41(internal-serialization). **도구**(1순위):
`framework/runtime/protocol/generate-service-wire-pilot-codecs.mjs`(+`--check`),
`validate-service-wire-schema.mjs`, `service-wire-v1.schema.json`, `golden/*` byte-동치 기준.

## C1. H-6 W-3 4언어 손 코덱 → 생성 코덱 스왑

- **범위**: 4언어 surface별 hand 코덱을 생성 코덱으로 교체, surface별 byte-동치 게이트.
  파일럿 생성기 존재(`framework/runtime/protocol/generate-service-wire-pilot-codecs.mjs`,
  `--check`). C-7 최종 수용(W-4)이 이 위에서 매트릭스 그린으로 판정.
- **DoD**: 4언어 생성 코덱 채택, byte-동치 게이트 그린, 교차 매트릭스 그린.

---

# Track D — 최종 게이트 (A/B/C 결과 수용)

**핵심 스펙**: 24(monitoring) · 25(metrics) · 31(failover) · 32(error-model). **게이트 명령**:
언어별 unittest·6샘플·harness `all`·doc 게이트(memory 및 각 AGENTS.md 참조). **완료 조건**:
아카이브 체크리스트 line 24-25(A·B·C 전 항목 + 전 테스트 그린 + 6샘플×언어 + sol spec-gap 리뷰).

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

# Track Z — ZoneWorld 구현 (7번째 샘플 완성)

**핵심 스펙**: 15(spot-actor) · 17(stage-wrapper) · 28(relocation-flow) · 30(host-relocation).
**현 상태**: ZoneWorld는 4언어에 샘플 디렉토리·빌드 타깃·일부 contract/regression 테스트가
존재하나(node/dotnet/cpp `samples/ZoneWorld`, `shared_sample/zoneworld`), **G-2 6샘플 게이트에서
제외**돼 있다(topology·relocation 복합 샘플이라 미완성).

## Z1. ZoneWorld 4언어 구현 완성 + 게이트 편입

- **범위**: ZoneWorld 도메인·topology·maintenance 동작을 4언어에서 완성해 다른 6샘플과 동일하게
  결정적 실행. 항목별 4언어 lockstep(Track B 실행 방식과 동일)으로 진행.
- **선행**: 6샘플(Bingo/DeliveryDispatch/GameQuest/ShoppingMall/SupportChat/TicTacToe) 전부
  결정적 green(= D2 완료).
- **DoD**: ZoneWorld 4언어 결정적 green + 샘플 게이트에 편입(더 이상 "zoneworld 제외" 아님).

---

# 실행 순서 (세션 시퀀스) — 각 단계 = 독립 세션, 순차 실행

아래는 **독립 세션에서 순차적으로 집어 실행**하는 마스터 순서다. 각 단계는 앞 단계 DoD가
충족돼야 착수한다. (A·C는 canonical 전송 마이그레이션 스트림으로, 초반에 진행하거나 별도
세션 스트림으로 병행 가능하나, **아래 6·7·8단계 제품-완성 tail 순서는 고정**이다.)

1. **A1**(진행 중) — .NET canonical 28 수신자 wire-연결.
2. **A3 → A2 → A4** — .NET 발신 재작업 → cpp 수신자 → cpp 발신 (canonical 4언어 수신·발신 활성화).
3. **A5 → A6 → A7** — attempt-lifecycle/bound Session → 크로스랭 매트릭스 → 사설 dialect 제거.
4. **C1** — W-3 4언어 생성 코덱 스왑(canonical 안정화 후, 또는 A와 병행 스트림).
5. **B0** — 하니스 안정화(H-10 relocation 레이스 · TicTacToe flake · ST-C4/D1). 6·8단계 선행.
6. **D2 — 6샘플×4언어 결정적 green**(Bingo·DeliveryDispatch·GameQuest·ShoppingMall·SupportChat·
   TicTacToe).
7. **Z1 — ZoneWorld 구현**(7번째 샘플 완성 + 게이트 편입).
8. **D3 → D4 — 집계·doc·harness 게이트 + 매트릭스 보고 준비**(java/kotlin 집계·doc 게이트·harness
   `all`; canonical·6샘플·ZoneWorld 결과 수용).
9. **B1 → B2 → B3 — 모든 e2e 테스트 추가·구현·실행 [맨 마지막 단계]**. **항목별 4언어 lockstep +
   spec-gap 게이트**(Track B "실행 방식" 필수 준수): 한 e2e 항목을 4언어 동시 확인 → spec-gap
   없음 확인 → 다음 항목. **왜 마지막인가**: e2e는 가장 포괄적인 교차-언어 검증이라, canonical
   마이그레이션(A)·ZoneWorld(Z)까지 통합된 **최종 상태**를 대상으로 해야 의미가 있다. 앞 단계가
   끝나기 전 e2e를 채우면 이후 변경으로 재작업된다.
10. **D5 — 최종 완료 판정**(e2e green 포함 전 조건: 전 테스트 green + 6샘플+ZoneWorld×언어 +
    모든 e2e + sol spec-gap 리뷰 통과 = 캠페인 종료). ※ 이는 작업이 아니라 sign-off.

**tail 고정 순서(사용자 확정)**: … → **6샘플 완료** → **ZoneWorld 구현** → **(집계·보고 게이트)**
→ **모든 e2e 추가·실행 [맨 마지막 작업 단계]** → **최종 완료 판정(sign-off)**.

병행 허용: A 스트림(1~3) · C(4) · B0(5)는 서로 다른 세션에서 동시 진행 가능(파일 소유권 분리).
단 6→7→8→9→10 tail은 순차이며, **9(모든 e2e)가 마지막 작업 단계**다.
