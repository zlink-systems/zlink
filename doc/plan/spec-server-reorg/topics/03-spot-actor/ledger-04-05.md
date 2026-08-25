# 재작성 대장 — 04-actor-model · 05-spot-actor-membership

> 옛 `14-actor-model.ko.md`(257 규칙), `15-spot-actor.ko.md`(382 규칙)를
> `03-spot-actor/04-actor-model.ko.md` · `03-spot-actor/05-spot-actor-membership.ko.md`로
> 재작성한 대장이다. [매핑표](mapping.ko.md) §5의 `14-actor-model-R#`,
> `15-spot-actor-R#` id를 그대로 쓴다. 연속된 R#가 같은 새 절로 갔으면 범위로 묶었다.
> 새 위치는 새 문서의 `##`/`###` 절 번호다. 각 범위는 대표 문구를 grep으로 새 문서에서
> 실물 확인한 뒤에만 적었다(가이드 §2.5) — 코디네이터 리뷰에서 5건(R18·R96·R172·R175·R200)을
> 추가로 재확인해 그 결과를 반영했다(아래 "재확인" 참고).
>
> 이 문서가 처리한 구조 문제: **S4, S6, S9, S11, S13, S14**(매핑표 §4, 코디네이터가 이
> 두 문서에 배정). S1·S2·S3·S5·S7·S8·S10·S12·S15·S16·S17·S18·S19·S20은 다른 문서
> (01-spot-model·02-spot-messaging·03-mesh-node·06-spot-address-messaging·08-routing·
> 09-object-lifecycle) 또는 코디네이터의 spec-gap 판정 몫이라 이 재작성에서 임의로
> 처리하지 않았다 — 단, S4·S19의 논리를 04/05 두 문서 사이의 중복(command 44 protocol,
> Actor queue gate/Yield)에 한해 적용했다(§4·§6.4·§6.5·§9·§10의 요약+링크 처리 참고).

## 코드가 검색하는 문장

`framework/languages/cpp/tests/Zlink.Framework.ContractTests/test_cpp_framework_layout_contract.cpp`의
`actor_model_documents_actor_destroy_lifecycle`이 `14-actor-model.ko.md`를 경로로 열어
`std::string::find`로 검색하는 12개 문자열이다. `04-actor-model.ko.md` §6.8 "종료와 destroy"에
줄바꿈 없이 한 물리 줄 안에 그대로 남겼다.

```
"Actor 종료는 새로운 payload admission을 닫고 session binding과 location ownership을"
"Bound session의 연결이 종료되었다는 이유만으로 Actor를 자동 종료하거나"
"현재 Spot에서 자동 leave하지 않는다."
"Actor destroy는 exact `ActorRef`를 받는다. Actor가 user Spot에 있으면 먼저 leave"
"또는 Entry Spot join을 완료해야 한다."
"Destroy는 membership 이동이 아니다."
"성공 과정에서 `OnLeaveActor`를 다시"
"새로운 payload admission을 닫는다."
"Session binding을 제거한다."
"Location ownership과 registry entry를 제거한다."
"Idempotent `false`를 반환한다."
"bound"
```

**grep 확인 완료** — 12개 전부 `04-actor-model.ko.md` §6.8에서 `python3`으로 정확 일치
확인(2026-08-25). 코디네이터 재확인 요청에서도 재확인 통과. 이 테스트는 현재도
`14-actor-model.ko.md` 경로를 하드코딩하므로, 옮기는 단계(README §5)에서 경로와 필요하면
needle을 함께 갱신해야 한다(캠페인 README §7이 이미 인지).

## 규칙 등가성 대장

## 14-actor-model.ko.md → 04-actor-model.ko.md

