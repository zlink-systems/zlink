# spec/server 재구성 — session 파일럿 매핑표

> 캠페인: `framework/doc/framework/common/spec/server`를 주제별로 재구성하고
> [스펙 문서 작성 가이드](../../../../principal/documentation/spec-writing-guide.ko.md)대로 다시 쓴다.
> 이 문서는 첫 주제 `session`의 작업 계획이다. 주제 루프(재작성 → 규칙 등가성 대조 →
> 구현 대조 → 판정·기록)의 양식을 여기서 굳혀 다른 주제에 그대로 쓴다.

관련: [spec-gap 대장](../../spec-gap.ko.md) · [주제 구분 초안](../../topic-map.ko.md)

## 1. 대상과 규모

| 현재 문서 | 줄 수 | 성격 | 외부 anchor 링크 수 |
|---|---|---|---|
| `19-stream-session` | 297 | 계약 — physical STREAM session, 등록, codec, 오류 경계 | 14 |
| `20-session-actor-dispatch` | 497 | 계약 — Session·Actor binding, relay, rebind, disconnect, relocation seal | 34 |
| `48-internal-session-binding` | 254 | 구현 스펙 — gate 분리, lane 정책 타입, 교체 순서, seal 구조 | 0 |

외부 참조 파일 87개(언어별 guide·e2e·sample·interface 문서). 코드 참조 1곳 —
`framework/languages/cpp/tests/Zlink.Framework.ContractTests/test_cpp_framework_layout_contract.cpp`
`session_actor_dispatch_documents_disconnect_destroy_boundary`가 20번 문서를 **경로로 열어 문장
3개를 needle로 검색**한다. 재작성 뒤 경로와 needle을 함께 갱신해야 한다(§6).

## 2. 독자 질문 — 주제 README가 답할 것

가이드 §1의 질문표를 session 주제에 맞게 채운 것. 새 문서의 절은 이 질문 순서를 따른다.

| 질문 | 답이 있어야 할 자리 |
|---|---|
| Session이 무엇이고 Application은 무엇을 보는가 | README 개요 + `stream-session` 개요 |
| 연결 하나에서 packet은 어떤 경로로 callback까지 오는가 | `stream-session` 정상 흐름 |
| Session과 Actor는 어떻게 연결되고, 한 Actor는 몇 개 session을 가질 수 있는가 | `session-actor-binding` 역할·binding 규칙 |
| 같은 Actor에 새 연결이 오면 이전 연결은 어떻게 되는가 | `session-actor-binding` 교체 |
| 연결이 끊기면 Actor는 어떻게 아는가 | `session-actor-binding` disconnect |
| Actor가 다른 node로 이동하면 연결은 유지되는가 | `session-actor-binding` relocation 중 Session의 책임 |
| 완료는 언제인가 (bind terminal, relay 수락, reply) | 각 문서 완료·실패 절 |
| 실패하면 무엇이 남는가 (`Unavailable`, `InvalidOperation`, seal timeout) | 각 문서 완료·실패 절 |
| 제한은 무엇인가 (`MaxMessageSize`, 100 ms, 3,000 ms, 1,000 ms) | 수치 표 한 곳 |

## 3. 새 구조

```
spec/server/session/
  README.ko.md                  주제 진입 1장 — 개요, 책임 표, 흐름 그림 1개, 질문→문서 표, 읽기 순서
  stream-session.ko.md          19 재작성 (+ 19 §10 permit은 execution 주제로 이관)
  session-actor-binding.ko.md   20 + 48 병합 재작성
```

(en 짝 문서 동일. 파일명에서 전역 번호를 뺀다 — 읽기 순서는 README가 정한다.)

### 3.1 `stream-session` 절 구성

