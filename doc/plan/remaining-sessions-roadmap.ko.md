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
- **canonical 마이그레이션(단계 1~3)은 actor-join의 wire/저장 포맷만 바꾸는 작업**이다 — 사설
  JSON dialect를 canonical service-wire(command 28) binary로 교체. **relocation/admission 로직은
  이미 완성돼 동작하며, 절대 바꾸지 않는다.** 포맷 변경에서 파생되는 유일한 실제 차이는:
  canonical 28엔 actorType이 wire에 없으므로 **stable type을 Store Authority row에서 해석**(§9)하는
  것 하나뿐. 그 외 admission·temp queue·CAS·cutover·relocation 흐름은 기존 로직을 **그대로 재사용**한다.
  즉 수신자 = thin decode(+Store 타입해석) → 기존 admission 투입. **로직을 새로 만들거나 검증을
  추가하면 그건 잘못된 방향이다**(이번 캠페인에서 이 착각으로 반복 실패). 체크리스트가 H-12/H-15를
  "미구현·4언어 대형 트랙"이라 부르지만, 실제 남은 건 포맷 활성화(수신 decode 연결·발신 게이트·
  dialect 제거)이지 로직 재작성이 아니다.
- **작업의 성격상 두 부류**: canonical 마이그레이션(1~3)·코덱(4)은 **내부 전송 교체**,
  안정화(5)·샘플(6)·ZoneWorld(7)·e2e(9)는 **검증 확장**. 후자는 현 JSON 위에서도 진행 가능하나,
  **e2e(9)는 canonical까지 통합된 최종 상태에서 해야** 하므로 맨 마지막이다(단계 9 참조).

## 진행 규칙 (요약; 상세는 memory + 아카이브 체크리스트 §진행규칙)

- 작업 agent = `codex exec -m gpt-5.6-terra ...`(일반 구현), 리뷰 = `codex exec -m gpt-5.6-sol ...`, 리서치 = codex.
  **고난도 구현은 `gpt-5.6-sol`로 진행**(사용자 지시 — 여러 라운드 STOP·회귀 반복 같은 복잡 통합. 예: cpp canonical 발신 2c). 옵션은 동일(`-c model_reasoning_effort="high" -s danger-full-access --skip-git-repo-check`).
- **Claude가 모든 agent 결과를 독립 검증**(diff 정독 + stash-baseline 재현 + 독립 테스트) **후
  sol 리뷰**, 두 결과 대조 후 판정·커밋. 맹신 금지.
- **스펙 문서 규율(엄수)**: `framework/doc/**`(스펙·가이드·인터페이스)는 **sub 에이전트가 절대
  수정하지 않는다.** 오직 **main 작업자(Claude)**만 편집하며, 편집은 **딱 두 경우로 제한**한다:
  ① **진짜 오류 수정**(모순·상호운용 파괴 조항 등 스펙 버그), ② **명확화 개선**(침묵·모호한 부분을
  명시화 — 단 **의도된 규범 의미·동작은 그대로 두고** 더 분명히 적는 것). **임의로 스펙 자체를
  변경(규범 요구·동작을 바꾸거나 구현 편의로 완화)하는 것은 금지.** 판정·4언어 전파도 Claude
  단독. **스펙 수정 후에는 반드시 리뷰한다**(codex sol로 그 스펙 변경을 검토 — 조항 일관성·타 절
  충돌·4언어 구현과의 정합·의도치 않은 규범 변경 여부). 스펙도 코드와 동일하게 수정→리뷰를 거친다.
  **스펙 문서는 순수 규범 텍스트로만 존재한다**: 진행로그·날짜·finding 번호·sol/리뷰 언급·
  커밋해시·"신설/기각한 대안" 같은 결정-로그성 문구를 스펙에 넣지 않는다(그런 기록은 plan 문서·
  커밋 메시지·아카이브에만). agent에게 스펙 근거를 줄 때는 **읽기 전용으로 인용**하고 "수정 금지"를
  프롬프트에 명시한다.