| 규칙 ID | 새 위치 | 비고 |
|---|---|---|
| 14-actor-model-R1–14-actor-model-R4 | §1 Actor 모델 개요 |  |
| 14-actor-model-R5–14-actor-model-R27 | §2 Actor identity와 상태 |  |
| 14-actor-model-R28–14-actor-model-R35 | §3 Actor queue |  |
| 14-actor-model-R36–14-actor-model-R83 | §3.1 Deferred Join barrier |  |
| 14-actor-model-R84–14-actor-model-R112 | §4 Spot이 처리하는 Actor control |  |
| 14-actor-model-R113–14-actor-model-R117 | §5 Actor 메시징 |  |
| 14-actor-model-R118–14-actor-model-R126 | §5.1 Route cache와 generation |  |
| 14-actor-model-R127–14-actor-model-R138 | §5.2 Handler 선택 |  |
| 14-actor-model-R139–14-actor-model-R157 | §6.1 Factory와 relocation policy 등록 |  |
| 14-actor-model-R158–14-actor-model-R164 | §6.2 Create와 GetOrCreate 입력 |  |
| 14-actor-model-R165–14-actor-model-R167 | §6.3 Mesh와 placement target 선택 |  |
| 14-actor-model-R168–14-actor-model-R179 | §6.4(요약+링크) Creation request와 factory 실행 → 05§2가 소유 | 옛 §6.4 상세(reservation·factory 실행 안전성·OnCreateActor 승인/거절/예외)는 04에서 3~4문장 요약 + 링크로 축소(S18 정신 반영). 전체 규칙 본문은 05§2가 소유(내용은 15§2에서 이미 가지고 있던 것과 동일 — 새로 만든 문장 없음) |
| 14-actor-model-R180–14-actor-model-R201 | §6.5(요약+링크) Create/GetOrCreate 상태·terminal record → 05§2가 소유 | 옛 §6.5 상세(Create/GetOrCreate 상태표, terminal record 5분 보존, DeadlineExceeded 비실패 의미)는 04에서 요약 + 링크로 축소. 본문은 05§2가 소유. R200(DeadlineExceeded는 생성 attempt 실패로 간주하지 않음)은 원래 15§2 서술에 없던 Actor 고유 뉘앙스라 05§2에 문장을 추가해 보존(원문 14 §6.5 그대로, 새 주장 없음) |
| 14-actor-model-R202–14-actor-model-R203 | §6.6 Find |  |
| 14-actor-model-R204–14-actor-model-R205 | §6.7 Spot 이동 |  |
| 14-actor-model-R206–14-actor-model-R216 | §6.8 종료와 destroy |  |
| 14-actor-model-R217–14-actor-model-R221 | §7 Session binding |  |
| 14-actor-model-R222–14-actor-model-R230 | §8.1 실패 |  |
| 14-actor-model-R231–14-actor-model-R232 | §8.2 관측 정보 |  |
| 14-actor-model-R233–14-actor-model-R257 | §9 구현 및 contract test 검증 요구 |  |

## 15-spot-actor.ko.md → 05-spot-actor-membership.ko.md

