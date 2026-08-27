# 재작성 대장 — 06-spot-address-messaging · 07-stage-wrapper-on-spot

> 옛 `16-spot-address-messaging.ko.md`(217 규칙)와 `17-stage-wrapper-on-spot.ko.md`(80 규칙)를
> 재작성한 대장이다. [매핑표](mapping.ko.md) §5의 `16-spot-address-messaging-R#`,
> `17-stage-wrapper-on-spot-R#` id를 그대로 쓴다. 연속된 R#가 같은 절로 갔으면 범위로 묶었다.
> 새 위치는 새 문서의 `##`/`###` 절 번호다. 각 행은 grep으로 핵심 문구를 새 문서에서 실물
> 확인한 뒤에만 적었다(가이드 §2.5).

## 06-spot-address-messaging.ko.md

| 규칙 ID | 새 위치 | 비고 |
|---|---|---|
| R1–R4 | §1 개요 | Application/Framework/Core 책임 표로 재구성 |
| R5–R13 | §2 Spot ID와 SpotRef (본문) | Spot ID 형식·wire encoding, 굵은 규칙+불릿으로 분리 |
| R14–R15 | §2 | MeshName·Spot kind 충돌 규칙 |
| R16–R18 | §2 | stable type 형식·정규화 금지·중복 등록 오류 |
| R19–R33 | §2.1 Entry Spot ID | Entry Spot ID 발급·예약 형식·NewClaim·cleanup 전체 |
| R34–R38 | §2.2 SpotRef | `SpotRef` 필드를 contract pseudocode 선언 + 인라인 주석으로(§8.3) |
| R39–R40 | §2.3 Instance Spot | Instance Spot 정의(membership 없음, 사용 가능/불가 표면) |
| R41–R49 | §3 User Spot 명시적 생성 (본문) | Create/GetOrCreate 비교표, option 규칙, deadline |
| R50–R52 | §3.1 Mesh와 capacity 선택 | InMesh 생략·후보 0/2+·weight 선택 |
| R53–R56 | §3.1 | encoded request 상한, factory 재실행 안전성 |
| R57–R66 | §3.2 동시 요청의 수렴 | Create UUID 발급, GetOrCreate convergence, deadline 초과 |
| R67–R72 | §3.2 | Terminal result state 표(Existing/Created/Rejected) |
| R73–R81 | §3.3 Remote User Spot 생성 | command 47 payload, reservation fence, command 20 tail |
| R82–R83 | §3.4 Find와 query | `Find` 비생성, query 상한 |
| R84 | §4 Cold activation (도입부) | "existing-only" 기본 규칙 |
| R85–R89 | §4 | cold activation 정의·intent 명시·Mesh 선택 옵션 |
| R90a–R90h | §4.1 절차 1~8단계 | resolve부터 envelope 전송까지, S10 참고 |
| R91–R94 | §4.1 절차 8단계 하위 불릿 | route kind 1/2, deadline 일치, operation identity |
| R95–R96 | §4.1 절차 9단계 (구 "8." 중복, S10로 9로 정정) | target의 owner·generation 확인 |
| R97–R101 | §4.1 절차 10단계 | Relocation Store recovery root, Reserve, Missing→Creating |
| R102–R106 | §4.1 절차 11단계 | factory 실행, durable inbox barrier(내부 확인 조건), Ready commit |
| R107–R109 | §4.1 절차 12단계 | replay cursor 갱신, CAS 삭제, root 삭제 |
| (다이어그램) | §4.1 mermaid | cold activation 단일 sequence diagram(가이드 §7.2) — 이 문서가 유일하게 소유 |
| 12-spot-messaging-R72 | §4.1.1 | target process가 `Reserve` 뒤 종료 → restart의 complete authority scan이 Pending creation을 재개하거나 fence로 중단. **원문 16에는 없었고 12-spot-messaging에만 있던 규칙** — 코디네이터 교차 검토로 발견, S1에 따라 이 문서(cold activation 단독 소유)로 이전 |
| 12-spot-messaging-R73 | §4.1.1 | `Ready` commit 뒤 queue 선두 복원 전 종료 → recovery root·cursor로 최초 record 우선 복원, 복원 전에는 Serving gate 미개방. **원문 16에는 없었고 12-spot-messaging에만 있던 규칙** — 코디네이터 교차 검토로 발견, S1에 따라 이 문서로 이전 |
| R110–R118 | §4.2 여러 node가 동시에 첫 message를 받는 경우 | 동시 activation 수렴, TypeMismatch |
| R119 | §5 (본문) | Spot direct 시작 method 시그니처 |
| R120–R121 | §5 (본문, 개념 문단) | Positive route cache resolve 개념 한 문단 + [08-routing](08-routing.ko.md) 링크 — 정확한 필드·수명은 08-routing 소유(S3) |
| R122–R123 | §5 | local/remote 동일 의미, existing-only 규칙 |
| R124 | **08-routing.ko.md** (S3, 이 문서는 링크만) | negative cache 상세 규칙은 08-routing 소유. 06 §10 검증 절에는 관찰 가능한 결과로 재수록(R212 참고) |
| R125 | §6 (본문, RouteCacheMaxAge 문단으로 이동) | positive cache 사용 조건을 수치 옆으로 재배치 |
| R126 | **08-routing.ko.md** (S3, 이 문서는 링크만) | invalidate 조건(StoreVersion 등) 상세는 08-routing 소유 |
| R127–R129 | §5 | resolve 후 close/recreate, stale route, 자동 재제출 금지 — 이 문서가 소유(cache 내부가 아니라 호출 완료 계약) |
| R130–R133 | §5 | one-way 완료 경계, outbound admission 정의, request terminal-once, hidden retry 금지 |
| R134–R137 | §6 (본문) | RouteCacheMaxAge/MessageFollowDuration 기본값·0 처리·5초 gap·런타임 변경 범위 |
| R138–R146 | §6 | Message Follow 검증 필드, hop 상한, 대기열 무제한, 오류 매핑 |
| R147–R151 | §6.1 | SpotWide aggregate commit, PerActor route 분리, ToSpot/ToActor |
| R152–R155 | §6.1 | seal-hold-abort-relay 규칙 유지, 순서 전체 소유는 [05-spot-actor-membership](05-spot-actor-membership.ko.md) 링크로 명시(S4·S17) |
| R156–R158 | §7 (본문) | Close 대상(User Spot exact SpotRef, Instance Spot local Close) |
| R159 | §7 절차 1~4단계 | Close 4단계 |
| R160–R164 | §7 | 실패 분기, membership 보유 시 false |
| R165–R166 | §7.1 Remote Close | command 48 payload |
| R167–R168 | §7.1 | target 검증 순서 |
| R169–R172 | §7.1 | command 20 tail, false 사용 조건, typed failure |
| R173–R178 | §8 (본문) | Relocate 시작 조건, factory 정책 3종, PerActor 제약, target shell 비공개 |
| R179–R181 | §8 | relay-ready reply 전 실패만 source 유지, target 종료 시 자동 재개 안 함 |
| R182–R183 | §8 | timer relocation payload 포함, Restore 재등록 금지 |
| R184–R185 | §8 | SpotWide/Instance vs PerActor 적용 범위 |
| R186 | §8 | 자동 재제출 금지, ingress hold relay |
| R187–R193 | §9 실패와 관측 표 | typed error 표로 재구성(S11 — 셀 분리) |
| R194–R195 | §9 | 관측 정보 구분, Spot ID metric 금지 |
| R196–R199 | §10 "Spot ID와 예약 형식" | |
| R200–R204 | §10 "Create와 GetOrCreate의 수렴" | |
| R205–R211 | §10 "Direct call과 cold activation" | R210의 "barrier"·"queue 선두 복원" 내부 서술은 §4.1 절차 11단계로 옮기고, 검증 절에는 "cold activation 최초 message가 항상 먼저 처리된다"는 관찰 가능한 문장으로 재작성(가이드 §4.4·§9.3) |
| R212–R213 | §10 "Route cache와 Message Follow" | R212(negative cache)는 관찰 가능한 결과로 유지, 내부 cache 구조는 08-routing 소유 |
| R214–R216 | §10 "Close" | |
| R217 | §10 "언어 사이 정합" | |

