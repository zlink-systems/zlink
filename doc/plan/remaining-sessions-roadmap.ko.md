# Relocation 캠페인 — 남은 작업 세션별 로드맵 (작업 순서)

작성 2026-08-21. 이 문서는 기존 통합 체크리스트(1178줄, 이력·커밋해시 포함 —
`doc/plan/archive/relocation-campaign-checklist.ko.md`로 아카이브)의 **미결 항목만** 뽑아,
**독립 세션에서 순차적으로 집어 진행할 수 있도록 작업 순서(단계 1→10)대로** 재구성한 것이다.
이 문서가 앞으로의 활성 트래커다. 각 단계 카드는 목표·문제·구체 단계·완료조건(DoD)·선행조건·
근거 스펙을 담는다. (본문의 "체크리스트 line/항목 …" 참조는 아카이브된 그 문서를 가리킨다.)

## 0. 현재 진실 (착수 전 반드시 읽기)

- **제품은 동작한다(JSON/legacy 경로).** 4언어 유닛·6샘플·harness가 green인 것은 사설
  actor-join 전송(JSON dialect)으로 동작하기 때문이다. canonical wire는 아직 **게이트가 닫혀**
  생산 경로가 아니다(체크리스트 C-5/C-6/line185 참조).
- **canonical 마이그레이션(단계 1~3)은 그 legacy를 spec 51 §9 통합 wire로 "교체"하는 작업**이다.
  코드 골격은 있으나 활성화(수신자 wire 연결·발신 게이트 개방·dialect 제거)가 미완이다.
  체크리스트가 직접 H-12/H-15를 "미구현·4언어 공통 대형 트랙"이라 부른다.
- **작업의 성격상 두 부류**: canonical 마이그레이션(1~3)·코덱(4)은 **내부 전송 교체**,
  안정화(5)·샘플(6)·ZoneWorld(7)·e2e(9)는 **검증 확장**. 후자는 현 JSON 위에서도 진행 가능하나,
  **e2e(9)는 canonical까지 통합된 최종 상태에서 해야** 하므로 맨 마지막이다(단계 9 참조).

## 진행 규칙 (요약; 상세는 memory + 아카이브 체크리스트 §진행규칙)

- 작업 agent = `codex exec -m gpt-5.6-terra -c model_reasoning_effort="high" -s danger-full-access --skip-git-repo-check`,
  리뷰 = `codex exec -m gpt-5.6-sol ...`, 리서치 = codex.
- **Claude가 모든 agent 결과를 독립 검증**(diff 정독 + stash-baseline 재현 + 독립 테스트) **후
  sol 리뷰**, 두 결과 대조 후 판정·커밋. 맹신 금지.
- **스펙 문서 규율(엄수)**: `framework/doc/**`(스펙·가이드·인터페이스)는 **sub 에이전트가 절대
  수정하지 않는다.** 오직 **main 작업자(Claude)**만 편집하며, 편집은 **딱 두 경우로 제한**한다:
  ① **진짜 오류 수정**(모순·상호운용 파괴 조항 등 스펙 버그), ② **명확화 개선**(침묵·모호한 부분을
  명시화 — 단 **의도된 규범 의미·동작은 그대로 두고** 더 분명히 적는 것). **임의로 스펙 자체를
  변경(규범 요구·동작을 바꾸거나 구현 편의로 완화)하는 것은 금지.** ②의 예: §9 moving-state
  명문화(기존 "provisional secure"의 함의를 바꾸지 않고 dispatch 불가 상태임을 명시). 판정·4언어
  전파도 Claude 단독. **스펙 문서는 순수 규범 텍스트로만 존재한다**: 진행로그·날짜·finding 번호·sol/리뷰 언급·
  커밋해시·"신설/기각한 대안" 같은 결정-로그성 문구를 스펙에 넣지 않는다(그런 기록은 plan 문서·
  커밋 메시지·아카이브에만). agent에게 스펙 근거를 줄 때는 **읽기 전용으로 인용**하고 "수정 금지"를
  프롬프트에 명시한다.