- 커밋은 Claude가 파일 명시 `git add`(광역 `-A` 금지). agent `git reset/checkout/stash` 금지.
- u64 규칙: 전범위 opaque equality 토큰(lifecycle/node generation, correlation, replyRouteId,
  OperationId, handoff/transfer/completion id)은 `== 0`/`!= 0`만. bounded counter(Object/
  AuthorityOwner/OwnerLease/DescriptorRevision, 1..2^63-1)만 `<= 0` 허용.
- 디버깅은 기존 message-flow 트레이싱(spec 26)부터. 임시 콘솔 로그 추가-후-재실행 금지.
- **검증 단일 책임(relocation/actor-join — 심플 유지, 엄수)**: spec 28 §2 — "한 주체가 다른
  주체의 결정을 반복해서 검증하지 않는다." 계층별 책임을 **한 곳에서만** 검증한다: **Transport**=
  authenticated peer·node 실행세대·frame 형식만, **Target admission(Location Store CAS)**=Store 타입
  해석 + actor/spot/owner fence, **Session owner**=current binding. **fence/type 검증을 transport 등
  여러 곳에 흩뿌리지 말 것** — 과거 relocation 검증을 다중화해 복잡해지며 구현이 두 번 실패했다.
  근본 해결은 항상 **심플**하게(28-relocation-flow.ko.md를 따른다).
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
| **51** internal-service-wire-protocol | `51-internal-service-wire-protocol.{ko,en}.md` | canonical actor-join(28) wire. §9 "Receiver Stable-Type Resolution"(Store Authority row 타입해석·typed terminal), §4 admission. **admission 흐름 권위는 §15 §4.2**. | 1·2·3·4 |
| **15** spot-actor | `15-spot-actor.{ko,en}.md` | **§4.2 다른 node의 spot으로 actor join 순서 = admission 단일 권위**(28 승인에서 temp queue 등록+factory 준비+chunk limit reply, 40이 재사용). factory stable type=Store Authority row. | 1·2·7·9 |
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