## 07-stage-wrapper-on-spot.ko.md

| 규칙 ID | 새 위치 | 비고 |
|---|---|---|
| R1–R2 | §1 개요 | Framework가 Stage runtime 미제공, wrapper가 조합 |
| R3–R9 | §2 책임 경계 표 | 표 그대로(이미 셀당 규칙 하나) |
| R10 | §2 | 굵은 규칙 문장으로 승격(비공개 표면 목록) |
| R11–R12 | §3 Spot turn 보존 (본문) | Spot turn 대상 callback 목록 |
| R13–R14 | §3 | SpotWide 공통 gate |
| R15–R18 | §3 | PerActor lane 분리 |
| R19 | §3 | 비동기 실행 정책 링크(`../05-async-execution-policy.ko.md`) |
| R20–R21 | §3 | continuation 재제출, transport thread 직접 변경 금지 |
| R22–R24 | §4 Actor 경계 | Actor payload가 Actor queue로 직접 전달 |
| R25–R26 | §4 | Stage state 변경은 명시적 호출 |
| R27–R28 | §4 | SpotWide/PerActor gate 차이 |
| R29–R31 | §4 | lifecycle 전용 queue, [04-actor-model](04-actor-model.ko.md) 링크 |
| R32–R34 | §5 Timer와 Yield (본문) | timer 등록·직렬화 범위 |
| R35–R38 | §5 불릿 | 종료 admission, 순서, option, native handle 비공개 |
| R39–R42 | §5 | Yield 허용 범위 |
| R43–R46 | §5 | Actor Yield 중 queue head 유지, 자기 자신 request 거부 |
| R47–R48 | §5 | Relocate 시작 시 기존 turn 유지, 내부 notification 비callback |
| R49–R50 | §5 | timer relocation payload, Restore 재등록 금지 |
| R51–R53 | §6 생성과 membership (본문) | Create/GetOrCreate 연동, 동시 생성 수렴 |
| R54–R56 | §6 | Actor join, [05-spot-actor-membership](05-spot-actor-membership.ko.md) 링크 |
| R57–R59 | §6 | Stage 알림 경로 2종, Logical Multicast durable source 금지 |
| R60–R63 | §7 Location과 수명 (본문) | domain key resolve, SpotRef 사용, [06-spot-address-messaging](06-spot-address-messaging.ko.md) 링크 |
| R64–R65 | §7 | 종료 drain, 종료 뒤 callback 미생성 |
| R66 | §8 Metadata와 관측 | metadata snapshot 그대로 제공 |
| R67–R68 | §8 | 관측 정보 구분, ID metric 금지 |
| R69–R80 | §9 구현 및 contract test 검증 요구 | "Spot turn 보존" · "Yield와 gate" · "Actor 경계" · "표면 준수와 종료" 4개 소제목으로 재그룹(가이드 §9.3 "주제별로 묶어 완결되게") |