| 새 절 | 옛 출처 | 서술 종류 |
|---|---|---|
| 1. STREAM session 개요 — Application이 보는 것, 범위 밖 | 19 서문, §1, §2 후반(범위 밖) | 계약 |
| 2. 역할과 책임 (Application / Framework / Core / connector) | 19 §2 bullet, §4 | 계약 + **결정**(모든 언어 `recv` mode, Core callback 미사용) |
| 3. 등록과 startup 검증 | 19 §7, §7.1, §7.2 | 계약 (선언·표는 절 뒤쪽) |
| 4. 연결 수락부터 callback까지 — 정상 흐름 | 19 §3, §4 경계 그림 | 계약 + **결정**(managed queue 뒤 callback, admission 실패 시 read 중단) |
| 5. reply 상관관계 | 19 §3.1 | 계약 |
| 6. payload 변환 — codec 경계 | 19 §5 | 계약 |
| 7. 오류 경계와 종료 | 19 §6, §4.1 | 계약 |
| 8. Session에서 Actor로 (요약 + 링크) | 19 §8 → 본문은 `session-actor-binding`으로 이관, 여기는 3~4문장 | 계약(링크) |
| 9. 수치와 제한 | 19 §7 `MaxMessageSize` 문단 | 계약 |
| 10. 검증 요구 | 19 §9 | 검증 |
| (이관) shared permit | 19 §10 | → execution 주제(33·46). 이 문서는 링크만 |

### 3.2 `session-actor-binding` 절 구성

| 새 절 | 옛 출처 | 서술 종류 |
|---|---|---|
| 1. Session–Actor binding 개요 — 전체 흐름 5단계, 질문·계약 표 | 20 §1 후반, §2 | 계약 |
| 2. 역할과 책임 — Session owner / Actor owner / relocation runtime / Application | 20 §1, §5 "Session에서만 검증하는 값", 48 §4 "단일 소유자", 52 §6 표 | 계약 + **결정**(검증은 경계마다 한 번) |
| 3. Startup 조건 | 20 §1 (`EnableActorDispatch`, role·Store), §8 표 startup 행 | 계약 |
| 4. Binding — 무엇을 잇고 무엇을 보관하는가 | 20 §4 앞부분(값 목록, 보관 표, 다중 binding 규칙) | 계약 |
| 5. Bind와 relay — 정상 흐름 | 20 §3, §4 중반(command 38·24·36, route 저장, Store 미조회) | 계약 + **결정**(session gate ≠ Actor gate — 48 §1; 제어 record는 application queue 밖 — 48 §1) |
| 6. 같은 Actor에 새 연결 — rebind와 교체 | 20 §4 후반(51, 100 ms, closing, tombstone, idempotent), 48 §3 전체 | 계약 + **결정**(즉시 확정·one-way 통지; `(연결 식별자, 교체 순번)` 식별) |
| 7. 연결 끊김 — disconnect 통지 | 20 §4.1 + diagram | 계약 |
| 8. Actor 이동 중 Session의 책임 — seal, held, route 전환 | 20 §5, §5.1, §6 + 48 §4 후반 + 52 §5·§7의 Session 부분 | 계약 + **결정**(seal ≠ retired 거부 — 48 §4) |
| 9. 재접속과 이동의 구분 | 48 §4 앞부분 표 | 계약 + **결정**(이전 연결 관계를 새 session에 옮기지 않음) |
| 10. 실행과 수명 | 20 §7 | **결정**(session owner 직렬화, infrastructure task 진행) |
| 11. 실행 engine과 lane 정책 타입 | 48 §2 | **결정** + **언어별 재량**(sealed 계층/tagged union) — [41](../../../../../framework/doc/framework/common/spec/server/41-internal-serialization.ko.md) 링크 |
| 12. 실패와 오류 | 20 §8 표(operation 행), §3 재전송 금지·late reply | 계약 |
| 13. Public interface 발췌 (.NET) | 20 §4 code block | 선언 |
| 14. 검증 요구 (계약 항목 + 확인할 결과 + 확인 방법 열) | 20 §9, 48 §5 | 검증 |
| (이관) Session control permit | 48 말미 | → execution 주제 |

## 4. 읽으면서 발견한 구조 문제 (재작성에서 처리)