- 커밋은 Claude가 파일 명시 `git add`(광역 `-A` 금지). agent `git reset/checkout/stash` 금지.
- u64 규칙: 전범위 opaque equality 토큰(lifecycle/node generation, correlation, replyRouteId,
  OperationId, handoff/transfer/completion id)은 `== 0`/`!= 0`만. bounded counter(Object/
  AuthorityOwner/OwnerLease/DescriptorRevision, 1..2^63-1)만 `<= 0` 허용.
- 디버깅은 기존 message-flow 트레이싱(spec 26)부터. 임시 콘솔 로그 추가-후-재실행 금지.
- **최대 병렬(기본 원칙)**: 가능한 한 최대로 병렬 진행한다. 코디네이터(Claude)는 지시를
  기다리지 말고 **유휴 자원(언어 트리·리뷰·검증·문서)이 생기는 즉시 안전한 병렬 작업을 스스로
  투입**한다. 구체적으로: ① 한 단계 안에서 서로 다른 언어 트리를 건드리는 하위 작업은 동시
  실행(예: 4언어 sweep은 언어별 agent 병렬), ② 병행 가능 스트림(1~3 canonical · 4 코덱 ·
  5 안정화)은 서로 다른 세션에서 동시, ③ agent 실행 중에도 Claude는 독립 작업(문서·다른 트리
  리뷰·검증)을 채운다. **직렬화는 실제 자원 충돌이 있을 때만** — 같은 언어 트리·같은 빌드
  디렉터리·타이밍 민감 e2e·공유 상태(redis·포트). 병렬 agent는 **파일 소유권 분리**로 충돌
  방지(커밋도 파일 명시 나열). 검증·sol 리뷰도 서로 다른 대상이면 병렬로 돌린다.
- **커밋·푸시 시점**: 각 그린 마일스톤(단계·하위카드 DoD 충족, 게이트 무회귀)마다 **즉시 커밋
  하고 push**한다. 미커밋 상태를 오래 두지 않는다(공유 트리 오염·유실 위험 최소화). 커밋 메시지는
  backtick/`<`/`>` 회피(bash `-m` 손상) — `git commit -F <file>` 사용. 각 세션은 최소 1회
  green 커밋+push로 마감한다.
- **리팩토링(각 단계에 상시 포함)**: 기능 완료로 끝내지 말고, 그 단계 범위 안에서
  ① **성능** 개선(핫패스 할당·복사·잠금 경합 제거, 불필요한 async/await·박싱 정리),
  ② **POSDDD**(Prove-Once → 죽은 코드는 oracle/커버리지로 미도달 증명 후 **일괄 삭제**;
  추측 삭제 금지, 반드시 미사용 증명 동반), ③ **불필요 코드 정리**(중복·미사용 심볼·주석화된
  잔재·과도기 폴백 제거)를 함께 수행한다. 리팩토링도 동일하게 **검증(테스트 무회귀)+sol 리뷰+
  커밋**을 거친다. 큰 정리는 별도 커밋으로 분리(리뷰 용이).

## spec-gap 확인 기준 (모든 변경에 적용, 3단계 차등)

**spec-gap** = 단일 스펙을 4언어가 동일 구현한다는 근간이 깨지는 것: ① 코드가 스펙 조항과
불일치, ② 스펙이 **침묵**하는 케이스를 언어마다 다르게 처리(사설 발산), ③ .ko/.en 스펙 불일치.
확인 깊이는 **변경이 무엇을 건드렸는지**에 따라 차등한다.

- **L0. 트리아지 (모든 변경, 즉시)**: "spec 지배 표면(wire byte · terminal code · Store 레코드
  필드 · admission/relocation 의미 · generation/token 의미 · 코덱)을 건드렸나?" 아니면(순수
  단일언어 내부 리팩터·관측 동작 무변화) 여기서 종료. 맞으면 L1.