## 나중에 anchor를 붙일 링크

같은 topic(03-spot-actor) 안에서 병렬로 쓰는 문서라 아직 없는 절 제목에는 anchor를 달지 않고
파일만 링크했다. 해당 문서가 완성되면 정확한 절 anchor로 보강해야 한다.

| 이 문서 | 링크한 파일 | 필요한 anchor |
|---|---|---|
| 06 | `04-actor-model.ko.md` | (06 본문에서는 직접 링크 없음 — 07이 사용) |
| 06 | `05-spot-actor-membership.ko.md` (nav, §1, §6.1, §8) | Actor join 순서·relocation seal-commit-restore 절 |
| 06 | `08-routing.ko.md` (§5, §10) | positive route cache 필드·수명 절 |
| 07 | `04-actor-model.ko.md` (§4) | Actor queue·lifecycle 처리 절 |
| 07 | `05-spot-actor-membership.ko.md` (§6, nav) | 동시 변경 확정 방법·relocation 중 message 수락 경계 절 |
| 07 | `06-spot-address-messaging.ko.md` (§7) | 이 문서 자신이 쓴 절이므로 anchor 확정 가능 — `#6-route-cache-수치와-message-follow` 등 정확한 절 번호로 이미 링크했다(추가 작업 불필요) |
| 07 | `08-routing.ko.md` (nav "다음") | 파일만, anchor 불필요(nav는 문서 전체를 가리킴) |

## 이동 후 갱신할 링크

캠페인 마지막 이동 단계(§5)에서 옛 flat 경로 문서가 주제 디렉터리로 옮겨지면 아래 링크의
상대 경로와 anchor를 함께 갱신해야 한다.

| 이 문서 | 현재 링크 | 옮겨갈 것으로 예상하는 곳 |
|---|---|---|
| 07 | `../05-async-execution-policy.ko.md` (§3) | `01-execution/` 주제로 이동 예정 |
| 07 | `../04-message-model.ko.md` (§8) | `02-channel-transport/` 주제로 이동 예정 |

`01-glossary.ko.md`는 이번 재구성에서도 최상위에 그대로 남는 계획이라(§2 결과물 트리) 갱신
대상이 아니다.

## spec-gap 후보

이번 재작성에서 새로 발견한 gap은 없다. 매핑표 §4의 S20(생성 상태 다이어그램 leaf 이름
`Failed` vs `Aborted` 불일치)은 `14-actor-model`·`15-spot-actor` 소관이며 이 문서들의 범위 밖이라
여기서는 등록하지 않는다 — 해당 주제 작업자가 `spec-gap.ko.md`에 등록한다.

06·07 자체 대조에서는 규칙 누락이나 신규 보장을 발견하지 못했다. R124·R126을 08-routing으로
위임한 것은 gap이 아니라 매핑표 S3가 이미 확정한 소유권 이전이다.

코디네이터 교차 검토로 `12-spot-messaging-R72`·`R73`(target process가 activation 도중 종료된
경우의 재개·복원 규칙)이 옛 16과 06 양쪽에 모두 빠져 있었음을 발견했다 — S1(cold activation
전체 절차를 이 문서가 단독 소유)에 따라 06 §4.1.1로 추가했다. 이는 gap이 아니라 12가 소유하던
중복 서술 중 이 문서로 옮겨야 했던 부분이 누락됐던 재작성 오류이며, 이번 정정으로 해소했다.