| 규칙 ID | 새 위치 | 비고 |
|---|---|---|
| 15-spot-actor-R1–15-spot-actor-R3 | §0 서두(개요) |  |
| 15-spot-actor-R4–15-spot-actor-R16 | §1 Identity와 authority |  |
| 15-spot-actor-R17–15-spot-actor-R64 | §2 Object를 하나만 생성하도록 확정하는 과정 |  |
| 15-spot-actor-R65–15-spot-actor-R78 | §3 Entry Spot과 User Spot의 Actor membership(gate 배정은 04§3 링크로 축소) | gate 배정(SpotWide/PerActor/Entry의 execution gate 구분, Yield 허용 범위)은 이미 04§3이 소유한 규칙과 동일 문장이라 04§3 링크로 축소(S19). 04§3 자체는 이 매핑표의 담당 범위 밖 문서지만 이미 옛 14§3에 그대로 있던 문장이라 새로 만든 것 없음 |
| 15-spot-actor-R79–15-spot-actor-R85 | §3(Join Defer 등록 규칙은 04§3.1이 이미 소유, 링크로 축소) | Join Defer() 등록 개수·크기·timeout 제한은 04§3.1이 이미 소유한 문장과 동일하여 링크로 축소(S19) |
| 15-spot-actor-R86–15-spot-actor-R94 | §3(고유 유지 — 제어 queue 직렬화, Instance Spot 제외, disconnected callback, Close 거부) |  |
| 15-spot-actor-R95–15-spot-actor-R145 | §4 Actor join과 commit 순서 |  |
| 15-spot-actor-R146–15-spot-actor-R154 | §4.1 Entry Spot과 User Spot의 callback 비교 |  |
| 15-spot-actor-R155–15-spot-actor-R231 | §4.2 다른 node의 Spot으로 Actor를 Join하는 순서(8-step, command 44 protocol·relay-ready invariant의 유일한 서술 자리) | relay-ready reply invariant(§4 diagram 앞 353-354행, diagram 뒤 566-575/582-593행)를 §4.2 안에서 한 번만 굵게 서술하고 나머지는 '위 규칙 참고'로 축소 |
| 15-spot-actor-R232–15-spot-actor-R247 | §5 Spot 종료와 lifecycle callback(4.2에서 분리한 절) |  |
| 15-spot-actor-R248–15-spot-actor-R269 | §6 모든 이동 경로가 공유하는 relocation policy |  |
| 15-spot-actor-R270–15-spot-actor-R307 | §7 User Spot과 member Actor를 함께 이동하는 maintenance aggregate |  |
| 15-spot-actor-R308–15-spot-actor-R325 | §8 실패 처리 범위 |  |
| 15-spot-actor-R326–15-spot-actor-R331 | §9 Message Follow |  |
| 15-spot-actor-R332–15-spot-actor-R343 | §10(요약+링크) Bound Session → 세션-owner 쪽은 session/02 §8이 소유 | 세션-owner 쪽 seal 검증(current Session identity·binding generation·relocation identity만 검증, AuthorityOwnerGeneration 재검증 안 함, late/duplicate는 Warning만)은 04-session/02-session-actor-binding.ko.md §8.1·§8.2가 이미 동일 내용으로 소유(session 주제 판정 완료) — 05§10은 그 문서로 링크만 하고 본문을 반복하지 않음(가이드 §4.2) |
| 15-spot-actor-R344–15-spot-actor-R382 | §11 구현 및 contract test 검증 요구 |  |

## 코디네이터 재확인 5건 (R18·R96·R172·R175·R200)