- **L1. 교차언어 parity + 조항 대조 (protocol/contract 변경 시 필수)**:
  (a) 같은 표면을 **다른 3언어 동작과 비교**해 일치 확인, (b) wire/store를 건드렸으면
  **golden/conformance byte-동치** + 해당 **cross-language harness 스테이지** 실행,
  (c) 지배 **스펙 조항 정독** — 코드가 문구와 정확히 일치하는가; **조항이 침묵하는 케이스를 코드가
  새로 처리하면 = 잠재 gap** → Claude가 판정·4언어 전파(agent 임의 스펙 수정 금지).
  ※ sol 리뷰는 (c) 조항-대조는 잘 잡지만 (a) parity는 Claude가 별도로 본다 — sol 하나로 끝내지 않음.
- **L2. 적대적·전면 (단계 경계 + 최종 sign-off 전 필수)**: **codex sol 전 문서 spec-gap 리뷰**
  (과거 13건 방식). 단계 9(모든 e2e)는 **항목별 4언어 lockstep**이 매 항목 L1을 강제하므로
  gap을 즉시 잡는다.

원칙: gap은 "코드≠스펙"만이 아니라 **"스펙이 안 정한 것을 코드가 정해버림"**으로도 생긴다 —
후자는 조용히 구현하지 말고 **Claude 스펙 판정**으로 승격해 4언어에 전파한다.

## 새 세션 온보딩 — 착수 전 읽기 순서

새 세션 작업자는 **이 순서로** 읽고 시작한다:

1. **이 문서**(활성 트래커) — 지금 진행할 단계를 고르고(진행 상태 표시 확인), 그 카드의
   선행조건·DoD·근거 스펙 확인.
2. **`framework/AGENTS.md`** + 해당 언어 **`framework/languages/<lang>/AGENTS.md`** — 빌드·게이트·트리 규칙.
   (현재 존재: `framework/AGENTS.md`, `framework/doc/AGENTS.md`, `framework/languages/dotnet/AGENTS.md`.)
3. **auto-memory** `/home/hep7/.claude/projects/-home-hep7-project-zlink/memory/MEMORY.md`
   — 환경·테스트 quirk, 에이전트 정책, 언어별 게이트 명령, 알려진 flake.
4. 그 단계의 **근거 스펙**(아래 §스펙 인덱스 + 각 단계 카드의 "근거 스펙" 줄).
5. 필요 시 **세션 근거 plan**(config-6-10 / f-e2e-inventory / canon-s4c-s4d) + **이력**
   `doc/plan/archive/relocation-campaign-checklist.ko.md`(커밋해시·과거 판정).

## 스펙 문서 인덱스 (server spec, `framework/doc/framework/common/spec/server/`)

각 파일은 `NN-name.ko.md` + `NN-name.en.md` 쌍으로 존재한다. **스펙 수정은 Claude 단독**(agent 금지).

