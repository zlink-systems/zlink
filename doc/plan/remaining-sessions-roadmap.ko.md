# Relocation 캠페인 — 남은 작업 세션별 로드맵 (작업 순서)

작성 2026-08-21. 이 문서는 기존 통합 체크리스트(1178줄, 이력·커밋해시 포함 —
`doc/plan/archive/relocation-campaign-checklist.ko.md`로 아카이브)의 **미결 항목만** 뽑아,
**독립 세션에서 순차적으로 집어 진행할 수 있도록 작업 순서(단계 1→10)대로** 재구성한 것이다.
이 문서가 앞으로의 활성 트래커다. 각 단계 카드는 목표·문제·구체 단계·완료조건(DoD)·선행조건·
근거 스펙을 담는다. (본문의 "체크리스트 line/항목 …" 참조는 아카이브된 그 문서를 가리킨다.)

## ⭐ 세션 핸드오프 (2026-08-21 마감 — 새 세션 여기부터)

**이번 세션 완료·push(main)**
- `dd234c3110` **A6: cpp canonical actorJoin app-reply 양방향 parity**(수신 wrap + 발신 unwrap + serializer 가드) —
  sol 2R 구현+2R 리뷰+Claude 독립검증(ctest 5/5·TicTacToe/Bingo). **완료.**
- `42ed2d0cc1`·`68bce060d4` **A5 검증**: Property 2(bound-Session seal 순서)=canonical parity PASS(공유 파이프라인·회귀
  없음; 크로스랭 2:2 seal-순서 불일치는 기존 이슈로 기록). Property 1(multi-attempt)=3언어 static PASS, **Node는 열린
  설계 질문**(eager-materialize와 얽힘 — 시도 fix revert, sol 2Critical+1High). §3a·[[canonical-multiattempt-design-trap]].
- `e96334f07e`+`7c7a67727b` 등 **3b 첫 스테이지 신설**(`user-spot-join-node-dotnet`, `all` 미포함): Node source
  `joinSpot`→.NET User-Spot canonical 28. canonical-only observable(Flow) 작동(양성/음성 검증됨). **아직 그린 아님.**

**⚠ 이번 세션의 판단 오류(반복 금지)**: 3b 실패를 "authority row 포맷 gap"으로 부풀렸다 정정했다 다시 뒤집는 등
플립플롭했다. 최종 grounded 결론은 아래. 3b 실패를 다룰 때 **§0(포맷-온리·로직/검증 추가 금지)**를 먼저 읽을 것.

**3b 첫 스테이지 blocker — grounded 진단(diag 에이전트, run-dir 증거)**
- 실패 지점 = `AdmitCanonicalActorJoinAsync`의 source-authority fence 중 **`TryDecodeRelocating == false`**
  (ZLinkFrameworkRuntimeActors.cs:2397-2405, ZLinkActorAuthorityPayloadCodec.cs:298/320). row는 **Found**,
  objectGeneration/actorNodeRid/ownerNodeGeneration **일치**, decoder만 실패.
- 원인: **Node가 actor Authority row payload를 JSON**(`{"version":1,"actorType":...}`,
  actor-authority-publication.ts:31/113 `encodeActorAuthorityIdentity`)으로 publish. .NET `TryDecodeRelocating`은
  relocation envelope를 벗긴 뒤 **direct codec이 첫 4바이트 `ZLAU`**를 요구 → false.
- **retraction의 전제가 틀렸음 주의**: "통과하는 whole-node relocation이 authority interop을 증명한다"는 **틀림** —
  그 경로는 `relocate()`+commit 시 authority rewrite라 **`AdmitCanonicalActorJoinAsync`/`TryDecodeRelocating`을 안 거침**
  (별도 경로, node_peer_host.js:493, actor-transfer-runtime.ts:1233). 즉 canonical 28 source-admission의 authority-read는
  이 스테이지가 **처음** 크로스랭으로 실행.
- private baseline 무효: force-private로도 admission 미도달 — .NET target이 noncanonical multipart를 admission **전에**
  ProtocolError로 거부(ZLinkManagedMeshNode.cs:5263).

**다음 세션 착수점(§0 포맷-온리 틀에서만; 로직/검증 추가 금지)**
1. **[x] 정본 판정 완료(2026-08-21, Claude 단독)**: 정본 = **ZLAU binary `authority-payload-v1`**.
   근거: `service-wire-v1.schema.json` **durableFormats**가 `authority-payload-v1`을 magic "ZLAU"·
   formatVersion 1·big-endian·u32 body length·trailing CRC-32C(Castagnoli)로 규정하고, golden fixture
   `golden/durable-authority-v1.json`이 `"consumers":["cpp","dotnet","jvm","node"]`로 4언어 소비를 명시.
   .NET `ZLinkActorAuthorityPayloadCodec`은 이 durable format과 정확히 일치(기준 구현), Java도 ZLAU 일치.
   spec 21 §2.4의 "payload=opaque"는 **Store/provider 관점**(schema `providerInterpretation:"opaque-bytes"`와
   정합)이며 포맷 부재가 아니므로 스펙 충돌 없음(스펙 수정 불요). → **Node(JSON)·C++(text `v1`)가 비정합**.
   Node를 지금 정렬하고, C++는 3b C++ 방향 착수 시 정렬. 참고: 어떤 언어도 durable-authority golden
   conformance test를 실행하지 않고 있었음(=drift 원인; Node 정렬에 conformance test 포함).
   .NET `TryDecodeRelocating`은 ZLAP/ZLAR envelope를 optional로 벗기므로 bare ZLAU도 fence 통과.
2. **최소 포맷 정렬**: 정본에 맞춰 Node의 actor Authority publication이 canonical representation을 쓰도록(같은
   generation/RID/lifecycle 값 유지). `joinSpot` 자체는 packet-format 선택 경로로 그대로 둔다. **admission 로직 무변경.**
   하니스로 row를 수동 덮어쓰는 우회는 금지(실제 Node-created actor를 검증 못 함).
3. 그 후 stage-1 재실행 그린 → §3a multi-attempt 실증(test-only 겹침 송신) → Java·C++ 방향 → 12방향 → `all` 편입.
4. 관련 메모리: [[actor-authority-row-format-gap]](정정판)·[[canonical-multiattempt-design-trap]]·
   [[canonical-actor-join-app-reply-contract]]. 하니스 스테이지는 보존됨(selector `user-spot-join-node-dotnet`).