| # | 문제 | 처리 |
|---|---|---|
| S1 | **relocation seal 흐름이 4곳에 서술**됨 — 19 §8 문단, 20 §5·§5.1·§6, 48 §4 후반, 52 §5·§7. 20 §5.1과 48 §4의 sequence diagram은 사실상 동일 | `session-actor-binding` §8이 **Session owner 행동**(seal 설치·hold·route 1회 전환·timeout·abort)을 단독 소유. 20 §5 단계 3~5(source/target 절차)는 [28 §4](../../../../../framework/doc/framework/common/spec/server/05-location-relocation/04-relocation-flow.ko.md) 링크로 축소. 52 §5·§7은 relocation 주제 차례에 링크로 교체(이번 주제에서는 손대지 않음) |
| S2 | **100 ms 교체 close 규칙이 3곳** — 19 §8, 20 §4, 48 §3 | §6 한 곳. 19는 한 문장 + 링크 |
| S3 | shared permit 절이 19 §10, 48 말미에 번호 없이 덧붙어 있음 (33 HWM 작업의 흔적) | execution 주제로 이관. 이번엔 두 문서 끝에 "→ execution" 링크 stub만 |
| S4 | 20 §4가 130줄 문단 벽 — bind·rebind·교체·disconnect·relocation·idempotent가 한 절 | 질문 기준으로 §4~§8로 분리 |
| S5 | 20 §8 표 머리 `Condition \| Result` 영문 (ko 문서) ; startup 표가 19 §7.2와 20 §8에 분산 | 표 머리 한국어화, startup 행은 각 문서 §3에 |
| S6 | 20 §1 제목 "이 문서가 정의하는 범위" — 가이드 §2.2 메타 제목 금지 | 주제 이름으로 |
| S7 | 19 §2 bullet에 계약과 결정이 섞임 (`recv` mode 강제는 결정) | §2에서 결정으로 표시 |
| S8 | command 번호 나열 순서가 문서마다 다름 (24·36·38·51 / 38·24·36 / 38·24·36·51) | 표 한 곳: 38 bind, 24 actorSend(+bound-session tail), 36 boundSessionSend, 51 boundSessionReplaced, 42/43 seal, 44 route, 45 reserved |
| S9 | 20 §4에 `<a id="4-binding-authority">`, §5.1에 `<a id="51-session-actor-위치-갱신-message">` 레거시 anchor | 외부 링크 치환 뒤 제거 여부 판단(§6) |
| S10 | 48 §4 "Target relocation runtime은 … 1,000 ms … CAS" 등 target 측 규칙이 Session 문서에 반복 | relocation 주제 소유. 여기서는 링크 |

## 5. 규칙 등가성 대장 — 초기 추출

재작성 뒤 이 표의 모든 행이 새 문서의 정확히 한 절에 있어야 한다("새 위치" 열은 재작성 뒤 채움).
행이 없으면 누락, 표에 없는 보장이 새 문서에 있으면 추가 보장(둘 다 대조 실패).