| 스펙 | 파일 | 역할 | 관련 단계 |
|---|---|---|---|
| **51** internal-service-wire-protocol | `51-internal-service-wire-protocol.{ko,en}.md` | **canonical actor-join(28) 본체.** §9 "Receiver Stable-Type Resolution", "Admission Lifecycle And Abandonment Cleanup"(2026-08-21 신설), §4 admission. | 1·2·3·4 |
| **15** spot-actor | `15-spot-actor.{ko,en}.md` | §4.2 다른 node의 spot으로 actor join 순서, factory stable type=Store Authority row. | 1·2·7·9 |
| **14** actor-model | `14-actor-model.{ko,en}.md` | Actor 수명주기·정체성. | 1·2 |
| **21** location-runtime | `21-location-runtime.{ko,en}.md` | §2.3/§2.4 Authority row·descriptor(`authority\0actor\0{ActorId}`). | 1·2 |
| **22** location-store-redis | `22-location-store-redis.{ko,en}.md` | §7 store 레코드 규범·counter 3행. | 1·2 |
| **23** relocation-store-redis | `23-relocation-store-redis.{ko,en}.md` | §8 relocation blob(raw STRING PSETEX). | 2·3 |
| **28** relocation-flow | `28-relocation-flow.{ko,en}.md` | §3 assembly identity-fencing, §4.2 direct transfer payload(logical stream), §12 bounded generation. | 2·3·7·9 |
| **52** internal-relocation-handoff | `52-internal-relocation-handoff.{ko,en}.md` | relocationState(52) wire·handoff. | 2·3 |
| **44** internal-relocation-continuity | `44-internal-relocation-continuity.{ko,en}.md` | relocation continuity 내부 계약. | 2·3 |
| **18** object-routing | `18-object-routing.{ko,en}.md` | §487 command 44/45(one-way, 무재시도) 라우트 갱신. | 3·9 |
| **20** session-actor-dispatch | `20-session-actor-dispatch.{ko,en}.md` | reply/completion terminal, Join completion. | 2·3 |
| **48** internal-session-binding | `48-internal-session-binding.{ko,en}.md` | bound Session seal(42/43)·route(44). | 3 |
| **17** stage-wrapper-on-spot | `17-stage-wrapper-on-spot.{ko,en}.md` | stage wrapper(ZoneWorld topology). | 7 |
| **26** message-flow-tracing | `26-message-flow-tracing.{ko,en}.md` | **디버깅 1순위**(임시 로그 대신 이 트레이싱). | 전 단계 |
| **32** framework-error-model | `32-framework-error-model.{ko,en}.md` | typed terminal·DeadlineExceeded·u64 규칙 근거. | 전 단계 |
| **13** mesh-node | `13-mesh-node.{ko,en}.md` | connection admission·generation·중복 선택. | 1·2 |
| **41** internal-serialization | `41-internal-serialization.{ko,en}.md` | 직렬화(생성 코덱 스왑). | 4 |
| **30/31** host-relocation·failover | `30-host-relocation-flow`, `31-failure-failover-policy` | 호스트 relocation·실패 정책. | 5·9 |
| **24/25** monitoring·metrics | `24-runtime-monitoring`, `25-runtime-metrics` | 최종 게이트 관측. | 8·10 |

## 프로토콜 도구 (`framework/runtime/protocol/`)

- **스키마**: `service-wire-v1.schema.json` (command body 계약).
- **생성기**: `generate-service-wire-pilot-codecs.mjs`(4언어 코덱 생성, `--check` 무결성),
  `generate-service-wire-assets.mjs`.
- **검증기**: `validate-service-wire-schema.mjs`, `verify-service-wire-decoder-fixtures.mjs`.
- **golden 픽스처**: `golden/actor-join-request-v1.json`, `golden/actor-join-reply-v1.json`(수신
  chunk-limit tail 고정), `golden/store-record-v1.json`, `golden/relocation-{control,data-chunk,
  envelope,manifest}-v1.json` 등 — 4언어 byte-동치 conformance 기준.

## 전체 순서 한눈에

```
[canonical 마이그레이션]        [검증 확장]                       [완료]
1 A1 .NET 수신자 wire연결  ─┐   5 B0 하니스 안정화  ─┐
2 A3→A2→A4 발신·수신 활성 ─┤   6 D2 6샘플 green      │
3 A5→A6→A7 lifecycle·제거 ─┘   7 Z1 ZoneWorld 구현   │
4 C1 W-3 코덱 스왑               8 D3→D4 집계·보고게이트 │
                                9 B1→B2→B3 모든 e2e ◀ 맨 마지막 작업
                                10 D5 최종 완료 판정(sign-off)
```
- 병행: 1~3(canonical)·4(코덱)·5(안정화)는 서로 다른 세션에서 동시 진행 가능(파일 소유권 분리).
- **고정 tail(사용자 확정)**: 6 → 7 → 8 → **9 모든 e2e(맨 마지막 작업)** → 10 sign-off.