| R# | 결과 | 내용 |
|---|---|---|
| 14-actor-model-R18 | **오탐(false alarm)** | "이전 `ActorRef`는 stale할 수 있다"가 `04-actor-model.ko.md` §2.2(60행)에 원문 그대로 있다. 최초 자동 검사 스크립트가 백틱을 포함하지 않은 패턴으로 검색해 놓쳤을 뿐 본문은 항상 있었다. |
| 14-actor-model-R96 | **오탐(false alarm)** | "owner 점유 상한만으로는 lane 사이 굶주림을 막지 못한다 — 같은 우선순위 규칙이 lifecycle을 다시 고르기 때문"이 `04-actor-model.ko.md` §4(224-226행)에 원문 그대로 있다("이것만으로는 lane 사이의 굶주림을 막지 못한다 — 이 owner에 turn이 돌아왔을 때 두 lane이 여전히 ready이면 같은 우선순위 규칙이 lifecycle을 다시 고르기 때문이다"). 이유 절까지 포함해 완전하다. |
| 14-actor-model-R172 | **다른 표현으로 존재 — 대장에 기록** | "Factory는 같은 (ActorId, ObjectGeneration, creation attempt)에 대해 한 번 이상 실행될 수 있다; 따라서 같은 attempt 재실행에도 안전해야 한다"는 `05-spot-actor-membership.ko.md` §2(121행)에 "Factory와 initialize는 `(logical key, ObjectGeneration, attempt)` 기준으로 한 번 이상 실행될 수 있으므로 같은 입력의 재실행을 안전하게 처리해야 한다"로 있다. `ActorId`→`logical key`, "같은 attempt 재실행"→"같은 입력의 재실행"으로 표현이 다르지만 이 문장은 **옛 `15-spot-actor.ko.md` §2 원문 그대로**(코디네이터가 이미 05§2를 15§2의 단일 소유로 확정했으므로 새로 만든 표현이 아니다). 04§6.4는 이 절 전체를 05§2로 위임하는 요약+링크만 갖는다. |
| 14-actor-model-R175 | **다른 표현으로 존재(두 문장에 분산) — 대장에 기록** | "Accepted이면 initial Entry membership과 Ready authority를 commit하고 Created를 publish; 이 최초 생성 과정에서는 OnActorJoin과 joined notification을 호출하지 않는다"는 `05-spot-actor-membership.ko.md` §2에 두 문장으로 나뉘어 있다 — "승인하면 initial Entry membership·Ready authority·active capacity와 `Created` terminal record를 함께 공개한다"(126행)와 "Actor creation은 initial Entry Spot membership과 Ready barrier를 같은 lifecycle에서 완료하며 `OnActorJoin`과 `OnJoinedActor`를 호출하지 않는다"(169-170행). "publish"→"공개한다", "joined notification"→"`OnJoinedActor`"로 표현이 다르지만 둘 다 **옛 `15-spot-actor.ko.md` §2 원문**이며 04는 링크만 한다. |
| 14-actor-model-R200 | **원래 누락 — 수정 완료** | "Creating 상태를 재확인하던 caller가 deadline에 도달하면 DeadlineExceeded로 끝나지만 생성 attempt가 실패했다고 간주하지 않는다"는 옛 `14-actor-model.ko.md` §6.5의 고유 문장으로, 옛 `15-spot-actor.ko.md` §2에는 이 "실패로 간주하지 않는다" 뉘앙스가 없었다. 04§6.5를 05§2 링크로 축소하면서 이 문장이 어느 문서에도 남지 않게 됐던 것을 발견해 `05-spot-actor-membership.ko.md` §2(136-139행)에 원문 그대로("생성 attempt 자체가 실패했다고 간주하지는 않는다") 추가했다. 새 주장을 만들지 않고 14 원문 문장을 그대로 옮겼다. grep 재확인 완료. |

12개 needle 문자열도 이 수정 뒤 다시 `python3` 정확 일치로 재확인했다 — 전부 `04-actor-model.ko.md`에
그대로 있다(변경 없음, §6.8은 이번 수정과 무관한 절이다).

## 나중에 anchor를 붙일 링크

병렬로 작성 중인 03-spot-actor 형제 문서는 파일만 링크하고 anchor를 붙이지 않았다(지침).
그 문서들이 완성되면 아래 자리에 정확한 절 anchor를 채워야 한다.

| 이 문서의 자리 | 가리키는 파일 | 필요한 anchor(추정) |
|---|---|---|
| `04-actor-model.ko.md` §3 | `03-mesh-node.ko.md` | 없음(파일 링크만, "MeshNode route와 peer admission" 전체를 가리킴) |
| `04-actor-model.ko.md` §4 부채 표 앞 | `02-spot-messaging.ko.md` | `§5.4`(execution mode) — 02가 그 번호로 쓰는지 확인 필요 |
| `04-actor-model.ko.md` §6.4·§6.5 | `05-spot-actor-membership.ko.md#2-object를-하나만-생성하도록-확정하는-과정` | 이미 anchor 있음(같은 주제 안 완성 문서라 anchor 확정) — 05 문서 절 번호가 나중에 바뀌면 갱신 |
| `05-spot-actor-membership.ko.md` §1 | `01-spot-model.ko.md` | 아직 링크 안 함(01은 이 두 문서에서 직접 인용하지 않음) — 만약 리뷰에서 §3 서두가 01을 링크해야 한다고 판단되면 anchor 없이 파일만 추가 |
| `05-spot-actor-membership.ko.md` §4.2 (2단계) | `../51-internal-service-wire-protocol.ko.md` (§9) | 이미 anchor 있음(옛 경로, 02-channel-transport 주제가 아직 안 옮겼으므로 유효) |