- [x] **단계 1 — [A1] .NET 수신자 wire-ingress 연결** `dotnet단일` — `e8409034c6` (포맷만·기존 admission 재사용·단일책임; sol 9/10 해소, 게이트 무회귀. fast-follow: 실제 wire→admission 통합 테스트 = sol #10 LOW/비차단)
- [ ] **canon-2a-sender-app-reply-delivery (follow-up, 단계 3b서 닫음)**: .NET canonical 발신자가 target handler의 non-empty application reply를 조인 caller에 전달하지 못함(sol HIGH, latent — 현재 테스트 미커버). 단계 3b(A6 크로스랭 pairwise app-reply)에서 해소.
- [ ] **canon-A1-real-wire-admission-integration-test (fast-follow)**: ProcessCanonicalActorJoin→AdmitCanonicalActorJoinAsync→AdmitRoutedActorJoinAsync 실제 경로 구동 테스트(accepted tail·Authority-row fence mismatch·TypeMismatch·malformed→ProtocolError·mailbox-full). sol #10(LOW/비차단). 기존 admission이 JSON 테스트로 커버되나 canonical 배선은 미커버.
- [ ] **단계 2 — canonical 수신·발신 활성화**
  - [x] 2a [A3] .NET canonical 28 발신 `dotnet단일` — `b67385822e` (포맷만·canonical reply tail 소비; 회귀 해소·게이트 1809/3·cross-harness 통과; #6 app-reply는 3b서)
  - [x] 2b [A2] C++ 수신자 완성(H-12) + chunk limit(H-14) `cpp단일` — `5f22587b0b` (포맷만·thin transport·Store fence .NET parity·H-14 연결·legacy 판별; sol 3건 해소, ctest green, relocation 로직 무변경)
  - [x] 2c [A4] C++ 발신 — `cpp단일` `7ca95170ac` (production canonical 활성화; continuation bridge·off-wire handoff id .NET byte-parity·app payload·generation equality·terminal 보존·allow-list; sol 구현+리뷰, 6/6 검증·샘플 통과·CAS 무변경. deferred: 수신자 app-reply 전달=3b)
- [ ] **단계 3 — attempt-lifecycle · 매트릭스 · dialect 제거**
  - [~] 3a [A5] attempt-lifecycle / bound Session(S4d·S4d-b) `혼합` — **검증 완료**: canonical 회귀 없음(Property 2 parity PASS, Property 1 3언어 static PASS). Node multi-attempt는 열린 설계 질문으로 3b 이월(시도 fix revert). 상세 아래 §3a.
  - [x] **A6-cpp-app-reply-parity (양방향 완료)** — `dd234c3110` (`cpp단일`, 포맷만): cpp canonical actorJoin app-reply를 node/java/.NET과 양방향 parity로 정렬. sol 2라운드 구현 + sol 2라운드 리뷰 + Claude 독립 검증(diff·클린 재빌드·ctest 5/5·TicTacToe/Bingo).
    - [x] **수신자(send)**: `actor_join_operation_result_t.application_reply` 추가 + `admit_wire_actor_join`가 handler reply를 framework-multipart(`[u32BE count=1][u32BE size][bytes]`) application_payload로 감싸 command-20 tail 뒤 2번째 frame으로 전송; `terminal_result!=0`이면 payload 거부. **.NET `EncodeFrameworkMultipart`/Java `encodeFrameworkMultipart`와 바이트 단위 일치**(BE count+per-part len).
    - [x] **발신자(receive) 언랩**: canonical source가 framework-multipart를 언랩(`unwrap_canonical_actor_join_application_reply`, 첫 파트 반환·non-multipart는 그대로·malformed는 protocol_error)해 handler 실제 reply message를 join caller에 전달. **JSON 경로(unwrapped 저장)·Java `decodeFrameworkMultipart`와 일치**. malformed reply는 negotiation chunk-limit 기록 **전에** 실패(순서 재배치).
    - [x] **serializer 가드 축소**: `canonical_actor_join_application_reply` → `result_t<optional>`; reply 없으면 serializers 없이도 admit, reply 있는데 serializers null이면 typed protocol_error. 기존 동작 회귀 없음.
    - 이월(3b서 자연 해소): ① full source-path end-to-end 커버(Java→cpp/.NET→cpp pairwise가 정확히 구동) ② throwing reply serializer 시 pending-admission unwind(pathological) ③ **.NET source(ZLinkActorRemoteJoiner:1044) non-unwrap 의심 → pairwise서 확인**.
  - [~] 3b [A6] 크로스랭 canonical 매트릭스 `4언어` — **스코핑 완료(대형 트랙)**. canonical User Spot JoinSpot 스테이지 현재 0개(기존 3개는 JoinEntrySpot·`all` 미포함). 남은 필요: ① **12 pairwise 방향**(Node↔.NET↔Java↔C++ 양방향) 신규 스테이지 ② **4언어 cross-language host에 User Spot actor-join mode 신설**(현 entry-spot mode만; C++ host는 relocation mode 자체 없음) ⑤ 통과 스테이지 `all` 편입(무음 금지). **완료: ③ cpp receiver/source app-reply 양방향(`dd234c3110`) ④ .NET app-reply 보존**. 진행: 항목별 4언어 lockstep + spec-gap. 하니스: framework/languages/cpp/cross-language/run_cross_language_smoke.sh. 근거: a6-scope 진단.
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

# 단계 1 — [A1] .NET canonical 28 수신자 wire-ingress 연결

**근거 스펙(권위)**: **15 §4.2**(actor-join admission 단일 권위) · 51 §9 Receiver Stable-Type Resolution · **28 §2**(검증 단일책임). **성격 = 포맷 배선만**: canonical 28 binary를 decode해 **기존 admission 로직에 투입**한다. 로직·검증을 새로 만들지 않는다.

- **문제**: `ZLinkManagedMeshNode.ProcessReceivedAsync`의 수신 whitelist가 ActorJoin(28)을
  제외 → wire로 온 28이 protocol error로 drop. 수신 경로 `PrepareCanonicalActorJoinAsync`
  (`ZLinkFrameworkRuntimeActors.cs:2678`)는 `ZLinkSpotActorJoinDispatcher`에서만 호출돼 raw
  수신 미연결. ("S3 receivers 4/4"가 .NET에선 틀렸음.)
- **단계(thin transport)**: ProcessReceivedAsync가 canonical 28을 decode(생성 decoder+legacy
  guard로 private 28은 fallthrough)하고, **peer/node세대/frame만 확인 후 기존 target admission
  경로에 그대로 라우팅**(fence/type 재검증 금지 — spec 28 §2). 포맷상 유일한 차이 = actorType이
  wire에 없어 **Store Authority row에서 stable type 해석**(§9). entry bool8로 Entry/User Spot 구분.
  admission이 소유한 fence/temp-queue/reply(chunk limit)·typed terminal은 기존 로직 재사용.
- **DoD**: wire로 온 28이 실제로 admit되는 신규 in-process/loopback 테스트 + mismatched-fence가
  typed terminal. 전체 dotnet 게이트 무회귀(sanctioned 3건만). sol CLEAN.
- **선행**: 없음(clean subset 커밋 `ab26a6527b` 기반).

---

# 단계 2 — canonical 4언어 수신·발신 활성화

## 2a. [A3] .NET canonical 28 발신

**근거 스펙**: 51 §9(28 body layout) · 15 §4.2. **성격 = 포맷 발신만.**

- **단계**: capability + observed-authority 게이트가 충족되면 사설 JSON 대신 **canonical 28 binary를
  encode해 발신**하고, 그 뒤 **기존 relocation prepare/data/cutover/CAS 흐름을 그대로 구동**한다.
  transfer/handoff/completion id는 language-internal(§9 — wire 무배치). capability 미충족 시 JSON 폴백.
  relocation 로직·completion 처리는 재사용(새로 만들지 않음).
- **DoD**: 게이트 아래 canonical 28 발신, 기존 흐름 무변경 통과, .NET↔.NET 왕복 + 전체 게이트 무회귀, sol CLEAN.
- **선행**: 단계 1(수신자).

## 2b. [A2] C++ canonical 28 수신자 완성 (H-12) + chunk limit (H-14)

**근거 스펙**: 51 §9 · 15 §4.2 · 21 §2.4 · 28.

- **성격 = 포맷 decode 배선만**(.NET A1과 동형): canonical 28을 decode해 **기존 cpp admission
  (`admit_wire_actor_join`)에 투입**. 포맷상 유일 차이 = **Store Authority row에서 stable type 해석**
  (`actor_type_from_authority` 연결, sync local-map→async Store). chunk limit(H-14)은 admission이
  reply로 협상하는 기존 값(negotiated) 그대로. seal/capture/Restore/cutover 등 relocation continuation은
  **기존 로직 재사용**(새로 배선·검증 추가 금지 — spec 28 §2 단일책임).
- **DoD**: cpp가 canonical 28을 decode해 기존 admission 투입·chunk limit 협상, focused + 관련 e2e 무회귀, sol CLEAN.
- **선행**: 단계 1(포맷 배선 패턴 참조). A1에서 검증된 "포맷만" 패턴을 cpp에 반복.

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

- **성격**: attempt-lifecycle·bound Session은 **기존 relocation 로직이 이미 처리**한다. canonical은
  포맷만 바꾸므로 새 바인딩·supersession 로직을 만들 필요가 없다. 이 카드는 **기존 동작이 canonical
  28 포맷에서도 그대로 성립하는지 검증**하는 것이다.
- **단계**: (a) 기존 multi-attempt(later-attempt-wins 재park)·bound Session 동작을 canonical 28
  경로에서 재확인, (b) 발산 발견 시에만 최소 수정(로직 재작성 아님). 실 gap이면 STOP·판정.
- **bound Session 스펙 순서(28 §4.1, 51 §9)**: ① target 준비 확인(=28 admission)이 **먼저**,
  ② bound면 **source dispatch 중단(capture 시작) 전에** 42로 seal(43은 결과 통지), ③ 44 route는
  **commit 후**, ④ 28 frame에 bound-session sideband 금지(seal/route는 42/43/44로만 이동).
  즉 순서는 **28 → 42/43 seal → dispatch 중단 → … → commit → 44**이다. (주의: "42/43 seal이 28보다
  먼저"가 **아니다** — seal의 기준점은 command 28이 아니라 source dispatch 중단이다.)
- **판정 기록 1 (.NET Property 2 = PASS)**: 에이전트가 "28이 seal보다 먼저 = 발산"으로 STOP했으나
  **오독**. `ZLinkActorRemoteJoiner.cs`에서 28 admission(:475) → `SealBoundSessionRouteAsync`(:651) →
  `BeginHandoffCaptureAsync`/`markSourceCaptureStarted`(:674/:676) 순서. .NET의 실제 ingress-hold는
  `BeginHandoffCaptureAsync`→`Handoff.BeginCapture()`(ZLinkActorRuntimeState.cs:1650, dispatch mailbox
  lock 안)로 seal **후**. → **seal-before-hold, 스펙 준수**.
- **판정 기록 2 (canonical parity = A5 DoD 충족)**: bound-Session seal/hold 순서는 JSON·canonical
  **공유 파이프라인**(Node `prepareMaintenanceSession`, .NET `SealBoundSessionRouteAsync`+`BeginHandoffCapture`)에
  있어 두 경로가 구조적으로 동일 → **canonical이 기존 동작 유지(parity) = A5 핵심 DoD 충족**. 순서 스펙 논쟁과
  무관하게 canonical은 회귀 없음.
- **별도 기존 이슈 A (Property 2 seal-순서 크로스랭 불일치 · 수정 금지 · 유저 판정)**: 4언어가 **2:2 분포** —
  **Node·Java = hold-before-seal**(Node `actorHandoff.begin` actor-transfer-runtime.ts:403→seal:713; Java
  `queue.trySealRelocation` ZLinkStandaloneActorRelocationSourceBuilder.java:326→seal:644),
  **.NET·C++ = seal-before-hold**(.NET seal:651→`BeginHandoffCaptureAsync`:674; C++ `seal_bound_sessions`
  mesh_node_runtime.cpp:2907→`try_begin_source_remote` spot_runtime.cpp:6172). 51 §9:507/28 §4.1:87은
  "seal은 dispatch 중단 전"을 요구 → hold-before-seal 두 언어는 문자적으로 어긋나나 **source backlog로 메시지
  손실 없음**. **모두 JSON·canonical 공유 파이프라인 = canonical 회귀 아님·A5 비차단**. relocation 동결 원칙상
  여기서 수정 안 함(유저 판정용 기존 이슈).
- **[ ] A5-canonical-multiattempt (열린 설계 질문 · 확정 결함 아님 · 3b서 실증 해소 · A5 비차단)**:
  정정 — 이전 "Node 확정 결함·최소 수정" 판단은 **틀렸다**. 사실관계:
  - Node canonical 수신자는 later-attempt-wins prewarm registry(`formalRemoteActorAdmissions`)에 등록 안 함
    (`begin()` 게이트가 사설 transferRequest 의존, spots/index.ts:1942). **그러나** canonical admission은 actor를
    **즉시 materialize**(`prepareCanonicalActorJoin`→`getOrCreateActor`+`setNativeActorRef`,
    remote-actor-join-receiver.ts:145/163, **기존 코드**). production ingress park(`routeIngress`)는
    actor 해소 **실패 후에만**(handleMissingActor) 도달하므로, actor가 이미 가시화된 canonical 경로에선
    parking-registry 모델이 **구조상 도달 불가**일 수 있다. → later-attempt-wins가 실제 깨졌는지, 아니면
    eager-materialize+authority CAS로 처리되는지는 **미해결 설계 질문**(grep이 아닌 설계 분석 필요).
  - **시도한 fix(canonical을 prewarm registry로 라우팅, lifecycleAliases)를 revert함**: sol 리뷰가 전제
    무효(actor 가시화로 parking 미도달) + alias exact-attempt identity 결함 + `canonical:<operationId.low>`
    attempt간 미유일(process-local 카운터, source RID/generation 무시)의 2 Critical + 1 High를 발견.
  - **test-shape 경고**: 추가 테스트가 resolver를 stub하고 registry를 직접 구동해 94/94 통과했으나 두 Critical을
    **놓침**. 향후 테스트는 **실제 ingress 경로**를 통과해야 한다.
  - **크로스랭 baseline caveat**: .NET/Java/C++의 "PASS"는 **static-read**일 뿐 canonical-28 multi-attempt
    통합테스트가 4언어 모두 부재. 게다가 **.NET도 canonical prepare에서 state를 eager 생성**
    (ZLinkFrameworkRuntimeActors.cs:2869 `GetOrCreateActorState`) → "Node만 outlier"는 확정 아님. 3언어의
    prewarm 등록이 vestigial인지 실제 동작인지도 미검증.
  - **해소 위치 = 3b**: pairwise 스테이지(Java→Node, .NET→Node 등)가 실제 두 번째 attempt를 **실제 ingress로**
    구동 → static read가 못 주는 실증 증거 확보. 이 항목을 3b에서 열어 판정(수정은 설계 확정 후).
- **DoD 결과**: bound Session(Property 2) = canonical parity 확인(공유 파이프라인, 회귀 없음) **PASS**.
  multi-attempt(Property 1) = .NET/Java/C++ static PASS, **Node는 열린 설계 질문**(위, 3b서 실증 해소).
  canonical이 기존 동작을 **회귀시키지 않음**은 확인됨(핵심 DoD 충족). multi-attempt 정확성 설계 판정만 3b로 이월.
- **선행**: 단계 1·2a.

## 3b. [A6] 크로스랭 harness canonical 매트릭스 (S4e)

**근거 스펙**: 51 §9 · 15 §4.2.

- **단계**: User Spot JoinSpot stage를 canonical 28 pairwise 매트릭스(Node↔.NET↔Java↔C++)로,
  JoinEntrySpot 의미 보존. harness 기본 `all`에 편입(무음 편입 금지, flake는 명시 스킵).
- **DoD**: 4언어 pairwise canonical 28 결정적 그린, 매트릭스 편입.
- **선행**: 단계 1·2(4언어 수신·발신 활성화).
- **harness recon(착수 전 읽기)**: 언어별 `framework/languages/<lang>/cross-language/run_cross_language_smoke.sh`
  (cpp 1187줄)에 스테이지 함수 = 1 producer/consumer 방향. host 프로그램: cpp `cross_language_host.cpp`
  (target `zlink_cpp_cross_language_host`), node `node_peer_host.js`, dotnet `Zlink.Framework.TestHost`,
  java host. 기존 `relocation*` 스테이지는 **JoinEntrySpot**(User-Spot mode·`all` 미편입). redis 페어링,
  ready/stop 파일 협약. 스테이지 선택: env `ZLINK_CPP_CROSS_LANGUAGE_STAGE`.
- **호스트 mode 현황(recon 확정)**: 기존 relocation 스테이지는 **Node/.NET/Java·JoinEntrySpot**(`relocation`,
  `relocation-{java-dotnet,dotnet-java,node-dotnet}`, cpp/run:47-50; cpp/run:932~ "entry-spot relocation").
  **C++ host(cross_language_host.cpp)는 actor-join/relocation mode 자체가 없음**(modes: stream/channel/
  spot-route만, :799~). → **C++ 관여 방향은 완전 새 mode 신설**(가장 큰 작업, 마지막).
- **실행 계획(항목별 4언어 lockstep, 최소 증분 순서)**:
  1. **첫 스테이지 = Node↔.NET User-Spot JoinSpot canonical 28**(기존 relocation 인프라 최다 재사용 → 최소 신규).
     양 host에 **User-Spot actor-join mode 신설**(현재 entry-spot 의미만). 한 방향부터 결정적 그린.
  2. **A5 multi-attempt 실증**: 그 스테이지에 **같은 actor 두 번째 canonical attempt** 추가 → later-attempt-wins
     (첫 시도 supersede/re-park)를 **실제 ingress**로 관측 → §3a 열린 설계 질문 판정(eager-materialize vs
     parking 실제 동작). 결함이면 STOP·설계 판정(즉석 admission 수정 금지).
  3. **Java 방향 추가** → 4. **C++ 방향(신규 mode 신설, 최대)** → 5. **12방향 완성**(양방향).
  6. **`all` 편입**: 통과 스테이지만(무음 금지, flake는 명시 스킵).
- **주의**: 대형·독립 세션 규모. host User-Spot mode 신설(특히 cpp 신규 mode)은 고난도 → sol. 각 스테이지 그린을
  Claude가 독립 재현. multi-attempt 결함 시 [[canonical-multiattempt-design-trap]] 원칙(즉석 수정 금지) 준수.

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