## 진행 현황 체크리스트

상태 표기: `[ ]` 미착수 · `[~]` 진행 중 · `[x]` 완료(커밋해시 병기). **카드 DoD 충족 시 즉시
체크하고 커밋해시를 적는다**(진행 규칙의 커밋·push 원칙과 동일). sol CLEAN + Claude 독립검증
통과가 `[x]`의 조건이다.

**이미 랜딩된 기반(참고, 이 로드맵 이전)**: spec 51 §9 admission-lifecycle ruling `8b8f3c9a36` ·
.NET 28 수신 decouple+seam 바인딩 제거(clean subset) `ab26a6527b` · node/java canonical 발신(S4c)
· 4언어 canonical 수신자 골격(단 .NET wire-ingress 미연결=단계 1) · S4d seam 타입 `0407b3e4bc`.

언어범위 태그: **`dotnet단일`** · **`cpp단일`** · **`4언어`**(node·dotnet·JVM(java+kotlin)·cpp
동시) · **`혼합`**. (kotlin은 java 프레임워크 공유 — 샘플만 kotlin.) 단일언어 단계도 종료 시
**L1 parity(다른 3언어 대조)** 필수.

- [~] **단계 1 — [A1] .NET 수신자 wire-ingress 연결** `dotnet단일` (sol NOT-CLEAN 7건 — 랜딩 보류, #1 §9 membership tension advisor 판정 대기)
- [ ] **단계 2 — canonical 수신·발신 활성화**
  - [ ] 2a [A3] .NET 발신 재작업(sol 6건 반영) `dotnet단일`
  - [ ] 2b [A2] C++ 수신자 완성(H-12) + chunk limit(H-14) `cpp단일`
  - [ ] 2c [A4] C++ 발신(S4c-cpp) `cpp단일`
- [ ] **단계 3 — attempt-lifecycle · 매트릭스 · dialect 제거**
  - [ ] 3a [A5] attempt-lifecycle / bound Session(S4d·S4d-b) `혼합`(node/java 검증→4언어 전파)
  - [ ] 3b [A6] 크로스랭 canonical 매트릭스(S4e) `4언어`
  - [ ] 3c [A7] 사설 dialect 제거(H-15/S5) `4언어`
- [ ] **단계 4 — [C1] W-3 생성 코덱 스왑(H-6)** `4언어`
- [ ] **단계 5 — [B0] 하니스 안정화** `혼합`
  - [ ] H-10 dotnet→java relocation 레이스 (dotnet+java)
  - [ ] TicTacToe 간헐 flake (샘플별)
  - [ ] D1 ST-C4 fault-injection variant (cpp)
- [ ] **단계 6 — [D2] 6샘플 × 4언어 결정적 green** `4언어`
- [ ] **단계 7 — [Z1] ZoneWorld 구현(7번째 샘플)** `4언어`
- [ ] **단계 8 — [D3·D4] 집계·doc·harness 게이트 + 보고 준비** `혼합`(집계=주로 JVM, 보고=전체)
- [ ] **단계 9 — 모든 e2e 추가·구현·실행 (맨 마지막 작업)** `4언어`(항목별 lockstep)
  - [ ] B1 config-6 (14 시나리오)
  - [ ] B2 config-10 Track E/G/H/I (~28 시나리오)
  - [ ] B3 F e2e_inventory backlog
- [ ] **단계 10 — [D5] 최종 완료 판정(sign-off)** `전체`

---

# 단계 1 — [A1] .NET canonical 28 수신자 wire-ingress 연결  ⟶ (진행 중)

**근거 스펙**: 51 §9(Receiver Stable-Type Resolution) · 15 §4.2 · 21 §2.4.

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

---

# 단계 2 — canonical 4언어 수신·발신 활성화

## 2a. [A3] .NET canonical 28 발신 (재작업 — sol 6건 반영)

**근거 스펙**: 51 §9 · 20(completion). **근거 plan**: `canon-s4c-s4d-sender-integration-plan.ko.md`.

- **배경**: 1차 발신 시도는 sol NOT-CLEAN 6건으로 clean subset(decouple+바인딩 제거)만 landing,
  origination(발신+gate+수신 dispatch+completion) revert. 6건:
  (1) 수신 dispatch 미연결 = 단계 1이 해소, (2) 28-accept 시 membership commit(=node와 동일 형상,
  moving 게이트로 dispatch 차단 확인 → ruling 위반 아님), (3) empty recovery로 public Join
  completion 유실, (4) entry 하드코딩 false, (5) bound-session을 admission 제외로 처리(ruling은
  seal/route leg만), (6) production invariant 미테스트.
- **DoD**: 단계 1 위에서 발신 재구현, (3)(4)(5)(6) 해소, 전체 게이트 무회귀, sol CLEAN.
- **선행**: 단계 1(수신자 없으면 .NET↔.NET 왕복 검증 불가).

## 2b. [A2] C++ canonical 28 수신자 완성 (H-12) + chunk limit (H-14)

**근거 스펙**: 51 §9 · 15 §4.2 · 21 §2.4 · 28.

- **문제**: cpp canonical 경로가 (a) Store Authority row에서 stableType 해석 미연결
  (`actor_type_from_authority`를 `admit_wire_actor_join`에 연결, sync local-map→async Store),
  (b) Accepted 후 seal/capture/Restore/relay/cutover continuation 미연결, (c) public Accepted가
  target owner CAS+queue-open 후에만. chunk limit(H-14)은 `mesh_node_runtime.cpp:2562`에서 수신
  경로에 미적용(origination revert `ab0b4b39a4`로 dormant).
- **주의**: 체크리스트 slice2(line 126) "canonical-accepted를 `seal_remote_application_actor_join()`
  경유"는 §9 "admission-time seal/commit 금지"와 충돌 — **spec §9에 맞춰 재작성 후 착수**.
- **DoD**: cpp가 canonical 28 수신·admission·chunk limit 적용, focused + 관련 e2e 무회귀, sol CLEAN.
- **선행**: 단계 1(§9 해석 형상 참조), spec 결정 `8d8e5cdffd`(이미 확정).

## 2c. [A4] C++ canonical 28 발신 (S4c-cpp)

**근거 스펙**: 51 §9 · 28 · 52.

- **문제**: cpp canonical send 활성화(`mesh_node_runtime.cpp:2317` gate)가 Bingo·TicTacToe
  샘플 회귀(baseline 통과, S4c 실패 — cpp completion 경로 손상). S4d(relocation/completion
  state 통합) 선행 필요.
- **DoD**: cpp 발신 게이트 개방, Bingo·TicTacToe stash-baseline 대조 무회귀, sol CLEAN.
- **선행**: 2b(cpp 수신자), 단계 3의 seam(A5).

---

# 단계 3 — attempt-lifecycle · 크로스랭 매트릭스 · dialect 제거

## 3a. [A5] Attempt-lifecycle / multi-attempt (S4d attempt-binding, S4d-b bound Session)

**근거 스펙**: 51 §9 · 48(bound Session) · 28 §3.

- **배경**: 28→40 identity 바인딩은 **불필요**로 판정됨(branch A: canonical admission은 40을
  기다리지 않고, 40 reservation은 relocation identity로 별도 트랙). 단 multi-attempt
  later-attempt-wins 재park 정확성은 node/java에서도 미검증(canon-node-java-multiattempt-repark).
- **단계**: (a) node/java multi-attempt 재park 테스트로 branch A/B 확정(코드상 바인딩 없음
  확인됨), (b) 필요 시 target-local supersession 규칙 spec 명문화, (c) bound Session은 28 전
  42/43 seal + target commit 후 44 route update(28 sideband 금지).
- **DoD**: 재park 정확성 테스트 존재, bound Session 분리 동작, sol CLEAN.
- **선행**: 단계 1·2a(수신·발신 활성화 후라야 실경로 검증).

## 3b. [A6] 크로스랭 harness canonical 매트릭스 (S4e)

**근거 스펙**: 51 §9 · 15 §4.2.

- **단계**: User Spot JoinSpot stage를 canonical 28 pairwise 매트릭스(Node↔.NET↔Java↔C++)로,
  JoinEntrySpot 의미 보존. harness 기본 `all`에 편입(무음 편입 금지, flake는 명시 스킵).
- **DoD**: 4언어 pairwise canonical 28 결정적 그린, 매트릭스 편입.
- **선행**: 단계 1·2(4언어 수신·발신 활성화).

## 3c. [A7] 사설 dialect 제거 (H-15 / S5)

**근거 스펙**: 51 §9.

- **단계**: 4언어(cpp typed-JSON·.NET envelope·java newline·node JSON) 상위 경로를 canonical
  28+Store-backed 수신자로 교체 완료 후, 사설 actor-join packet 삭제.
- **DoD**: 사설 dialect 코드 제거, 전 게이트+샘플+harness 그린, sol CLEAN.
- **선행**: 단계 1·2·3a·3b 완료.

---

# 단계 4 — [C1] W-3 4언어 손 코덱 → 생성 코덱 스왑 (H-6)

**근거 스펙**: 51 · 41. **도구**(1순위): `generate-service-wire-pilot-codecs.mjs`(+`--check`),
`validate-service-wire-schema.mjs`, `service-wire-v1.schema.json`, `golden/*` byte-동치 기준.

- **범위**: 4언어 surface별 hand 코덱을 생성 코덱으로 교체, surface별 byte-동치 게이트.
  C-7 최종 수용(W-4)이 이 위에서 매트릭스 그린으로 판정.
- **DoD**: 4언어 생성 코덱 채택, byte-동치 게이트 그린, 교차 매트릭스 그린.
- **선행**: 없음(canonical과 독립, 병행 가능). 단 A7(dialect 제거)과 surface 겹치면 순서 조율.

---

# 단계 5 — [B0] 하니스 안정화 (불안정 e2e 해소 — 단계 6·9 선행)

**근거 스펙**: 28 · 30 · 31. **현재 e2e는 "일부만 동작"** — 아래 셋이 대표 불안정.

- **H-10**: dotnet→java relocation 스테이지 저빈도 레이스 → 기본 게이트 미편입 상태. 결정적
  수정 후 harness 기본 `all`에 편입(무음 편입 금지).
- **TicTacToe 간헐 flake**: baseline에서도 4회 중 1회 실패(node G-2 결정적 6/6 아님) → 안정화.
- **[D1] ST-C4 fault-injection**: config-10 계약 "동일 relocation identity의 checksum/길이
  불일치" assembly 충돌을 계약 fault point 경유로 재작성(현 variant는 독립 두 public Join 경합).
- **DoD**: 세 항목 결정적 green, harness 기본 매트릭스 편입.

---

# 단계 6 — [D2] 6샘플 × 4언어 결정적 green

**근거 스펙**: 24 · 25 · 31 · 32. **게이트 명령**: 각 언어 AGENTS.md·memory 참조.

- **범위**: Bingo · DeliveryDispatch · GameQuest · ShoppingMall · SupportChat · TicTacToe × 4언어.
  (TicTacToe flake는 단계 5에서 해소된 상태여야 함.)
- **DoD**: 6샘플×언어 **결정적** 성공(운좋은 통과 아님, 최종 HEAD 재확인).
- **선행**: 단계 5(flake 안정화). canonical(1~3) 완료 후라면 canonical 전송 위에서 재확인.

---

# 단계 7 — [Z1] ZoneWorld 구현 (7번째 샘플 완성)

**근거 스펙**: 15 · 17(stage-wrapper) · 28 · 30.

- **현 상태**: ZoneWorld는 4언어에 샘플 디렉토리·빌드 타깃·일부 contract/regression 테스트가
  존재하나(node/dotnet/cpp `samples/ZoneWorld`, `shared_sample/zoneworld`), **G-2 6샘플
  게이트에서 제외**돼 있다(topology·relocation 복합 샘플이라 미완성).
- **범위**: ZoneWorld 도메인·topology·maintenance 동작을 4언어에서 완성해 다른 6샘플과 동일하게
  결정적 실행. 항목별 4언어 lockstep(단계 9 실행 방식과 동일 원칙)으로 진행.
- **DoD**: ZoneWorld 4언어 결정적 green + 샘플 게이트에 편입(더 이상 "zoneworld 제외" 아님).
- **선행**: 단계 6(6샘플 전부 결정적 green).

---

# 단계 8 — [D3·D4] 집계·doc·harness 게이트 + 매트릭스 보고 준비

**근거 스펙**: 24 · 25.

- **D3**: java/kotlin 집계 게이트 + doc 게이트 + harness `all` 스테이지 최종 그린.
- **D4**: canonical·6샘플·ZoneWorld 결과를 최종 보고 매트릭스로 정리 + clean-break 마이그레이션
  (Redis 상태 drain) 명기.
- **DoD**: 집계·doc·harness 게이트 그린, 보고 매트릭스 초안.
- **선행**: 단계 1~7.

---

# 단계 9 — [B1·B2·B3] 모든 e2e 테스트 추가·구현·실행  ★ 맨 마지막 작업 단계

**근거 스펙**: 15 · 18 · 28 · 30 · 31. **근거 plan**: `config-6-10-authoring-plan.ko.md`(B1/B2),
`f-e2e-inventory-plan.ko.md`(B3). **시나리오 소스**: 각 언어 sample/e2e 하니스 + `feature-map.ko.md`.

이 단계는 **"모든 e2e 테스트를 추가하고 실행"**한다. 순수 문서 작업이 아니라 **① 시나리오 드라이버
코드 작성 → ② dispatch/aggregate runner 연결(wiring) → ③ 실행·green 확보**이며, authoring 중
문서화된 동작이 실제 미구현이면 결함으로 드러나 framework 수정으로 번질 수 있다(coverage 중심,
correctness 예외 존재).

**왜 맨 마지막인가**: e2e는 가장 포괄적인 교차-언어 검증이라, canonical 마이그레이션(단계 1~3)·
ZoneWorld(단계 7)까지 통합된 **최종 상태**를 대상으로 해야 의미가 있다. 앞 단계 전에 e2e를 채우면
이후 변경으로 재작업된다.

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

## 대상 인벤토리

- **B1 — config-6**: SF-B3/C3/C4/C5/C5A/F1/F4~F10/G1/G2 등 14 시나리오(헤딩만·코드 부재).
- **B2 — config-10**: Track E/G/H/I ~28 시나리오(미구현; 실제 22 dispatch는 orphan 없음).
- **B3 — F e2e_inventory backlog**: `f-e2e-inventory-plan.ko.md` 목록.
- **DoD**: 위 전부 authoring + dispatch 연결 + **4언어 lockstep green + spec-gap clean**.
- **선행**: 단계 1~8 완료(특히 canonical·ZoneWorld·B0 안정화).

---

# 단계 10 — [D5] 최종 완료 판정 (sign-off)

- **범위**: 4언어 unittest + 6샘플+ZoneWorld×언어 + 모든 e2e + doc 게이트 + sol 전 문서 spec-gap
  리뷰 통과.
- **DoD**: 캠페인 완료 조건(아카이브 체크리스트 line 24-25) 전부 충족 = 캠페인 종료.
- ※ 작업이 아니라 sign-off — 단계 1~9 결과를 종합 확인한다.
