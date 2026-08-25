# ledger-02 — 02-glossary.ko.md (옛 01-glossary.ko.md)

> 대상: `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md`
> (옛 `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md`, 2,232줄 → 새 문서 2,257줄)

매핑표 [`mapping.ko.md` §3.0](mapping.ko.md#30-glossary-처리-방침-가이드-32)의 A/B/C
판정 기준을 129개 항목 전체에 적용한 결과다. `<a id>` anchor 127개와 `### 용어` 제목
129개는 옛 문서와 정확히 동일하게 유지했다(§ 끝의 "anchor 대조" 참고).

## 항목별 A/B/C 판정

| 용어 | 갈래(A/B/C) | 옮긴 절차의 소유 문서 | 비고 |
|---|---|---|---|
| Spot | A | — |  |
| Spot ID | A | — |  |
| Entry Spot, User Spot과 Instance Spot | A | — |  |
| Actor membership | A | — |  |
| User Spot execution mode | C | [Spot 모델 「5. User Spot」](../../03-spot-actor/01-spot-model.ko.md#5-user-spot) | PerActor target 복원 절차 문단 삭제, 링크로 대체. 03-spot-actor는 병렬 진행 중인 다른 주제 — 완료 확인 필요 |
| Spot relocation coordination mode | C | [Spot 모델 「5.1 SpotWide relocation 경계」](../../03-spot-actor/01-spot-model.ko.md#51-spotwide-relocation-경계) | Defer() 뒤 callback 호출 순서 문단 삭제, 링크로 대체. 03-spot-actor 완료 확인 필요 |
| MeshNode | A | — |  |
| RouteMesh | A | — |  |
| Location Store | A | — |  |
| Object Client와 Object Server role | A | — |  |
| MeshName | A | — |  |
| Spot kind | A | — |  |
| Stable type | A | — |  |
| ObjectGeneration | A | — |  |
| Owner | A | — |  |
| Authority | A | — |  |
| Compare-and-set | A | — |  |
| Ready | A | — |  |
| Admission seal | A | — |  |
| Owner route | A | — |  |
| Owner fence | A | — |  |
| Target descriptor fence | A | — |  |
| Positive route cache | A | — |  |
| Creation attempt | A | — |  |
| Reservation ID | A | — |  |
| Creation terminal result | A | — |  |
| Instance intent | A | — |  |
| Cold activation | B | [Framework API 「16. Missing object 생성 — cold activation 순서」](../06-framework-api.ko.md#16-missing-object-생성--cold-activation-순서) | S1 — 절차 문단을 링크로 대체, 정의 표(형태·NET표기·공개구성·수명)는 그대로 유지 |
| Activation envelope | A | — |  |
| Operation identity | A | — |  |
| Actor Join OperationId | A | — |  |
| Deferred Join barrier | A | — |  |
| Bounded aggregate commit | A | — |  |
| Message Follow | A | — |  |
| Message Follow duration | A | — |  |
| Relocation ingress hold | A | — |  |
| Reply correlation | A | — |  |
| Deadline | A | — |  |
| Factory | A | — |  |
| Activation barrier | A | — |  |
| Durable activation inbox | A | — |  |
| Replay cursor | A | — |  |
| Activation recovery pointer | A | — |  |
| Recovery receipt | A | — |  |
| Reservation fence | A | — |  |
| Spot direct | A | — |  |
| Spot turn | A | — |  |
| Async와 Yield | A | — |  |
| One-way 정상 완료 | A | — |  |
| Backpressure | A | — |  |
| Backpressured | A | — |  |
| Core HWM budget | A | — |  |
| Application job queue | A | — |  |
| DeadlineExceeded | A | — |  |
| TargetNotFound | A | — |  |
| RouteNotConnected | A | — |  |
| Shutdown | A | — |  |
| ChannelName | A | — |  |
| Topic | A | — |  |
| Logical Multicast | A | — |  |
| Subscription | A | — |  |
| Publish target snapshot | A | — |  |
| Relocation policy | A | — |  |
| Preserve-state relocation policy | A | — |  |
| Classic fanout | A | — |  |
| Snapshot | A | — |  |
| Spot application queue | A | — |  |
| Object execution queue | A | — |  |
| Relocation temporary queue | A | — |  |
| Spot control claim | A | — |  |
| Actor queue claim | A | — |  |
| Relocation mode | A | — |  |
| Relocation unit | A | — |  |
| Relocation state chunk | A | — |  |
| In-flight payload budget | A | — |  |
| 재전송 창 (Cutover retransmission window) | A | — |  |
| SafeToShutdown | A | — |  |
| Maintenance wave | A | — |  |
| Drain과 draining | A | — |  |
| Drain deadline | A | — |  |
| Metadata snapshot | A | — |  |
| Membership | A | — |  |
| Channel Client와 Server role | A | — |  |
| Weight | A | — |  |
| Full mesh | A | — |  |
| Peer admission | A | — |  |
| Lifecycle generation | A | — |  |
| Descriptor | A | — |  |
| MeshNode descriptor | A | — |  |
| ClientServer Server descriptor | A | — |  |
| Fanout publisher descriptor | A | — |  |
| Descriptor revision | A | — |  |
| Automatic discovery | A | — |  |
| Manual endpoint | A | — |  |
| Ready target | A | — |  |
| MaxMessageSize | A | — |  |
| Node direct | A | — |  |
| Select-one | A | — |  |
| Handler namespace | A | — |  |
| Message kind | A | — |  |
| Packet name | A | — |  |
| Liveness와 liveness beacon | A | — |  |
| ClientServer Channel | A | — |  |
| Server identity | A | — |  |
| Reply token | A | — |  |
| Downstream request | A | — |  |
| Owner lease | A | — |  |
| Fencing deadline | A | — |  |
| Network listener | A | — |  |
| BindHost | A | — |  |
| AdvertiseHost | A | — |  |
| Wildcard address | A | — |  |
| Advertised endpoint | A | — |  |
| Routing ID | A | — |  |
| Routing ID prefix | A | — |  |
| CSPRNG | A | — |  |
| RoutingIdConflict | A | — |  |
| SpotIdConflict | A | — |  |
| STREAM session | A | — |  |
| Binding token | A | — |  |
| Binding route | C | [Session과 Actor binding 「8. Actor relocation 중 Session의 책임」](../../04-session/02-session-actor-binding.ko.md#8-actor-relocation-중-session의-책임) | relocation 중 route 갱신 절차 문단 삭제, 링크로 대체 (옛 링크는 20-session-actor-dispatch.ko.md였음 — session 주제 완료로 새 경로 사용) |
| Session Actor 위치 갱신 | C | [Session과 Actor binding 「8.2 Control message 42·43·44」](../../04-session/02-session-actor-binding.ko.md#82-control-message-424344) | command 44 세부 절차(timeout·abort) 삭제, 링크로 대체. 옛 링크(20-session-actor-dispatch.ko.md#51-…)를 session 주제 새 경로로 교체 |
| Binding generation | A | — |  |
| AuthorityOwnerGeneration | A | — |  |
| OwnerLeaseGeneration | A | — |  |
| Session sequence | A | — |  |
| Stream Connector | A | — |  |
| Stream packet | A | — |  |
| Dispatch mode | A | — |  |

## 판정 요약

| 갈래 | 개수 |
|---|---|
| A — 순수 정의, 그대로 유지 | 124 |
| B — 정의 + 이 주제 안 문서가 소유한 절차(링크로 대체) | 1 (Cold activation) |
| C — 정의 + 다른 주제 문서가 소유한 절차(링크로 대체) | 4 (User Spot execution mode, Spot relocation coordination mode, Binding route, Session Actor 위치 갱신) |
| 합계 | 129 |

읽어본 결과 129개 항목 대부분이 이미 매핑표 RG1 형식(요약 표 + 정의 문장)을 지키고
있어 절차 문단이 표 밖에 따로 없었다. 실제로 표 밖에 다른 계약 문서가 이미 소유한
절차·조정 순서를 다시 풀어 쓴 항목은 5개였고, 그 5개만 B/C로 표시해 절차 문단을
링크 한 줄로 바꿨다. 나머지는 값의 형태·공개 구성·생성/관리/수명을 설명하는 정의
문장이며 다른 문서와 겹치는 절차 서술이 아니므로 압축하지 않았다(가이드 §3.2, 이
방침에 따라 목표는 "줄이기"가 아니라 "같은 절차가 두 곳에 있는 상태를 없애는 것").

## RG1~RG8 — 새 위치

| RG# | 규칙 | 새 위치 |
|---|---|---|
| RG1 | 값·record 용어는 먼저 요약 표(형태/.NET 표기/공개 구성/생성·관리/수명)를 둔 뒤 필요하면 실제 `.NET` 선언 | "이 용어집이 지키는 규칙" 절, 첫 불릿 |
| RG2 | `.NET 표기`에 "public type 없음"이면 코드 블록은 contract pseudocode이며 실제 API 이름이 아님 | "이 용어집이 지키는 규칙" 절, 둘째 불릿 |
| RG3 | 실제 `.NET` 선언의 단일 기준은 `.NET` exact interface 문서; 용어집 표기는 보조 | "이 용어집이 지키는 규칙" 절, 셋째 불릿 |
| RG4 | 용어집은 11개 주제 절(`## N.`)로 나뉘고 각 항목은 `<a id>` + `### 용어` 고정 형식 | "이 용어집이 지키는 규칙" 절, 넷째 불릿 |
| RG5 | 용어는 같은 이름을 다른 개념에 재사용하지 않음(유일성) | "이 용어집이 지키는 규칙" 절, 다섯째 불릿 |
| RG6 | 용어집은 여러 스펙이 공유하는 정의의 기준이며, 개별 스펙은 첫 사용 자리에서 한 문장 + 링크로만 소개 | "이 용어집이 지키는 규칙" 절, 도입 문장(가이드 §3.2 링크 포함) |
| RG7 | 항목이 다른 계약 문서가 소유한 절차를 다시 서술하면 안 됨 | "이 용어집이 지키는 규칙" 절, 여섯째 불릿 |
| RG8 | 새 용어 추가 조건 4가지 | "이 용어집이 지키는 규칙" 절, 마지막 불릿(가이드 §3.4 링크 포함) |

RG1~RG5·RG7는 기존 "표와 .NET 코드 예제를 읽는 방법" 절 내용을 그대로 유지하는
형태로도 이미 충족되어 있었다("이 용어집이 지키는 규칙" 절은 그 내용을 명시적인
불릿 규칙으로 다시 정리해 §3.0/RG8이 요구한 "가이드 §3.2/§3.4를 서문에 명시" 여부를
확정한 것이다). RG6·RG8은 기존 문서에 없던 내용이라 새로 추가했다.

## 이 문서 밖에서 바뀐 링크 (원본 → 새 경로)

용어집 본문의 외부 참조 10개를 이 주제/다른 주제의 새 경로로 갱신했다. 괄호 안은
링크가 있던 옛 위치(줄 번호는 옛 `01-glossary.ko.md` 기준).

| 옛 링크 | 새 링크 | 비고 |
|---|---|---|
| `00-public-contract-governance.ko.md` (nav, L7) | `01-public-contract-governance.ko.md` | 이 주제 sibling, 번호만 이동 |
| `02-overview.ko.md` (nav, L7) | `03-overview.ko.md` | 이 주제 sibling, 번호만 이동 |
| `languages/dotnet/interfaces/README.ko.md` (L36) | `../languages/dotnet/interfaces/README.ko.md` | 경로 깊이 +1 |
| `../stream-connector/languages/dotnet/03-stream-connector.ko.md` (L37) | `../../stream-connector/languages/dotnet/03-stream-connector.ko.md` | 경로 깊이 +1 |
| `12-spot-messaging.ko.md` (L12) | `../03-spot-actor/02-spot-messaging.ko.md` | spot-actor 주제로 이미 이동됨 |
| `11-spot-model.ko.md` (L107) | `../03-spot-actor/01-spot-model.ko.md` | spot-actor 주제로 이미 이동됨 |
| `27-flow-correlation.ko.md` (L691) | `../06-observability/04-flow-correlation.ko.md` | observability 주제 완료 |
| `05-async-execution-policy.ko.md#13-one-way-submit` (L887) | `../01-execution/01-submit-and-completion.ko.md#4-one-way-submit--admission-경계` | execution 주제로 이동, 절 번호도 13→4로 바뀜 |
| `24-runtime-monitoring.ko.md` (L1253) | `../06-observability/01-runtime-monitoring.ko.md` | observability 주제 완료 |
| `10-network-listener-identity.ko.md#7-시스템-전체-transport-rid와-spot-id-정책` (L1998) | `../02-channel-transport/04-network-listener-identity.ko.md#6-시스템-전체-transport-rid와-spot-id-정책` | channel-transport 주제로 이동, 절 번호도 7→6으로 바뀜 |
| `20-session-actor-dispatch.ko.md#51-session-actor-위치-갱신-message` (L2115, Binding route ack 항목) | `../04-session/02-session-actor-binding.ko.md#82-control-message-424344` | session 주제 완료, 절 구조가 바뀌어 §5.1이 아니라 §8.2로 흡수됨 |

이 10개는 모두 대상 문서가 **이미 존재하는 경로**를 가리키며 `check_doc_links.py`로
검증했다(§ 끝 "검증" 참고). `03-spot-actor`는 이 리뷰 시점에 병렬로 진행 중인 다른
주제라서, 위 두 링크(Spot 모델·Spot 메시징)와 본문의 C 항목 2개(User Spot execution
mode, Spot relocation coordination mode)가 가리키는 `03-spot-actor/01-spot-model.ko.md`가
최종 확정본인지는 그 주제가 리뷰를 통과한 뒤 다시 확인이 필요하다.

## 이동 후 갱신할 링크

캠페인 전체가 en 작성과 함께 파일을 최종 위치로 옮기는 시점(§5)에 다시 확인할 것.

- 이 문서 자체가 `00-foundation/02-glossary.ko.md`로 이미 새 위치에 있으므로, 옛
  `01-glossary.ko.md`를 `../01-glossary.ko.md` 형태로 참조하던 **다른 91개 문서**의
  링크는 최종 이동 시점에 `00-foundation/02-glossary.ko.md`(및 anchor는 변경 없음)로
  일괄 치환해야 한다 — 이 작업은 이번 범위 밖(캠페인 §5).
- `03-spot-actor` 주제가 이번 리뷰 뒤 구조를 바꾸면(예: `01-spot-model.ko.md`의 §5/§5.1
  번호가 바뀌면) 이 문서의 네 링크(L12, L132, L160, L179 부근)를 함께 갱신해야 한다.
- `02-channel-transport`·`01-execution` 주제도 이 리뷰 시점에 병렬 진행 중이므로, 최종
  리뷰 통과 전에 절 번호가 바뀌면 L2023(네트워크 리스너), L912(one-way submit) 링크를
  다시 확인해야 한다.

## spec-gap 후보

이 문서를 다시 읽으며 새로 발견한 spec-gap 후보는 없다. mapping.ko.md의 기존 G-F1~G-F3은
`01-public-contract-governance`·`06-framework-api` 소관이며 이 문서(glossary)와는
무관하다.

## 검증

- `grep -n ' $' 02-glossary.ko.md` → 0건
- `grep -c "정본" 02-glossary.ko.md` → 0
- `python3 doc/site/scripts/check_doc_links.py framework` → `00-foundation` 관련 깨진
  링크 0건 (기존 실패 62건은 모두 `03-spot-actor`·`05-location-relocation`·zoneworld
  샘플 관련이며 이 작업과 무관)
- anchor 대조: 옛 `01-glossary.ko.md`의 `<a id="...">` 127개, `### 용어` 제목 129개와
  새 `02-glossary.ko.md`를 `comm`으로 비교한 결과 양쪽 집합이 **정확히 일치**(추가 0,
  누락 0) — 새 문서가 옛 문서의 superset이라는 요구를 anchor 변경 없이 만족한다.