**그 외 남은 단계**: 3c(A7 dialect 제거), 4(C1 코덱 스왑), 5(B0 안정화), 6(D2 6샘플), 7(Z1 ZoneWorld),
8(D3/D4 게이트), 9(모든 e2e — 맨 마지막), 10(D5 sign-off). 상세는 아래 각 카드.

---

## 진행 방식 전환 (2026-08-22, 사용자 지시 — 직렬 발견 → 감사-후-배치)

지금까지 canonical blocker를 "스테이지 실행→timeout→진단→단건 수정"으로 **직렬 발견**했으나,
드러난 blocker가 전부 한 패턴(스펙이 정의한 경계의 언어별 사설 표현 잔존: ZLAJ·JSON-ZLAP·
ZLRA3·one-way 전송·applicationVersion 오용·eager materialize)이므로, **4언어 병렬 스펙-정합
감사(읽기 전용, 확정 ruling 기준표 포함)로 delta를 전수 나열 → Claude가 병합·판정 → 언어별
배치 수정 → 스테이지 일괄 디버깅** 방식으로 전환한다. 감사 기준·표면 정의는
스크래치 `audit-common.md`(세션 산출물, 요지: 전송 종별·payload/envelope 포맷·admission
provisional 모델·발신 게이트·사설 잔존물 5개 표면 × 확정 ruling 8건). 개별 수정의 검증·리뷰·
파일명시 커밋 규율은 그대로 유지한다.

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
  - **사다리형(다단 blocker) 작업의 커밋 단위**: 최종 목표(예: 스테이지 그린)가 여러 blocker
    사다리로 쪼개질 때, 전체가 그린이 될 때까지 diff를 키우지 말고 **자체 검증+리뷰가 끝난
    중간 단(rung)도 트리-로컬 게이트 그린이면 개별 커밋**한다(커밋 메시지에 "스테이지는 아직
    red, N차 blocker 해소" 명시). 2026-08-22 stage-1에서 6단 사다리 diff가 30여 파일로 커진
    교훈.
  - **병렬 스트림 합류**: 언어 트리별 병렬 작업의 완료분은 한 덩어리로 모으지 말고 **스트림별
    검증+리뷰 통과 즉시 파일 명시 add로 개별 커밋**한다(다른 스트림 미완이 커밋을 막지 않게).
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
  - [x] 3a [A5] attempt-lifecycle / bound Session(S4d·S4d-b) `혼합` — **완료(2026-08-22)**: Property 2(bound
    Session) canonical parity PASS + Property 1(multi-attempt) **실제 ingress 실증으로 판정 완료**
    (`327c2b86c1` probe + sol 정합 분석): later-attempt-wins는 admission registry 계층서 동작,
    Node/.NET 대칭. 잔여 스펙-gap 3건(typed terminal 21·.NET post-PREPARE abort·Node eager-materialize
    정리)은 별도 semantic 카드(§3a 판정 기록). 상세 아래 §3a.
  - [x] **A6-cpp-app-reply-parity (양방향 완료)** — `dd234c3110` (`cpp단일`, 포맷만): cpp canonical actorJoin app-reply를 node/java/.NET과 양방향 parity로 정렬. sol 2라운드 구현 + sol 2라운드 리뷰 + Claude 독립 검증(diff·클린 재빌드·ctest 5/5·TicTacToe/Bingo).
    - [x] **수신자(send)**: `actor_join_operation_result_t.application_reply` 추가 + `admit_wire_actor_join`가 handler reply를 framework-multipart(`[u32BE count=1][u32BE size][bytes]`) application_payload로 감싸 command-20 tail 뒤 2번째 frame으로 전송; `terminal_result!=0`이면 payload 거부. **.NET `EncodeFrameworkMultipart`/Java `encodeFrameworkMultipart`와 바이트 단위 일치**(BE count+per-part len).
    - [x] **발신자(receive) 언랩**: canonical source가 framework-multipart를 언랩(`unwrap_canonical_actor_join_application_reply`, 첫 파트 반환·non-multipart는 그대로·malformed는 protocol_error)해 handler 실제 reply message를 join caller에 전달. **JSON 경로(unwrapped 저장)·Java `decodeFrameworkMultipart`와 일치**. malformed reply는 negotiation chunk-limit 기록 **전에** 실패(순서 재배치).
    - [x] **serializer 가드 축소**: `canonical_actor_join_application_reply` → `result_t<optional>`; reply 없으면 serializers 없이도 admit, reply 있는데 serializers null이면 typed protocol_error. 기존 동작 회귀 없음.
    - 이월(3b서 자연 해소): ① full source-path end-to-end 커버(Java→cpp/.NET→cpp pairwise가 정확히 구동) ② throwing reply serializer 시 pending-admission unwind(pathological) ③ **.NET source(ZLinkActorRemoteJoiner:1044) non-unwrap 의심 → pairwise서 확인**.
  - [~] 3b [A6] 크로스랭 canonical 매트릭스 `4언어` — **진행 중(6/12 그린: node↔dotnet,
    node→java, java→dotnet, cpp→dotnet, node→cpp)**. **node→cpp 그린 확정(2026-08-22)**: 최종
    blocker는 코드가 아닌 **stale C++ host 바이너리**(capacity-row 수정이 host 재빌드 이후 랜딩).
    재빌드 후 28→40→52→34→authority+capacity 원자 CAS→OnJoined→probe 전 구간 Redis MONITOR로
    완주 실증, 코디네이터 직접 재검증 passed(cpp→dotnet 무회귀). 하니스 개선 이월: host 존재만
    검사하고 freshness 미검사(run_cross_language_smoke.sh:47) — 셀 실행 전 증분 재빌드 강제 권장.
    **잔여 6 selector 작성 완료 `ec6b721205`** — cpp→node·cpp→java 즉시 그린 → **8/12**. 실패 4셀
    (run-dir 보존): ①② dotnet→java·dotnet→cpp — **진단 확정(공통 단일 원인)**: .NET source **이중
    decode** — mesh 계층(CompleteNativeActorJoinRequest)이 multipart 언랩 후 sole raw part 전달,
    ZLinkActorRemoteJoiner:1044이 이를 다시 envelope decode → 계약 준수 raw reply(Java/C++)에서
    40 전송 전 사망. Node empty-reply는 decode 우회로 그린, .NET target은 envelope 중첩 송신이라
    우연 통과(중첩 송신 정리는 3c 카드). Ruling: single-decode+호환 분기(envelope면 1회 언랩) +
    content type 전달 + 3형태 focused test — terra 구현 중(하니스 검증은 코디네이터 직렬) ③④ java→node·java→cpp — **진단 확정(공통
    단일 원인, identity 가설 기각)**: Java decode 불변식 `SERVER ⇔ entrySpotId`
    (ZLinkMeshNodeDescriptor.java:74)가 Node/C++의 정당한 Server+entry없음 Store row를 거부해
    Java startup 자체가 사망("Only an Object Server descriptor must publish entrySpotId"). Ruling:
    단방향 완화 **랜딩 `9b9a50cacb`**(startup 사망 해소, java→dotnet 3/3·node→java 2/2 회귀 그린).
    새 rung: java→node JoinSpot dispatch handler_exception(INTERNAL_FAILURE, Node측 'prepare
    target fence does not match the target owner')·java→cpp(PROTOCOL_ERROR) — terra 진단 NOT-CONVERGED:
    공통 취약 seam 확정 — Java source가 40 target fence를 승인 fence가 아닌 **재조회 descriptor로
    재구성**(ownerId/appVersion 무대조, SourceBuilder.java:498/:651, StateMachine:1053). **계측 수렴(2026-08-22)**: ⓐ java→node =
    40 applicationVersion 불일치 — envelope 인코더 writer.u64(1) 하드코딩
    (ZLinkCanonicalActorRelocationEnvelope.java:126) vs 정본 target.applicationVersion()=0.
    ⓑ java→cpp = targetAttemptGeneration — 상수 new Fence(relocationId,1)(SourceBuilder:661) vs
    cpp 기대 target lifecycleGeneration(spot_runtime.cpp:5767, 16 conjunct 중 유일 실패). Ruling
    2건 확정 → **수정 랜딩 `f04506d1ff`: java→node 그린(9/12)**, Java 게이트 198 XML 그린.
    java→cpp는 targetAttempt 수정 후에도 PROTOCOL_ERROR 잔존(/tmp/tmp.JAfGly9W8Y — 다음 conjunct
    계측 필요, 하니스 직렬 대기). 회귀 java→dotnet·node→java 코디네이터 직접 실행 그린(무회귀 확인).
    **dotnet 셀 다음 불일치 계측 수렴**: coordinator fence는 이제 wire 정상(decode 성공) — 최초
    불일치는 ZLJR ReplyContentType 의미 혼선(.NET이 inner `application/json` 기록
    ManagedMeshNode:2262→:9557→RemoteJoiner:741, Java는 outer `application/x-zlink-multipart` 요구
    Adapter:363 → DATA_LOST). Ruling: .NET completion서 inner/outer 분리, ZLJR엔 outer — **랜딩
    `ad07fbdaa7`**(RecoveryReplyContentType 신설, DATA_LOST 소멸 실증, dotnet→node·node→dotnet
    회귀 그린). **3라운드 계측 수렴**: ⓐ dotnet→java =
    Java ZLAU codec signed 양수 검사(PayloadCodec:262)가 .NET high-bit nodeGeneration(u64) 거부
    → authority invalid → 40 FAILED + **failure 경로 이중 결함**(pre-stage cleanup
    requireActorStage 동기 예외로 FAILED reply 미전송 → silent, StateMachine:474/:652). Ruling:
    opaque token용 unsigned-nonzero reader 분리 + failure reply 보장 — 랜딩 `0a9fc8bd52`(silent
    hang 소멸). diag4: currentSpotGeneration도 동일 계열 → opaque 처리 랜딩 `534154d595`(ZLAU 거부 소진,
    1098 테스트 그린). cpp attempt conjunct 수정 랜딩 `8ae6886892`(회귀 4셀 그린). **diag5 수렴 — dotnet 2셀 공통
    근본 원인**: .NET precommit이 40 전 source authority slot을 phase=2·attempt=0(빈 fence)로
    기록(schema `ordinal-or-zero` 적합, PrecommitCoordinator:70) — Java decoder가 0 거부
    (RelocationAuthorityStateCodec:128→PayloadCodec:120 slot-비어야함 실패), cpp도 동일
    (actor_authority_payload.hpp:618). **40/52/ZLJR 무결 — Java/C++ decoder의 schema 비준수.**
    **[전수 감사 — 스펙 외 요소] 진행 중(2026-08-22)**: 4언어 병렬 감사 중 Node(19건)·.NET(14건)
    도착, Java·C++ 대기. **최대 발견 = 28 app-reply 프레이밍 4자 발산(상호 모순, 스펙 무규정)**:
    cpp/Java/.NET target은 frame-2를 framework-multipart로 랩, Node target은 inner 타입 직접 —
    Node ZLJR 검증(inner 일치, spots/index.ts:2820)과 Java claimRecovery(multipart 리터럴,
    Adapter:363)는 동시 만족 불가. .NET source의 multipart 필수 conjunct(ManagedMeshNode:9529)도
    스펙 근거 없음(이 탓에 dotnet→node non-empty reply 소비 불가 — 현재 empty reply라 잠복).
    → 4런타임 공통 프레이밍 규칙 스펙 명문화 + 정렬이 유일 해소(통합 판정은 Java·C++ 감사 후).
    **Java 감사 도착(A8건+B11+C12)**: ⭐A1 Java만 ZLJR을 saved-work 선두(order=1)+전체 +1 시프트
    (.NET/C++은 꼬리 append — wire 발산, java→cpp 잔존 PROTOCOL_ERROR 유력 용의) ⭐A6
    objectCapabilities 불변식(entrySpotId 동형 가짜 blocker 잔존, Descriptor:111) ⭐A3 `+1` 정확
    일치 4런타임 공통(스펙 21:913은 `>`) — 전언어 ruling 대기 ⭐A4 replayCursor(기존 OPEN RULING)
    + Java 내부 코덱 자기모순 ⭐A7/A8 스펙 명시 위반 재조회·재검증. 상세: 태스크 보고 +
    scratchpad/java-audit-notes.md.
    보고서: scratchpad audit-extraspec-{node,cpp}-sol.log, .NET/Java는 Agent 태스크. 기타 주요:
    .NET ZLAP/ZLAR 이중 래퍼(schema trailingBytes forbidden 위반 의심·LE byte-order), tautological
    admission conjunct(RemoteJoiner:569), 순서 비교 잔존(:581 — 51 §9 위반), one-way 40 legacy
    분기 제거 조건 충족(ManagedMeshNode:6214), 메시 전체 조회(:756 성능); Node Store 재조회
    다수(4180 등)·Relocation Store 과잉 필수(actor-transfer-runtime.ts:424).
    **[정책] 리뷰·수정 감사(2026-08-22 사용자 지시)**: sol 리뷰는 큰 단계 마감에만, 일상 리뷰는
    코디네이터 직접. 랜딩된 수정이 스펙 외 단계 추가·복잡화·성능 저하를 만들면 보고 후 revert.
    감사 결과: Java attempt=lifecycle(f04506d1ff 절반)은 제거된 cpp 스펙위반 검사에 맞춘 것 —
    상수 1로 revert 진행. 3c revert-target: .NET nested-envelope 호환 분기 + RecoveryReplyContentType
    단순화 재검토.
    Ruling: 두 언어가 frozen authority-relocation-state대로 source-phase slot 수용 — **랜딩
    `6a772686d8` → dotnet→java 그린(10/12,** stale Java host 재빌드 후 — installDist도 freshness
    함정 목록에 추가). dotnet→cpp 최종 rung 수렴(sol): ZLJR consume 한
    지점에 identity 불일치 2건 — ⓐ .NET이 ZLJR coordinator를 precommit 전 스냅샷으로 조립
    (RemoteJoiner:631)해 40의 Capture-후 snapshot(:853)과 StoreVersion 불일치 → cpp exact
    equality(:5819) 실패 ⓑ cpp가 28 correlation과 ZLJR public completion operation을 동일 비교
    (:5851) — 계약상 분리된 identity(스펙 외 동일시). 에이전트 STOP·에스컬레이션으로 판정 전환(V0
    단일 값 정본) → **랜딩 `c6e7323872` + cpp 동일시 제거 → dotnet→cpp 그린(11/12)**. A1(Java
    ZLJR 선두삽입→꼬리 append 정렬) 랜딩 `5055950d7d` → java→cpp가 PROTOCOL_ERROR에서 admission
    통과 후 silent로 전진(잔존 용의 = cpp 감사 #19 스펙 외 등식 — 제거 terra 진행 중). **단
    dotnet→java가 V0 전환으로 red 회귀**(Java target의 coordinator 기대가 cpp와 상호 모순 의심 —
    스펙 인용 판정 포함 sol 진단 중, dotnet→node는 그린 유지). 현재 10/12+1 요동. 이월
    카드: ① Java의 .NET slot 완전 semantic decode는 여전히 불가(field-12+ 레이아웃 발산 —
    strip은 경계 기반이라 무영향, 별도 판정 대상) ② cpp
    test_cpp_framework_actor_authority_payload의 user-spot hex assertion **HEAD 기존 red**(베이스
    라인 대조 확인 — f29a4c69d8 이후 encoder/literal 불일치, 재핀 또는 encoder 결함 분석 필요). 스펙 구체화는
    sol 리뷰 승인(앵커 보완 `4a39e14dca`). ⓑ
    dotnet→cpp = .NET TargetAttemptGeneration=1(:967) vs cpp validator lifecycle 동일성 요구
    (spot_runtime.cpp:5764). **설계 판정 완료(opus 분석→Claude ruling, 후보 A)**:
    attempt = RelocationId 내 시도별 유일 nonzero 불투명 값, 정확 equality 전용(결정 근거 21
    §7.1 — lifecycle 유도값은 같은 target 재시도 구분 불가; later-attempt-wins는 admission
    registry 담당이라 ordinality 불요). cpp 초과 제약 2곳만 수정(spot_runtime.cpp:5766-5768
    conjunct 삭제, :5838-5839 비교대상 target_node_generation으로), source 3언어 무변경. 스펙
    모호성 구체화 `d89aaad84d`(28:73·21 §7.1 ko/en, sol 리뷰 중). sonnet cpp 수정+5스테이지 검증
    중. **부수 발견 카드**: ① Java command 33이 attempt 슬롯에 authority owner generation+1 오용
    (RetireTargetEndpoint:719-721↔ReplyRoutes:337-339 자기정합 — 52:286 위반, 잠복) ② java→cpp
    PROTOCOL_ERROR는 attempt 원인 아님(별도 conjunct — 미진단) ③ 재시도 카운터 도입 시 4언어
    source 증가값 발행(향후). java→dotnet 그린은 .NET ValidatePrepare(:2846) 관대함
    때문(정합 증거 아님). .NET coordinator fence 수정(1264653381)은 dotnet-java 재실행서 여전히
    joined 미관측 — 다음 불일치 여부 sol 연속 계측 진단 중.
    .NET 이중 decode 수정 랜딩 `adfa26824b` 후 새 rung **sol 계측으로 수렴**: unbound join에서
    .NET source가 ZLJR coordinator fence를 빈값/gen0으로 송신(RemoteJoiner:625 hasBoundSession
    조건 안에서만 채움, Packets:214) → Java decoder 필수 요구(RecoveryCodec:287)로
    DATA_LOST→40 FAILED(RelocationDataLost)→34 미도달(실측 /tmp/tmp.TVjNIHjKI7). Ruling: fence
    생성을 조건 밖으로 분리, unbound에서도 source authority로 채움 — sonnet 구현 중(unit만,
    하니스는 코디네이터 직렬). java fence 실패 필드/conjunct는 sol 계측 재실행 진행 중. 러너 부수 변경: Java/C++ target
    peer-rid 선택값화(.NET 자동 RID 지원), Node target 실 probe handler. 이하 이전 기록: **역방향 .NET→Node 그린** `00dbdfd054`(Node provisional admission + journal
    canonical slot 모델 — TTT 회귀 모드 A 0/8 소거 동반). **4언어 배치 랜딩**: Java
    `6bb05dce85`(flags 판별·eviction race·typed 21), .NET `63e551c2b4`(47/48/49 request/reply·
    raw-20 차단·typed 21), C++ `2032cb6ba5`(**원격 Join을 인라인에서 ZLJR+40/52 canonical chunk로
    전환** — 3b C++ 방향의 핵심 전제 완성, ZLJR metadata ordered-json으로 Node 고정 벡터 byte-핀).
    **Java 2셀 그린 `fef06c1e4a`** — Java ZLJR producer/consumer 신설(4언어 공통 Node hex 벡터로
    byte-핀 완성) + admission의 Store-resolved actorType(51 §9) 수정. **매트릭스 4/12: C++ host가
    필요 없는 방향 전부 그린**(node↔dotnet, node→java, java→dotnet + 기존 relocation 스테이지
    무회귀). 남은 큰 조각 = C++ cross-language host의 actor-join mode 신설(런타임 쪽 chunk 경로는
    `2032cb6ba5`로 완비 — host 하니스만 남음) → C++ 관여 6방향 → 나머지 2방향(dotnet→java 등
    User-Spot 역조합) → `all` 편입. 이하 이전 기록:
    **완료(2026-08-22)**: ① **stage-1 Node→.NET 결정적 그린** `de17ac7179`(6단 사다리 — ⭐핸드오프·
    §3b 사다리 기록 참조) ② multi-attempt 실증 스테이지 `327c2b86c1` ③ cpp app-reply 양방향
    (`dd234c3110`) ④ .NET app-reply 보존 ⑤ 4언어 durable-authority conformance 대칭
    (`077aa29987`·`7dd0813b96`·`ea7805d54b`·`387ee43361` — cpp는 ZLAU 정렬 포함, drift 오라클 확보).
    **진행 중**: 역방향 .NET→Node 스테이지(`user-spot-join-dotnet-node`) — 구현 완료(양 host mode+
    selector), 첫 실행서 2건 발견·수정 중: ⓐ 하니스 gap(Node target에 RouteMesh Entry Spot 미등록 →
    'authority requires its RouteMesh Entry Spot' 거부) ⓑ **[판정] canonical 28 전송 계약 ruling**
    (2026-08-22, Claude 단독, spec 51 근거 — 20='request terminal result'(:180), 28='요청 body'(:467),
    admission reply tail(:571)): **28은 Core request로 발신, 20은 그 reply leg** 가 정본. Node 발신자·
    .NET 수신자는 적합, **.NET 발신자가 비적합**(one-way routed 28 + 독립 20 수신,
    ZLinkActorRemoteJoiner.cs:453) → request 채널로 정렬(sol). Node 수신자는 unsequenced 28에서
    replyService 예외로 terminal 유실 → .NET 동형 ProtocolError-drop 방어 추가(terra). 참고:
    .NET↔.NET canonical은 하니스 스테이지가 없어 이 비대칭이 지금까지 미검출 — 정렬 후 in-process
    왕복 테스트로 보강. **남은 것**:
    Java 방향(host User-Spot mode 신설) → C++ 방향(host actor-join mode 완전 신설, 최대) →
    12방향 완성 → 통과 스테이지 `all` 편입(무음 금지). 하니스:
    framework/languages/cpp/cross-language/run_cross_language_smoke.sh. 진행: 항목별 lockstep + spec-gap.
  - [ ] 3c [A7] 사설 dialect 제거(H-15/S5) `4언어`
- [ ] **단계 4 — [C1] W-3 생성 코덱 스왑(H-6)** `4언어`
- [ ] **단계 5 — [B0] 하니스 안정화** `혼합` — 3건 병렬 착수(2026-08-22, canonical과 독립이므로
  단계 6 최단경로로 선행 투입)
  - [x] H-10 dotnet→java relocation 레이스 (dotnet+java) — **판정(2026-08-22)**: 22회 반복(수정 전
    12 + 편입 후 10)에서 레이스 재현 0회 — 문서화 이후의 다수 relocation 수정으로 이미 해소된 것으로
    판정. 실제 잔여 문제는 스테이지가 기본 `all`에 호출되지 않던 것 → 명시 편입(10/10 그린, 무음
    아님·근거 run-dir 보존). 러너 diff는 Java 방향 작업과 같은 파일이라 그 커밋에 동승 예정.
  - [~] TicTacToe 간헐 flake (샘플별) — **판정 완료(2026-08-22, worktree 대조)**: 실패모드 A는
    **`de17ac7179` 회귀**(baseline 0/8 vs HEAD 4/8). 기전: ① deferred-join journal이 binary ZLAR을
    쓰게 되면서(F1 fix) relocation host가 **모든 ZLAR을 '기존 relocation root'로 오인**(journal
    판별 inventory-digest 검사는 journal 내부에만, deferred-join-accepted-journal.ts:573/:589) ②
    target finalize가 admission을 연 뒤(2617) 이전 publication root를 늦게 지우는(2632) 창에서
    다음 relocation 예약이 stale root를 읽음(3417). **수정 방향(판정)**: relocation host의 root
    독해가 journal-root를 구분·무시하도록(표현 판별 — journal 마커/digest를 reader 쪽에서 확인),
    admission-open-vs-root-clear 순서는 별도 검토. baseline의 3/8 'B-like'(105/errno2, stale 선행
    없음)는 기존 admission 결함으로 분리 등재. — Node provisional-admission 작업 완료 후 착수
    (같은 파일 소유권).
  - **[판정] relocation envelope applicationVersion 정본(2026-08-22)**: spec 21(:443 'Application
    배포 순번')·28 §4.1(target의 application version 지원 확인) — envelope/Prepare의
    `applicationVersion`은 **application 배포 순번**이다. **Node가 root envelope에
    aggregateGeneration을 기입(service-relocation-runtime.ts:143)한 것이 비적합**(node→java 셀
    red의 원인 — Java는 envelope==Prepare 동등성 검증, 적합). .NET의 미검증은 minor L1 gap(백로그).
    Node 수정을 TTT 회귀 수정과 같은 태스크로 진행(파일 인접).
  - [~] D1 ST-C4 fault-injection variant (cpp) — **STOP·판정 완료(2026-08-22)**: 계약 fault point는
    target assembly에 실존(relocation_transfer.hpp accept의 conflict + relocationDataLost=35 terminal)
    하나, cpp 원격 Join은 state를 commit request에 **인라인** 전송해 relocationState(52) chunk를
    전혀 안 씀(chunk 생산은 종료-drain relocation뿐 — 실측 kind 52 0건) → ST-C4의 판정 조항(source
    생존)과 양립 불가. **판정: cpp canonical join 방향(3b)이 랜딩되어 Join이 40/52 chunk 경로를 타게
    된 후 재작성**(e2e-only 트리거·계약 완화 기각). 제작된 ZMP chunk-corruption 프록시 seam
    (SpotActorTransfer/Support/relocation_chunk_conflict_proxy.py, 미배선)은 그때 사용. SF-F7 주석
    모순은 1런 판정법 기록됨.
  - **[ ] [신규 등재] Node TTT 모드 C 정적 진단 완료(2026-08-22, 실증 대기)**: 재접속 세션
    JoinGameNotify 미전달의 유력 기전 — relocation seal 창에서 push()가 fire-and-forget 보류 경로
    (bound-actor-relay-sender.ts:79-98)로 들어간 뒤, 브라우저 물리 재접속이
    `notifyPhysicalDisconnect→unbind→clearRelocation`(actor-session-binding-registry.ts:274-290,
    :863-872)으로 보류 outbound를 **새 바인딩 생성 전 무조건 파기**하고, `.catch(()=>undefined)`
    (:97)가 이를 무기록 은폐. 수정 방향: 동일 actor 재바인딩 시 보류 미전달 항목을 새
    bindingToken으로 re-home(단순 catch 로깅만으로는 관측만 되고 손실은 그대로). 실증 절차: spec 26
    flow 트레이싱으로 notify terminal이 unbind 전/후인지 + reportOwnershipRefreshError 유무로
    로컬/크로스노드 경로 판별(하니스 여유 시 실행 — 현재 6셀 실행과 직렬화 대기).
  - **[ ] [신규 등재] cpp 샘플 기존 결함 2건(단계 5 대상, baseline 실증)**: ① TicTacToe —
    `location_committed` marker가 completion 완료 증거가 아님(marker 후 on_actor_joined→source
    leave→completion 순서, spot_runtime.cpp:6216) + 앱 callback의 JoinGameNotify가 bound Session
    `.submit()`만 해 detached delivery terminal과 미결합(player_actor.hpp:90) → 간헐 미수신(baseline
    3/5). ② Bingo — detached FIFO `pending.dispatch()` 사후 실패 시 `target_closed`
    (actor_gateway_runtime.cpp:119, baseline 재현). 모두 Join-chunk 전환과 무관한 기존 결함으로
    5회×2(현/base) 대조 판정.
  - **[x] [해소] cpp SpotActorTransfer e2e HEAD red(5/5 결정적)**: stale e2e 바이너리가 가리던
    실패 — 재빌드 후 ST-C4/D2 `get_actor_ref` 404(원격 Join 후 target actor_directory find 실패),
    ST-C2 session bind 실패. 용의: `ea7805d54b`(cpp ZLAU 정렬) 또는 `dd234c3110`. 회귀 판정 진행 중.
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
- **[x] A5-canonical-multiattempt — 실증·판정 완료(2026-08-22)**: 실제 ingress 실증(스테이지
  `user-spot-join-node-dotnet-multiattempt`, `327c2b86c1`) + sol 정합 분석 결과, **later-attempt-wins는
  admission registry 계층에서 동작하며 Node/.NET 대칭**((ActorId,ObjectGeneration) 단일 슬롯,
  새 transfer가 이전 entry abort+delete; .NET ZLinkActorJoinPrewarmRegistry / Node
  formal-remote-actor-admission-registry.ts:100). eager-materialize가 supersession을 깨지 않음 —
  열린 질문 해소, 3b 비차단. .NET의 버려진 attempt는 deadline expiry로 잔여물 없이 정리(준수).
  **판정된 스펙-gap 3건(semantic — 포맷-온리 아님, 별도 수정 카드, 매트릭스와 병행)**:
  (A) superseded attempt의 terminal이 opaque RequestFailed(17)→InternalFailure — spec 15 실패표상
  public `Unavailable`이어야 하고, wire 코드는 스펙 침묵 → **판정: `ActorLocationStale(21)`을
  정본 wire 코드로 채택, 4언어 전파**(양 target 대칭 발산). (B) .NET은 PREPARE 이후 단계에서
  newest-wins 미준수 — 15 §4.2 "기존 prep을 언제나 먼저 abort" 위반(ZLinkActorHandoffState.cs:438
  "already active" 거부). (C) Node target의 admission-전 eager-materialize(claimLocation=true)가
  버려진 attempt의 actor state/factory 자원을 expiry에서 미정리 — "Accepted 전 factory 준비,
  CAS 전 공개 금지" 계약과 발산(.NET과 비대칭). **[승격 2026-08-22] C는 역방향 스테이지 blocker로
  실증**: .NET→Node에서 canonical prepare의 eager getOrCreateActor가 Entry Spot authority claim을
  요구해 예외(opaque 105), 우회 시 restore(40)가 existing-actor 거부 — 자기모순. 해소 = §0 프레임
  (Node 사설 경로의 admission registry+hidden-actor restore 기계로 canonical prepare 배선 교체,
  신규 로직 금지) — sol 진행 중. 참고: "재-park"는 스펙상 receiver admission
  직렬화 의미로, source 재배치 개념은 침묵/비적용(§3a의 이전 표현 정정).
- **[구판 기록] A5-canonical-multiattempt (열린 설계 질문이던 시기의 기록)**:
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
- **stage-1 설계(리서치 매핑 완료, execution-ready)**:
  - **기존 relocation 스테이지는 actorJoin(28)을 안 씀** — Entry-Spot actor + whole-node `relocate()`이며 `joinSpot`
    호출·28 admission 자체가 없다. 3b는 **진짜 새 종류**(User-Spot JoinSpot)를 신설한다.
  - **Node source mode `user-spot-join-source`**: Entry Spot로 source actor 생성 후 handler에서
    `actor.context.joinSpot(targetSpotId, request).timeout(..).defer()` 호출 → remote User-Spot join
    (actor-remote-joiner.ts:97 → actor-local-native-join.ts:330). canonical 선택은 런타임 자동
    (authority fence + no bound session + peer capability, actor-local-native-join.ts:357;
    peer lifecycle gen 일치 + service-wire capability, service-stateful-runtime.ts:5039).
  - **.NET target mode `user-spot-target`**: `AddSpotFactory<RelocationUserSpot>`(entry-spot mode 인프라 재사용),
    ready 전에 고정 target User Spot 생성(`spots.GetOrCreate(spotId, UserSpotType).InMesh(..).Request(..).Async`,
    ActorNodeEndpoints.cs:422 패턴). `OnActorJoinAsync`가 admission+typed app-reply, `OnJoinedActorAsync`가
    lifecycle 마커. 수신은 `ZLinkSpotActorJoinDispatcher`→`AdmitCanonicalActorJoinAsync`.
  - **canonical-only 관측 신호(리서치 최대 리스크 → 해소책)**: host 마커만으론 canonical vs 사설 fallback 구분 불가.
    **기존 message-flow 트레이싱(spec 26, [[zlink-debugging-message-flow]])을 사용** — .NET dispatch는 이미
    `ZLinkFlowContext.Enter`(ZLinkSpotActorJoinDispatcher.cs:35), Node는 `decodeActorJoin28`
    (service-stateful-runtime.ts:1929)에서 canonical 분기. Flow capture를 켜고 **command-28 decode/admit 지점의
    canonical-특화 flow 이벤트**를 스테이지 성공 조건에 넣는다(새 콘솔 계측 추가 금지). 구현자가 정확한 flow 이벤트 확정.
  - **multi-attempt 훅(A5 §3a 실증)**: 공개 `joinSpot().defer()` 2회 금지(deferred-join이 concurrent pending 거부).
    **test-only host ingress 송신**으로 같은 actor identity·distinct correlation의 canonical attempt 2개를 실제
    `joinActorSpotCanonical` transport로 겹치게 발사 → newest-wins((ActorId,ObjectGeneration) 키) 관측. 사설 JSON
    hand-encode 금지, 반드시 실제 transport 사용.
  - **런너 배선**: .NET `user-spot-target` 먼저 기동→ready+`user-spot-created` 대기→Node `user-spot-join-source`.
    전용 selector `user-spot-join-node-dotnet`(기존 `relocation-node-dotnet`에 접지 말 것 — 다른 admission 경계).
  - **구현 전 해소할 불확실성**: ① canonical-only observable(위 Flow로) ② target User Spot가 source 시작 전 완성
    (placement가 source 택하거나 fallback 방지) ③ automatic discovery 유지(PeerConnections.Connect 추가 시 .NET
    maintenance-relocation 제약 재현) ④ 런너의 "cross-lang User-Spot reply 비호환" 주석은 **stale**, 무시.
- **[정정됨] stage-1이 노출한 것 = Node→.NET canonical 28 admission 미완(별도 대규모 작업 아님)**:
  ⚠ 이전 커밋(e96334f07e·6addd7c720·4de89c8248)에 "authority row 포맷이 3가지로 갈린 대규모 gap"이라 적었으나 **틀렸다**.
  스펙 21 §2.4(:490-505)가 명확: **authority record는 canonical JSON(4언어 공통)**, `allocation.stableType`은 JSON field,
  `payload`만 base64 opaque bytes. **Node의 JSON authority는 정상(적합)**. ZLAU/ZLAP/`v1`은 authority row가 아니라 그
  안의 opaque `payload`/relocation 인코딩(별개 계층)이다.
  - **실제 관측**: Node가 canonical 28을 정상 전송(observable `wire_command=28 canonical=true`, force-private 음성검증
    통과)한 뒤, .NET `AdmitCanonicalActorJoinAsync`가 source authority의 `payload`를 `TryDecodeRelocating`
    (ZLinkFrameworkRuntimeActors.cs:2399)하는 지점에서 admission이 미도달. 즉 **Node→.NET canonical actorJoin의
    relocating-payload 소비가 아직 end-to-end로 안 맞는** 마이그레이션 미완 지점이다(=진행 중인 canonical 작업의 일부).
  - **판정**: 새 결정·새 대규모 트랙 아님. canonical 커스텀바이너리 마이그레이션의 **일반 완료 항목**으로 다룬다.
  - **[정정의 정정 — 위 "[확정] 포맷 문제 아님"(구판)은 틀렸음]**: "whole-node Node→.NET relocation 스테이지
    통과가 authority interop을 증명한다"는 전제가 **오류** — 그 경로는 `relocate()`+commit 시 authority rewrite라
    `AdmitCanonicalActorJoinAsync`/`TryDecodeRelocating`을 거치지 않는다(별도 경로, node_peer_host.js:493,
    actor-transfer-runtime.ts:1233). canonical 28 admission의 크로스랭 authority-read는 3b 스테이지가 처음이다.
    **최종 grounded 결론(⭐핸드오프와 동일): 포맷 mismatch가 맞다** — Node payload=JSON vs .NET decoder=ZLAU 요구.
  - **정본 판정·진행(2026-08-21, ⭐핸드오프 항목 1 참조)**: 정본 = ZLAU binary `authority-payload-v1`
    (schema durableFormats + golden `durable-authority-v1.json` consumers 4언어). Node 정렬(포맷-온리:
    ZLAU 코덱 신설 + JSON payload 경로 제거 + binary ZLAP/ZLAR envelope 정렬 + golden conformance test)을
    codex sol로 진행. **admission 로직·검증 무변경**, harness 스테이지 보존(`all` 미포함). C++ 텍스트 포맷은
    3b C++ 방향 착수 시 정렬. sol 리뷰 4건(잔존 JSON journal writer·ZLAP phase 이원화·zero-generation
    거부·conformance 강화) 수정 완료, Node 게이트 그린(기존 baseline m6b 1건 제외 — `remote Entry Spot
    actor join derives the well-known node route fence`는 stash-baseline에서도 동일 실패로 확인된 기존 이슈).
  - **[2차 blocker 진단 완료(2026-08-21, dump-증거)] canonical 28 payload 이중 포장**: ZLAU 정렬 후
    `TryDecodeRelocating` fence는 통과. 다음 실패 = Node canonical 발신이 command 28의
    `application-payload-envelope-v1` payload(schema :7292) 안에 `encodeFrameworkActorJoinPayload`의
    **`ZLAJ` 중첩 포장**을 넣음(actor-local-native-join.ts:384 → actor-join-payload-codec.ts:16 →
    service-stateful-runtime.ts:4264). .NET target은 외부 envelope를 벗겨 raw bytes를 application
    request로 쓰므로 handler JSON decode가 첫 바이트 'Z'에서 즉시 fault → Accepted/reply 미생성 →
    Node deferred join 30s 절대 deadline까지 대기. Store fence 구간은 36ms(admission 지연 아님, 4개
    지연 가설 전부 기각). **수정 완료(sol)**: canonical 발신=raw `request.data()` 직접
    (actor-local-native-join.ts:384), canonical 수신=ZLAJ 스킵·raw 전달(spots/index.ts:1923),
    legacy 경로 ZLAJ 보존(:104/:236). .NET(ZLinkActorRemoteJoiner.cs:979)·Java(ZLinkActorSpotJoinCall.java:746)와
    동형 확인. Node 게이트 그린(baseline 1건 제외). → **.NET typed admission Accepted 도달**(2 run 재현).
  - **[~] 3차 blocker(진단 완료·수정 중) = .NET reply 경로 결함**: .NET canonical actorJoin ingress가
    `MessageType=Request`/`RequestSeq`를 보존하지 않고(ZLinkManagedMeshNode.cs:5513) admission
    terminal(Accepted 20 + chunk-limit tail, payload는 정상)을 `TryScheduleRoutedSend` **one-way**로
    발송(:5679) → Node request promise 미완료(unsolicited frame 거부), `reply_received` 없음 →
    capture/Restore/cutover 미시작 → 30s deadline. rejection/error terminal도 동일 경로(= follow-up A와
    동일 근원, 이 수정으로 함께 해소 기대). Node continuation 배선은 완전함이 확인됨
    (actor-local-native-join.ts:396→:479; Node↔Node는 ingress reply callback으로 완주,
    service-stateful-runtime.ts:3829). .NET target은 prewarm/temp queue 등록 후 Restore 대기(정상).
    위반 스펙: 51(:180·:571 — 20은 request terminal reply), 26(:35), 15 §4.2(:420). 기준 패턴 =
    `SendNativeTerminalReply`(:8969, `_socket.Reply(sourceRid, requestSeq)`). **수정 완료**
    (ZLinkManagedMeshNode.cs:5263/8997 exactly-once reply gate) — 코디네이터 stage 재실행으로
    accepted 즉시 소비 확인. Java/C++는 동일 결함 없음(읽기 대조 — .NET만 outlier였음).
  - **[x] stage-1 그린(2026-08-22)**: `user-spot-join-node-dotnet` **3/3 결정적 통과** + 기존
    `relocation-node-dotnet` 무회귀(코디네이터 직접 실행). 사다리 요약: ① ZLAU 정렬 → ② ZLAJ
    이중포장 제거 → ③ .NET terminal reply 경로(one-way→`_socket.Reply` exactly-once+backpressure
    재제출+gate 수명) → ④ 28↔40 ZLJR recovery 배선 + canonical 판별 ruling(private flag 0x01) →
    ⑤ SourceSpotId=Store snapshot CurrentSpotId(추정값 제거) → ⑥ HandoffId=AggregateId 재사용 +
    reservation token/bytes 실값 전달. sol 리뷰 5회(각 라운드 findings 전부 해소), Claude 독립
    검증(스테이지 3회+회귀+baseline 대조). m6b 1건·stream-runtime 2건은 stash-baseline 동일 실패
    확인(기존 이슈, 비차단).
  - **[x] 4차 blocker(해소) = 28↔40 handoff identity 단절**: Node canonical source가
    admission(28)과 Restore(40)를 잇는 canonical handoff identity/Actor-Join recovery를 manifest에
    싣지 않음 → .NET target의 relocation envelope에 `remoteJoinRecovery == null`
    (ZLinkStandaloneActorRelocationRuntime.cs:1750/2037) → Restore가 **generic maintenance import**
    (:1994, marker `relocation_target_complete_entry`)로 처리, accepted prewarm claim 경로
    (:1987→PrepareCanonicalRoutedActorJoinTargetAsync→CompleteCanonicalRoutedActorJoinLifecycleAsync
    :1166) 미실행 → queue migration·OnJoinedActorAsync·completion·source leave 미실행, prewarm은
    deadline timer(ZLinkFrameworkRuntimeActorJoinPrewarm.cs:90)로 만료. 근거 스펙 15 §4.2(:303
    OperationId manifest 보존·:433/:451 temp queue 재사용·:464 순서)·28(:170·:261). C++ 2c의
    "off-wire handoff id .NET byte-parity"의 Node 대응물이 빠진 것. **주의**: m6c host-relocation
    테스트는 admission을 사전 주입해 이 seam을 못 잡음 — 실제 28→40 관통 테스트 필요.
    **sol 수정 진행**(Node 트리; 병렬로 리뷰3 High 2건도 수정 — .NET terminal reply
    backpressure 재제출 포함).
  - **[판정] Node canonical/legacy 28 판별 ruling(2026-08-21, Claude 단독)**: probe 결과 Node
    사설 flavor와 canonical 28은 **전체 multipart가 byte-identical**(사설이 canonical frame 형태를
    그대로 사용) → 구조 판별 불가로 sol STOP. 판정: **command 28 = canonical 전용**(4언어 수신자
    계약 동형 — .NET/C++: 28 head ⇒ canonical, 실패 ⇒ ProtocolError, fallthrough 없음). Node
    사설 flavor가 **private flag bit**로 이동(canonical decoder는 flags==0 요구라 자동 거부 —
    구조적 판별 확보; frame이 flags 불가면 private command id 대안). 사설 flavor는 Node↔Node
    전용(크로스랭 사설은 이미 .NET이 ProtocolError — 동작 무변화), 3c에서 경로 자체 삭제 예정인
    과도기 조치. payload sniffing은 opaque 계약 위반으로 최종 기각.
    (참고: Java canonical typed 경로는 content type을 `application/json`으로 고정 — Node/.NET은 실제
    serializer content type 보존. 잠재 L1 parity 관찰 항목, 비차단.)
  - **[ ] follow-up A(비차단, 후속)**: target application decode fault 시 .NET dispatcher의 fast-fail
    terminal(ZLinkSpotActorJoinDispatcher.cs:52)이 source에서 관측되지 않아 typed failure 대신 30s
    deadline으로 실패함 — malformed canonical payload의 즉시 typed 실패 전파를 focused 크로스랭
    검증으로 확인 필요(spec 26 §caller `reply_received` 계약).
  - **[ ] OPEN RULING — activationRecoveryState 인코딩 3자 발산(스펙-gap, Claude 판정 대기, 3b 비차단)**:
    opus 조사(2026-08-22, byte-offset 증거)로 확정된 사실 — schema 텍스트(u16 reference + u8-prefix
    sha256, 59B, replayCursor 없음) vs golden bytes(u8 reference + bare sha256, 57B) vs 구현:
    Java==golden(단 reference 255B cap), Node service codec==schema prefix **+ schema에 없는
    replayCursor u64**(67B, instance-activation 실사용), .NET ZLAU=항상 absent(tail opaque 통과),
    .NET 실제 recovery는 별개 ZLIS 포맷(u32 LE prefix, CRC32C, ReplayCursor), C++=absent.
    validator의 golden 검사는 손코딩 거울이라 어떤 게이트도 못 잡음. recovery present 시
    Java↔Node 상호 해독 불능. **판정 필요 사항**: 정본 인코딩(u8 vs u16/sha256 prefix),
    replayCursor의 규범 여부(semantic — Node·ZLIS만 보유), Java reference bound, golden 재생성
    + validator를 schema-유도로 교정, 4언어 전파. Node를 golden에 맞추는 건 replayCursor 유실이라
    포맷-온리 아님 — 설계 판정 후 진행. (actor-join 경로는 recovery를 소비하지 않아 3b 비차단.)
  - **[ ] follow-up B(비차단, 관측성)**: .NET `ZLinkSpotActorJoinDispatcher.ZLinkFlowContext.Enter`(:35)가
    context만 설정하고 command-28 admission flow 이벤트를 방출하지 않아 spec 26 계측으로 admission
    내부가 안 보임 — 반복 디버깅 비용 발생 시 spec 26 정식 이벤트로 승격 검토([[zlink-debugging-message-flow]]).

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