| # | 규칙 (수치·상태·오류) | 옛 위치 | 새 위치 |
|---|---|---|---|
| R1 | STREAM ingress는 모든 언어에서 `recv` mode; Core packet/raw callback 미사용 | 19 §2, §4 | |
| R2 | admission 실패 시 새 packet 읽지 않음; 이미 받은 packet 폐기·재전달 없음 | 19 §4 | |
| R3 | Session dispatch에 handler filter 미적용 | 19 §3 | |
| R4 | routing ID가 session dispatch까지 손실 없이 전달 | 19 §3 | |
| R5 | `Response`·`Error` header에 packet name 없음; sequence 단독 매칭; typed reply 타입은 client 지정 | 19 §3.1 | |
| R6 | 닫기 시작 시 새 admission 차단; read/write는 소유 문맥에서 완료·취소 후 resource 파괴; 이중 완료·중복 시작 없음 | 19 §4.1 | |
| R7 | codec registry: server root별 / HTTP host별 / connector instance별, instance 비공유 | 19 §5 | |
| R8 | 오류 경계 4행(transport 오류→callback, handshake·socket·node 오류→monitoring, handler 예외→handler 경로) | 19 §6 | |
| R9 | 종료 사유는 Stream Connector §6.3 닫힌 집합과 정합, 계기는 25 §4 소유 | 19 §6 | |
| R10 | 명시 등록만; attribute/decorator 등록 없음 | 19 §7 | |
| R11 | `MaxMessageSize` 기본 64 KiB; header+payload(6-byte prefix 제외); inbound만; `0`→Core `-1`; 음수→startup error; 초과 시 `EMSGSIZE` 기록 후 종료, client는 종료만 관찰 | 19 §7 | |
| R12 | node당 session type 하나 | 19 §7 | |
| R13 | TLS: cert+key 필수; client cert 요구 기본 false; 검증 실패 연결은 session 생성 전 거부 | 19 §7.1 | |
| R14 | startup 실패 8조건(이름 공백, 이름 중복, endpoint 없음, session type 중복, node당 2+ session, cert 공백, key 공백, TLS 없이 client cert 요구) | 19 §7.2 | |
| R15 | Session callback은 Spot 상태를 직접 변경하지 않음 | 19 §8 | |
| R16 | physical socket·session object는 session owner에 유지; 24·36·38·51만 node 간 전달; Node RID·generation·fence·codec 비노출 | 19 §8, 20 §5 | |
| R17 | close 시 current binding generation tombstone 제출 → 이전 bind의 late close가 새 binding 해제 못 함 | 19 §8, 20 §4 | |
| R18 | `EnableActorDispatch()`는 MeshName 없음; Object Client/Server role + Location Store 필요; 없으면 startup 거부; hidden local-only binding 없음 | 20 §1, §8 | |
| R19 | session:Actor = N:1 (session은 여러 Actor, Actor는 session 하나) | 20 §2, §4 | |
| R20 | relay는 bind 때 route 사용; message마다 Store 조회 없음 | 20 §2, §4 | |
| R21 | envelope 보존 값 6개(correlation, binding token, ObjectGeneration, AuthorityOwnerGeneration, OwnerLeaseGeneration, session sequence) | 20 §3 | |
| R22 | payload는 target Actor queue에 직접; session thread에서 Actor handler 실행 안 함; session/Spot global queue 직렬화 없음 | 20 §3 | |
| R23 | reply·error는 original correlation으로 terminal-once; timeout·cancel·route failure 뒤 자동 재전송 없음 | 20 §3 | |
| R24 | session 닫힌 뒤 late reply를 새 session/binding에 사용 안 함 | 20 §3 | |
| R25 | Actor destroy → binding 종료; 새 incarnation에 이전 token 무효; 새 `ActorRef`로 재bind 필요 | 20 §4 | |
| R26 | binding 보관 정보 5행 표 | 20 §4 | |
| R27 | bind = control request 1회(38); Actor owner가 ObjectGeneration·NodeGeneration·AuthorityOwnerGeneration 확인 후 generation 등록, terminal reply 1회 | 20 §4 | |
| R28 | 24 record에 binding generation + session sequence; 36 push는 source generation·NodeGeneration·AuthorityOwnerGeneration·expected binding generation이 current일 때만 제출 | 20 §4 | |
| R29 | bind 전 Store 사전 조회 없음; local instance overload 없음 | 20 §4 | |
| R30 | 저장 route 무효 시 Message Follow로 정확히 1회 또는 `Unavailable`; 자동 재전송 없음 | 20 §4 | |
| R31 | route 유효 범위 = owner lease + local admission deadline; Store 장애 시 연장 없음 | 20 §4 | |
| R32 | binding identity = (session owner Node RID, lifecycle generation, owner-local binding generation); 대소 비교는 같은 lifecycle 안에서만 | 20 §4, 48 §3 | |
| R33 | rebind: Actor owner 등록 → 성공 reply → 새 owner가 route 저장; terminal은 이전 session 완료 대기 없음 | 20 §4, 48 §3 | |
| R34 | 51 one-way 최대 1회 적용; identity 전부 일치 시만 callback; closing 전이(inbound 거부·outbound 허용); terminal 후 100 ms non-blocking timer close; timer는 identity 재확인; outbound 비어도 단축 없음; lifecycle deadline 내 미terminal 시 강제 종료 | 19 §8, 20 §4, 48 §3 | |
| R35 | 51 전송 실패·callback 실패·close 지연은 diagnostics만; bounded async retry; 새 binding rollback 없음; 이전 generation ingress 계속 거부 | 20 §4 | |
| R36 | unbind·disconnect는 callback terminal 뒤 38 tombstone으로 해당 identity만 제거; 이전 lifecycle의 late push·ingress·close 무시; malformed control/one-way는 application queue 밖, terminal route 없음 | 20 §4 | |
| R37 | 같은 physical session이 current binding 재제출 → idempotent 성공, 51 자기 전송·close 없음; 이전 connection close 시 다른 binding도 각 1회 정리, 새 identity 제거 금지 | 20 §4 | |
| R38 | 같은 ObjectGeneration 유지 relocation은 rebind 아님; destroy 후 새 generation은 explicit bind | 20 §4, 19 §8 | |
| R39 | 다른 owner/generation rebind: atomic 등록 → reply → 51 one-way; ACK 없음; bind 실패 시만 기존 route 유지 | 20 §4 | |
| R40 | bind 시 target에 Actor 없음: active Message Follow → relay; 없음·만료 → `Unavailable`; generation 다름 → `InvalidOperation`; pre-commit seal → `Unavailable`; hidden retry 없음; `BindOrGet`의 Get은 exact 동일 binding만 | 20 §4, §8 | |
| R41 | bound-session API: push·close만; global proxy 없음; disconnect는 binding 해제, Actor destroy·membership 변경 없음 | 20 §4 | |
| R42 | disconnect: snapshot 고정 → identity마다 자동 제출 → all-settled; `NotifyDisconnected`와 dedupe, callback 최대 1회; deadline 내 terminal 대기 후 tombstone; Store 조회 없음 | 20 §4.1 | |
| R43 | 통지 대상은 Actor의 현재 Entry/User Spot `OnDisconnectActorAsync`; 두 통지 모두 Actor destroy·membership 변경 없음 | 20 §4.1 | |
| R44 | relocation 중 socket·session state 이동·복제 없음; Session은 target 선택·준비 판정·Store 접근 없음 | 20 §5 | |
| R45 | Session owner 검증 값 4개(physical Session identity·SessionRid, binding generation·ActorId·ObjectGeneration, relocation identity, seal binding=route 변경 binding) | 20 §5, 48 §4, 52 §5 | |
| R46 | seal: 42 request → 43 reply(seal 결과만, sequence·high-water 없음) → hold; 다른 binding 무영향 | 20 §5, §5.1 | |
| R47 | 44 one-way commit: route+location snapshot 원자 갱신 → held 순서대로 제출 → seal 해제; 응답 없음; 45 미사용; duplicate no-op | 20 §5, §5.1, 48 §4 | |
| R48 | `SessionRelocationSealTimeout` 기본 3,000 ms, 설정 가능; timeout 시 physical Session 종료 + binding·held·seal 정리; timeout과 44는 같은 직렬 구간, 먼저 처리된 것만 유효; late는 `late_session_route_update` Warning | 20 §5, §5.1, §6, 48 §4 | |
| R49 | relay-ready accepted 전 명시 실패 → 44 abort one-way → matching seal 해제, held를 source route로; reply 없음. relay-ready 뒤 실패·cutover submit 실패 → source route 재개방 없음, seal timeout이 정리 | 20 §5, §6, 48 §4 | |
| R50 | CAS 뒤 source rollback 없음; 이전 주소 message는 Message Follow; connection 간 전역 순서 미보장; disconnect는 relocation 증거 아님; owner process 종료 시 connection 복구 없이 close | 20 §6 | |
| R51 | session owner가 handler turn·binding mutation·close·barrier 직렬화; Actor 제출 뒤 순서는 Actor queue; shared lock·callback stack 합치기 없음 | 20 §7, 48 §1 | |
| R52 | completion·binding update·barrier·cleanup은 infrastructure task에서 진행 | 20 §7 | |
| R53 | Session owner host Relocate/Shutdown: 신규 거부, accepted는 deadline까지, connection 이동 없음 | 20 §7 | |
| R54 | 오류 표: stale+no Follow→`Unavailable`; generation 다름→`InvalidOperation`; pre-commit seal→`Unavailable`; packet key 중복→startup; factory 없음→explicit create error; binding 없이 push/close→`InvalidOperation`(session-not-bound); stale fence→typed stale error, fallback 없음 | 20 §8 | |
| R55 | 결정: session gate ≠ Actor gate; 제어 record는 application queue 밖 | 48 §1 | |
| R56 | 결정: 실행 engine 하나; lane 정책 타입(Spot/session/Actor 전달 lane 상태 표); boolean 조합 금지; 재량: sealed/tagged union | 48 §2 | |
| R57 | 결정: 재접속은 새 session, 이전 관계 복원 없음; 이동은 연결 유지, route만 갱신 | 48 §4 | |
| R58 | 결정: relocation seal ≠ retired binding 거부 | 48 §4 | |
| R59 | seal에 numeric high-water·record 수·byte 상한 없음; 개별 message 제한은 그대로 | 48 §4, 20 §5 | |
| R60 | 확인할 결과 17항(48 §5) + 검증 요구 9항(19 §9) + 16항(20 §9) | 각 검증 절 | |