## 이동 후 갱신할 링크

이 두 문서는 아직 다른 주제로 옮기지 않은 문서를 옛 flat 경로(`../NN-slug.ko.md`)로 링크한다.
해당 주제가 재작성·이동을 마치면 아래 경로를 새 경로로 바꿔야 한다(README §5).

| 링크 대상 | 현재 경로(이 문서에서 사용) | 소속 주제 | 상태 |
|---|---|---|---|
| 메시지 모델 | `../04-message-model.ko.md` | 01-execution | 작성 중 |
| 비동기 실행 정책 | `../05-async-execution-policy.ko.md` | 01-execution | 작성 중 |
| Framework API | `../06-framework-api.ko.md` | 00-foundation | 작성 중 |
| Actor와 Spot relocation 전체 흐름(28) | `../28-relocation-flow.ko.md` | 05-location-relocation | 매핑표 작성 중 |
| Graceful drain과 handoff(30) | `../30-host-relocation-flow.ko.md` | 05-location-relocation | 매핑표 작성 중 |
| 51 내부 service wire protocol | `../51-internal-service-wire-protocol.ko.md` | 02-channel-transport | 작성 중 |
| .NET Actor interface | `../languages/dotnet/interfaces/06-actors.ko.md` | languages(그대로 유지) | 경로 불변 예상 — 확인 필요 |

session 관련 링크(`../04-session/02-session-actor-binding.ko.md`)는 session 주제가 이미 완료됐고
파일 경로가 최종 형태이므로 이동 갱신 대상이 아니다.

## 최상위 README가 인용하는 절 (S6·S5 관련)

- **owner 점유 상한**(옛 `14-actor-model.ko.md` §3, 새 `04-actor-model.ko.md` §4)와
  **lifecycle 연속 실행 상한**을 최상위 `spec/server/README.ko.md`가 "여러 장이 연결되는
  구조 결정" 절에서 인용한다. 절 번호가 §3 → §4로 바뀌었으므로 최상위 README 갱신은
  이동 단계(§5)에서 코디네이터가 처리해야 한다. `04-actor-model.ko.md` §4 끝에 이 사실을
  적은 안내문을 남겨 두었다.

## spec-gap 후보

- **S20 (매핑표 §4, 이 두 문서에 남아 있음, 판정은 코디네이터 몫)** — Actor 생성 상태
  다이어그램의 세 번째 leaf 라벨이 두 옛 문서에서 달랐다(`14-actor-model.ko.md`는 `Failed`,
  `15-spot-actor.ko.md`는 `Aborted`). 이번 재작성에서 `05-spot-actor-membership.ko.md` §2가
  생성 reservation 절차의 단일 소유자가 되면서(가이드 §4.2에 따른 중복 제거) 다이어그램은
  `15-spot-actor.ko.md` 원문의 `Aborted` 라벨만 남는다. `04-actor-model.ko.md` §6.5는 이제
  독자적인 다이어그램을 갖지 않고 05§2로 링크만 하므로, 두 표기가 나란히 보이던 이전의
  "불일치가 보이는" 상태는 사라졌다 — 그러나 이것은 표기를 임의로 통일한 것이 아니라 이미
  코디네이터가 확정한 소유권 이전(§6.4·§6.5 요약+링크)의 부수 효과다. 실제 구현이
  `Failed`/`Aborted`를 어떻게 구분하는지는 여전히 확인되지 않았으므로, spec-gap 대장에는
  "Aborted가 단일 표기로 남았지만 실제 4언어 구현과 대조해 `Failed`라는 이름을 쓰는 곳이 있는지
  확인 필요"로 등록해야 한다.