(R60은 재작성 시 항목 단위로 펼쳐 확인 방법 열을 붙인다.)

## 6. 링크·코드·site 영향

| 대상 | 처리 |
|---|---|
| 스펙 내부 링크 (16개 문서가 19/20/48을 참조) | 새 경로·새 절 anchor로 치환. 절 제목이 바뀌므로 anchor 치환표를 §3 매핑에서 생성 |
| 외부 87 파일, anchor 링크 48종 | 같은 치환표로 sed. 언어별 guide는 `generate_language_guides.py`가 공통 guide에서 생성하므로 **공통 guide만 고치고 재생성** |
| cpp layout contract test | 경로를 `session/session-actor-binding.ko.md`로, needle 3개를 새 문장으로. 문장은 재작성 때 이 needle이 가리키는 계약(disconnect는 destroy·membership 변경 없음)을 한 문장으로 고정해 두고 그 문장을 needle로 쓴다 |
| 레거시 `<a id>` 2개 | 외부 링크 치환 후 참조 0이면 제거 |
| mkdocs nav | "STREAM and session" 그룹 → `04-session/README`, `stream-session`, `session-actor-binding` |
| redirect | 캠페인 말미 site 작업에서 `19-stream-session`→`session/stream-session`, `20-…`→`session/session-actor-binding`, `48-…`→`session/session-actor-binding` |
| 검증 | `check_doc_links.py`, `mkdocs build --strict`, `git diff --check`, cpp layout contract test |

## 7. 구현 대조 — 에이전트 입력

재작성이 끝나면 언어별 에이전트 4개(dotnet·jvm·cpp·node)에 다음을 준다.

- 입력: 새 `stream-session`, `session-actor-binding` 두 문서와 §5 대장(새 위치 열 채운 것)
- 과제: 대장의 행마다 해당 언어 구현에서 **일치 / 불일치 / 스펙 미정 / 판단 불가** 중 하나와 근거(파일:줄)
- 금지: 스펙 수정, 코드 수정. 판정은 하지 않고 사실만 보고
- 출력 양식: `| R# | 판정 | 근거 | 비고 |`

판정(Claude): 옛 문서·4개 구현이 일치하는데 새 문서만 다르면 **문서 오류** → 수정.
옛 문서 때부터 달랐거나 언어끼리 다르면 **spec gap** → [spec-gap 대장](../../spec-gap.ko.md) 등록,
문서는 계약 의도대로 유지.

## 8. 작업 순서

1. `04-session/README.ko.md` 초안 (§2 질문표 기준) — 문서 소속·중복 판정의 최종 확인
2. `stream-session` 재작성 (ko) → §5 대장 R1~R17 새 위치 채움
3. `session-actor-binding` 재작성 (ko) → R18~R60
4. 등가성 대조 — 대장 빈 행 0, 추가 보장 0
5. en 짝 작성
6. 링크 치환·guide 재생성·nav·cpp test 갱신 → 검증 4종 그린
7. 구현 대조(§7) → 판정·기록
8. 한 커밋(문서 이동+내용) + spec-gap 대장 갱신
