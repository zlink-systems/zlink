# spec/server 재구성 — spot-actor 주제 매핑표

> 캠페인: `framework/doc/framework/common/spec/server`를 주제별로 재구성하고
> [스펙 문서 작성 가이드](../../../../principal/documentation/spec-writing-guide.ko.md)대로 다시 쓴다.
> 이 문서는 `spot-actor` 주제의 작업 계획이다. 양식은 파일럿
> [04-session/mapping.ko.md](../04-session/mapping.ko.md)를 따른다.

관련: [spec-gap 대장](../../spec-gap.ko.md) · [주제 구분 초안](../../topic-map.ko.md) ·
[전체 목차 초안](../../target-readme.ko.md)

## 1. 대상과 규모

| 현재 문서 | 줄 수 | 성격 | 외부 anchor 링크 수 |
|---|---|---|---|
| `11-spot-model` | 581 | 계약 — Entry·User·Instance Spot의 공통점과 차이점, lifecycle callback | 14 |
| `12-spot-messaging` | 1,037 | 계약 — Spot direct, Channel 범위 Logical Multicast, Subscription, queue 규칙 | 12 |
| `13-mesh-node` | 423 | 계약 — MeshNode identity, object role, placement capability, startup·peer admission | 1 |
| `14-actor-model` | 716 | 계약 — Actor identity, queue, control, lifecycle(Create/GetOrCreate/Find/destroy) | 8 |
| `15-spot-actor` | 929 | 계약 — Spot·Actor membership, Actor join/commit 순서, relocation policy | 14 |
| `16-spot-address-messaging` | 524 | 계약 — Spot identity/reference, User Spot Create/GetOrCreate, route cache, close | 1 |
| `17-stage-wrapper-on-spot` | 178 | 계약 — Spot 위에 room·stage 상위 실행 모델을 얹는 경계 | 6 |
| `18-object-routing` | 443 | 계약 — Global ID routing, bound-session relay, reply route | 8 |
| `45-internal-routing-and-cache` | 426 | 구현 스펙 — target 선택 절차, route cache 수명, 성능 절벽 | 1 |
| `47-internal-object-lifecycle` | 332 | 구현 스펙 — 객체 종류 구분, cold activation, 유휴 정리, memory 회계 | 2 |

합계 ko 5,589줄, 10개 문서 중 8개 계약 + 2개 구현 스펙. 외부에서 이 열 개 문서로 들어오는
markdown anchor 링크는 총 **67개**(문서별 위 표), 링크를 포함해 이 열 개 문서 경로를 참조하는
markdown 파일은 중복 제외 약 **150개 이상**(문서마다 8~66개, `15-spot-actor`가 가장 많다).

### 코드에서 이 문서를 경로로 여는 곳

session 파일럿보다 훨씬 많다. 아래는 markdown이 아닌 코드·스크립트·config가 이 열 개
문서를 **경로 문자열로 열어** 존재를 확인하거나 문장을 needle로 검색하는 곳이다.

| 문서 | 여는 곳 | 방식 |
|---|---|---|
| `11-spot-model.ko.md` | `framework/languages/dotnet/tests/Zlink.Framework.SampleRegressionTests/Regression.cs:454` | 경로로 열어 `"Entry Spot은 close operation 대신 Actor destroy"` 문장을 needle로 검색 |
| `12-spot-messaging.ko.md` | `scripts/verify-framework-instance-spot-contracts.sh` (formalFixtures) | 문장 7개를 needle로 검색(§6 표) |
| `12-spot-messaging.ko.md` | `scripts/verify-framework-submit-api.sh` | 문장 2개(`Publish 완료는 handler 실행 결과가 아니라 local outbound admission`, `monitoring snapshot, metric 또는 runtime event로 제공하지 않는다`)를 needle로 검색 |
| `13-mesh-node.ko.md` | `framework/languages/java/.../JavaDocumentationRegressionTest.java:37` | 파일 존재만 확인(`Files.isRegularFile`), 문장 needle 없음 |
| `14-actor-model.ko.md` | `framework/languages/cpp/tests/Zlink.Framework.ContractTests/test_cpp_framework_layout_contract.cpp` `actor_model_documents_actor_destroy_lifecycle` | **문장 12개**를 needle로 검색(§1.1에 전문) — 이번 작업이 다루는 유일한 cpp layout contract test 대상 |
| `14-actor-model.ko.md` | `framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Documentation/Regression.cs:788` | 파일 존재만 확인 |
| `15-spot-actor.ko.md` | `scripts/verify-framework-instance-spot-contracts.sh` (formalFixtures) | 문장 7개를 needle로 검색(§6 표) |
| `15-spot-actor.ko.md` | `framework/runtime/conformance/relocation-behavior-v1.json`, `validate-runtime-conformance-fixtures.mjs:587` | `contractSources`에 `path` + `sections: ['4.2','5','6','7','8']`로 절 번호를 명시 참조 |
| `15-spot-actor.ko.md` | `framework/languages/node/test/contract/documentation-regression.test.js:281` | 파일 존재만 확인 |
| `15-spot-actor.ko.md` | `framework/languages/cpp/cross-language/cross_language_host.cpp:710`, `run_cross_language_smoke.sh:973` | 주석에서 절 번호·줄 번호(`:489`, `:938`)를 지정 |
| `15-spot-actor.ko.md` | `framework/languages/dotnet/cross-language/.../Program.cs:138`, java `EntryRelocationContracts.java:6`, `RelocationUserSpot.java:16`, `UserSpotJoinContracts.java:5` | 주석에서 절 번호·줄 번호(`:489`) 지정 |
| `16-spot-address-messaging.ko.md` | `scripts/verify-framework-instance-spot-contracts.sh` (formalFixtures) | 문장 9개를 needle로 검색(§6 표) |

**결론**: `12`·`15`·`16`은 markdown 링크뿐 아니라 **shell 스크립트가 매 CI에서 문장 needle로
검사**한다(`scripts/verify-framework-instance-spot-contracts.sh`). `14`는 cpp layout contract test가
문장 12개를 검사한다. `15`는 추가로 conformance fixture가 **절 번호**(`4.2`, `5`, `6`, `7`, `8`)를,
cpp/dotnet/java cross-language 파일 4곳이 **줄 번호**(`:489`, `:938`)를 주석에 박아 두고 있다 —
문서를 옮기고 절을 다시 쓰면 이 절 번호·줄 번호 참조도 함께 갱신해야 한다. 이 목록은 이동
단계(§5, 마지막 단계)의 입력이며, 이번 ko 재작성 단계에서는 **문장을 그대로 보존**하는 것으로
충분하다.

### 1.1 cpp contract test needle 12개 (14-actor-model.ko.md, 재작성 후에도 그대로 유지해야 함)

`test_cpp_framework_layout_contract.cpp`의 `actor_model_documents_actor_destroy_lifecycle`가
`14-actor-model.ko.md` 전체 본문(§6.8 "종료와 destroy" 부근)에서 아래 12개 문자열을
`std::string::find`로 검색한다. 이번 재작성에서 이 12개 문자열은 **줄바꿈 없이 한 물리 줄
안에** 그대로 남아 있어야 한다(주석에 "prose wraps at column width … 한 물리 줄 안에 있게
재고정했다"고 적혀 있다 — 즉 과거에 이 needle이 줄바꿈에 걸려 깨진 전례가 있다).

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

(2026-08-25 확인 — 12개 전부 현재 `14-actor-model.ko.md`에서 grep으로 실물 확인됨.)

참고로 `06-framework-api.ko.md`(foundation 주제)도 같은 cpp 파일의
`framework_api_documents_actor_destroy_lifecycle`에서 문장 4개가 검사되지만, 그 문서는
이 주제 소관이 아니다 — foundation 주제 작업자에게 인계할 사항으로만 기록한다.

## 2. 독자 질문 — 주제 README가 답할 것

가이드 §1의 질문표를 spot-actor 주제에 맞게 채운 것. 새 문서의 절은 이 질문 순서를 따른다.

| 질문 | 답이 있어야 할 자리 |
|---|---|
| Spot이 무엇이고 Entry·User·Instance는 무엇이 같고 다른가 | README 개요 + `spot-model` §2·§3 |
| MeshNode는 무엇이고 object는 어떤 조건에서 그 위에 배치되는가 | `mesh-node` 역할·placement capability |
| 메시지를 Spot으로 보내면 실제로 어떤 경로를 거쳐 handler까지 가는가 | `spot-messaging` Spot direct·Channel 범위 Logical Multicast |
| Spot 이름으로 새 Instance Spot을 언제 만드는가 | `spot-messaging` §3.2, `spot-address-messaging` §4 |
| Actor는 무엇이고 어떻게 identity·queue·control을 갖는가 | `actor-model` §2·§3·§4 |
| Actor를 새로 만들거나 이미 있는 걸 찾으려면 무엇을 호출하는가 | `actor-model` §6 |
| 같은 객체를 여러 caller가 동시에 만들려 하면 무엇이 이기는가 | `spot-actor-membership` §2, `object-lifecycle` §3 |
| Actor가 Spot에 join하는 순서는 무엇이고 다른 node면 무엇이 다른가 | `spot-actor-membership` §4 |
| global SpotId·ActorId로 보낸 message는 현재 owner를 어떻게 찾는가 | `routing` §2 |
| Session에 bind된 Actor로 보낸 message는 어떤 경로를 쓰는가 | `routing` §3 |
| 이동(relocation) 중에는 message가 어디로 가는가 | `spot-actor-membership` §5·§6, `routing` §2.4 |
| Actor·Spot이 이동한 뒤에도 이전 경로로 온 message는 어떻게 되는가 | `routing` §2.4, `object-lifecycle` §4 |
| 실행 중인 객체를 언제 정리하고 무엇으로 재사용을 막는가 | `object-lifecycle` §5·§6 |
| 매 message마다 위치를 다시 조회하는가, 캐시를 쓰는가 | `routing`(45 병합) §"target 선택과 route cache" |
| 실패하면 무엇이 남는가(`NotFound`, `Unavailable`, `InvalidOperation` …) | 각 문서 실패·관측 절 |
| Spot 위에 room·stage 같은 상위 모델을 얹으려면 무엇을 지켜야 하는가 | `stage-wrapper-on-spot` |

## 3. 새 구조

`target-readme.ko.md` §"03-spot-actor"가 이미 슬러그를 정해 두었다(§3.1의 번호 조정 제외).
이 매핑표는 그 계획을 그대로 쓴다.

```
spec/server/03-spot-actor/
  README.ko.md                      주제 진입 1장
  01-spot-model.ko.md               11 재작성
  02-spot-messaging.ko.md           12 재작성
  03-mesh-node.ko.md                13 재작성
  04-actor-model.ko.md              14 재작성
  05-spot-actor-membership.ko.md    15 재작성 (파일명에 "membership"을 남긴 이유는 §3.2)
  06-spot-address-messaging.ko.md   16 재작성
  07-stage-wrapper-on-spot.ko.md    17 재작성
  08-routing.ko.md                  18 + 45 병합 재작성
  09-object-lifecycle.ko.md         47 재작성
```

(en 짝 문서는 동일 번호·슬러그로 마지막 단계에서 함께 작성한다.)

### 3.0 target-readme.ko.md 초안의 번호 겹침 — 수정 없이 기록만

`target-readme.ko.md`의 03-spot-actor 표(126~143행)는 `08. routing`과 `08. object-lifecycle`을
**같은 번호 08로** 적어 두었다(45가 08-routing에 병합되는 것과 별개로, 47이 새 번호를 못 받은
채 08로 남았다). 이 매핑표는 그 오기를 **09로 바로잡아** 위 구조에 반영했다. `target-readme.ko.md`
자체는 이번 작업에서 수정하지 않는다(건드릴 파일 범위 밖) — 코디네이터가 해당 문서를 갱신할
때 참고할 사항으로만 남긴다.

### 3.1 18-object-routing + 45-internal-routing-and-cache 병합 판정

**전문을 대조한 결과, 45는 두 계약 소유 문서에 걸쳐 있다 — 45 자신이 그렇게 밝히고
있다.** 45의 머리말(9~15행)이 이미 선언한다.

> **계약 소유** — 선택 순서와 tiebreak는 [Channel 메시징](08-channel-messaging.ko.md)이,
> cache 수명과 무효화 조건은 [Spot·Actor routing](18-object-routing.ko.md)이 소유한다.

실제로 45의 절을 나눠 보면 이 선언과 정확히 일치한다.

| 45의 절 | 다루는 내용 | 계약을 소유하는 문서 |
|---|---|---|
| §1, §1.1 (route cache가 필요한 이유, resolver 결과 타입 `ReadyRoute`/`Missing`/`Unavailable`/`StoreFailure`, cache 수명 상한) | Positive route cache 자체 | `18` §2.2 (최근 Ready route 재사용 조건) — **이 주제 소관** |
| §2 (relocation과 cache가 만나는 성능 절벽, relay 통지로 캐시 무효화) | 이동 뒤 우회 경로와 캐시 갱신 | `18` §2.4 (이전 owner route에 도착한 message) — **이 주제 소관** |
| §3 (후보 목록을 호출마다 만들지 않음) | Channel select-one 후보 캐시 | `08-channel-messaging` §3.2 — **02-channel-transport 주제 소관** |
| §4 (선택을 어느 계층이 하는가 — Core vs framework) | MeshNode·ClientServer·수동 fallback 대상 선택 계층 | `08-channel-messaging`, `07-channel-topology` — **02-channel-transport 주제 소관** |
| §5 (smooth weighted round-robin 절차, 결정적 tiebreak) | Channel select-one의 선택 알고리즘 | `08-channel-messaging` §선택 순서, `09-client-server-channel` §5 — **02-channel-transport 주제 소관** |
| §6 (직접 지정한 대상은 바꾸지 않음) | 대상 직접 지정 호출 일반 규칙 | 공통 규칙이나 예시가 Channel 대상 선택 — **02-channel-transport 주제 소관** |
| §7 (여러 대상에게 함께 보낼 때 — 발행 target 고정, node당 1 record, 부분 실패 미복구) | Logical Multicast/publish fanout | `12-spot-messaging` §4(발행)과 `08-channel-messaging` — **이 주제(spot-messaging)와 02-channel-transport 양쪽에 걸침** |
| §8 확인할 결과 | 위 각 절의 검증 요구가 섞여 있음 | 절 분할과 같이 나뉜다 |

**결론 — 45 전체를 18로 병합하는 계획은 절반만 맞다.** `45` §1·§1.1·§2(대략 90줄)만
"위치 조회를 message마다 하지 않는다"는 `18`의 계약(§2.1 조회 순서, §2.2 캐시 재사용
조건, §2.4 이전 owner route 처리)을 구현하는 내용이고, `18` 병합이 정확히 맞다 — `18`의
각 계약 절 뒤에 그 절을 구현하는 45의 절을 이어 붙이는 방식이 적합하다(가이드 §4.4).
`45` §3~§7(대략 260줄, 45의 60% 이상)은 **Channel 메시징의 대상 선택 계약을 구현하는
내용이며 08-object-routing과는 별개 주제(02-channel-transport)**다. §7의 발행 fanout은
그중에서도 `12-spot-messaging`(이 주제) §4가 소유하는 Logical Multicast 완료 계약과
`08-channel-messaging`의 fanout이 함께 걸린 자리라 더 세밀한 판단이 필요하다.

**이번 주제에서 하는 일**: `45` §1·§1.1·§2만 `08-routing.ko.md`로 병합한다. §3~§6은
이번 재작성에서 **손대지 않고 원 위치(45)에 그대로 둔다** — 02-channel-transport 주제가
그 문서를 처리할 때 08-channel-messaging으로 병합할지 판단한다(§4 S11, §8 작업 순서).
§7(발행 fanout)의 "발행 결과는 대상별로 돌려주지 않는다" 부분은 `12-spot-messaging`의
기존 §4.4·§4.6이 이미 같은 계약을 갖고 있어 실질적 중복이다(§4 S12) — 이 주제
재작성에서는 `12-spot-messaging`이 그 계약을 소유하도록 확정하고 `45` §7에는 링크만
남긴다. `45`가 완전히 빈 문서가 될 때까지는(§3~§6이 다른 주제로 옮겨질 때까지는) `45`
파일 자체를 삭제하지 않는다 — 파일 삭제·이동은 마지막 단계(§5)의 몫이다.

`45`의 `### 문제` / `### 결정` 소제목(§1, §2, §4, §5)과 `47`의 유사 구조는 가이드 §2.4가
금지하는 "결정" 라벨 그대로다 — 08-routing으로 옮기는 §1·§1.1·§2 부분은 재작성 시 굵은
규칙 문장 + 이유 불릿으로 바꾼다(§4 S-항목).

**47은 그대로 독립 문서로 남긴다.** 계약 소유 문서가 따로 없는 독립 구현 스펙(객체 종류
구분·활성화·정리·memory 회계)이라 병합 대상이 아니다 — `09-object-lifecycle.ko.md`로
재작성한다. topic-map.ko.md의 원래 계획과 일치.

### 3.2 파일명에 "membership"을 남기는 이유

`target-readme.ko.md` 145~147행이 이미 적어 둔 이유를 그대로 따른다 — 번호만 떼는 기계적
규칙을 적용하면 `spot-actor/spot-actor.ko.md`가 되어 디렉터리 이름과 겹쳐 읽힌다. 이 문서만
원 제목("Spot과 Actor membership")을 살려 `05-spot-actor-membership.ko.md`로 정한다.

### 3.3 제안하는 읽기 순서 (주제 README)

1. `spot-model` — Spot 세 종류가 무엇인지 먼저 안다.
2. `spot-messaging` — 그 Spot에 메시지가 어떻게 도달하는지 안다(direct·multicast·subscription).
3. `mesh-node` — 메시지가 타는 물리 layer(RID, role, placement)를 안다.
4. `actor-model` — Spot 위에 사는 Actor의 identity·queue·lifecycle을 안다.
5. `spot-actor-membership` — Actor가 Spot에 join·commit되는 정확한 순서와 relocation policy.
6. `spot-address-messaging` — global SpotId로 User/Instance Spot을 만들고 부르는 방법.
7. `stage-wrapper-on-spot` — Spot 계약 위에 상위 모델을 얹는 경계(짧고 응용적이므로 뒤로).
8. `routing` — 지금까지 나온 모든 대상(Spot·Actor·session-bound Actor)에 실제로 message를
   보낼 때 어떤 route를 쓰고 언제 위치를 다시 조회하는지 하나로 모은다(계약+구현).
9. `object-lifecycle` — 구현자 전용: 객체 종류를 코드에서 어떻게 구분하고 언제 만들고
   정리하는지(구현 스펙, 마지막에 둔다).

## 4. 읽으면서 발견한 구조 문제 (재작성에서 처리)

10개 문서를 전문 대조한 결과다. session 파일럿과 같은 유형(중복 서술·문단 벽·메타
제목)이 훨씬 큰 규모로 나타난다 — 특히 `12-spot-messaging`과 `16-spot-address-messaging`은
같은 cold activation 절차를 두 벌로, `11-spot-model`과 `13-mesh-node`는 Entry Spot ID
발급 규칙을 두 벌로 갖고 있다.

| # | 문제 | 처리 |
|---|---|---|
| S1 | **Cold activation 전체 절차(11단계)가 `12-spot-messaging` §3.2(mermaid + 산문 + 8단계 목록, 세 벌)와 `16-spot-address-messaging` §4(mermaid + 11단계 목록)에 거의 같은 내용으로 중복**된다. `16`이 command 번호(39)·route kind(1/2)·오류 코드별 분기까지 더 정밀하다. | `06-spot-address-messaging.ko.md`가 전체 절차(diagram 1개 포함)를 단독 소유. `02-spot-messaging.ko.md` §3.2는 "Instance Spot이 없을 때"를 개념 수준 3~4문장 + 그 문서로 링크로 축소 |
| S2 | **Entry Spot ID 발급·형식·충돌 규칙이 `11-spot-model` §4.1과 `13-mesh-node` §3.2에 거의 같은 문장으로 중복**되고, 두 문서의 "검증 요구" 절까지 같은 문장을 반복한다. `11`의 문서 경계표(§8)는 이미 "Object role, Entry Spot과 factory 등록은 13이 소유"라고 적어 두었지만 `11` §4.1 본문이 그 경계를 어기고 있다. | `03-mesh-node.ko.md`가 Entry Spot ID 규칙을 단독 소유(자신이 이미 선언한 경계와 일치시킴). `01-spot-model.ko.md` §4는 "Framework가 발급한다" 한 문장 + 링크로 축소 |
| S3 | **Positive route cache·owner resolve 규칙이 `12-spot-messaging` §3.1과 `16-spot-address-messaging` §5에 중복**되고, 다시 `18-object-routing` §2.1~§2.2에도 나온다(이 규칙의 최종 소유 문서는 `18`이어야 한다 — 병합 후 `08-routing.ko.md`). | `02-spot-messaging` §3.1은 "owner를 못 찾으면 cache→Store 순으로 조회한다"는 한 문단 개념 설명만 남기고 정확한 필드·수명·무효화 조건은 `08-routing.ko.md`로 링크. `06-spot-address-messaging` §5도 같은 방식으로 축소 |
| S4 | **command 44 `sessionRelocationRoute` 프로토콜(Session owner ↔ target ↔ Actor)이 `11-spot-model` §4.2·§5.2(SpotWide)·§5.2(PerActor)에서 세 번, `15-spot-actor` §4.2·§6에서 또 서술**된다. 세 번 모두 거의 같은 문장이 반복되고 매번 산문으로만 쓰여 있어(3주체 이상, node 경계를 넘음) 가이드 §7.2가 요구하는 sequence diagram이 하나도 없다. | 이 프로토콜의 계약(적용 range, seal, timeout, 응답 없음)은 `04-actor-model.ko.md`(Actor 쪽 route 갱신) 또는 `05-spot-actor-membership.ko.md`(Actor join/relocation을 이미 소유)가 한 곳에서 sequence diagram과 함께 소유하고, 나머지 두 자리는 "SpotWide는 Spot과 member Actor를 하나의 aggregate로 묶어 같은 절차를 따른다" 한 문장 + 링크로 축소. Session 쪽 수신 규칙은 이미 `04-session/02-session-actor-binding.ko.md` §8이 소유(session 주제 판정 완료) — 이 주제는 그 문서를 링크만 한다 |
| S5 | **큐 포화 결과표가 `12-spot-messaging` §5.3에 있고, spec/server 현재 README가 이미 이 절을 "여러 장이 연결되는 구조 결정 — 대기열 포화 시 공개 결과"의 기준 문서로 지정**해 두었다(다른 주제가 이 절을 링크해 들어온다). | `02-spot-messaging.ko.md`가 계속 이 표를 단독 소유. 재작성 시 절 번호가 바뀌므로 **주제 README 작성 뒤 최상위 README(마지막 단계)의 해당 링크도 함께 갱신**해야 한다는 사실을 §6에 기록 |
| S6 | **owner 점유 상한(Actor queue 연속 실행 상한)도 최상위 README가 `14-actor-model` §3을 기준 문서로 지정**하고 있다 — S5와 같은 유형 | `04-actor-model.ko.md`가 계속 단독 소유. 절 번호 변경을 §6에 기록해 마지막 단계에 넘김 |
| S7 | **45의 §3~§7(대상 선택 알고리즘, smooth weighted round-robin, 후보 캐시)은 이 주제가 아니라 02-channel-transport 주제(08-channel-messaging)가 계약을 소유**한다(§3.1에서 판정). 지금 45 안에 있는 채 남는다 | 이 주제의 재작성에서는 `45` §1·§1.1·§2만 옮기고 §3~§7은 손대지 않음. 02-channel-transport 주제 작업자에게 인계(§8) |
| S8 | **`### 문제` / `### 결정` 소제목**이 `45` §1·§2·§4·§5, `47` §1·§3·§4·§5·§6 전역에 반복된다. 가이드 §2.4가 금지하는 "결정" 라벨 패턴 그대로다. | 재작성 시 굵은 규칙 문장 + 이유 불릿으로 바꾸고 소제목은 주제 이름으로(예: "### 문제" → 없애고 본문에 흡수, "### 결정"→ 규칙 문장이 곧 그 절의 첫 문장) |
| S9 | **메타 절 제목 "이 문서가 정의하는 범위"**가 `13-mesh-node` §1, `16-spot-address-messaging` §1, `14-actor-model` §1, `15-spot-actor`의 서문, `17-stage-wrapper-on-spot` §1, `18-object-routing`의 서문에 반복된다. `11-spot-model`만 예외적으로 "## 1. 범위"를 쓴다 — 10개 문서 안에서도 일관되지 않다. | 전부 그 주제 이름으로(예: "Spot 모델 개요", "MeshNode identity 개요") 통일. 블록쿼트 "이 장이 정의하는 것"과 §1 서문이 거의 같은 문장을 두 번 반복하는 자리도 함께 정리 |
| S10 | **`16-spot-address-messaging` §4 원문에 번호 중복 결함**이 있다(267행과 280행이 모두 "8."). 이 결함을 그대로 옮기면 재작성본에도 같은 혼란이 생긴다. | 재작성 시 8→9로 바로잡아 순서를 다시 매긴다. 규칙 내용 자체는 원문과 동일하게 유지(가이드 §2.5 — 번호 정정은 규칙 추가가 아니다) |
| S11 | **표가 독립된 규칙 여러 개를 한 셀에 압축**하는 사례가 전 문서에 반복된다 — `11-spot-model` §3의 15행 비교표(예: "Relocation" 행 하나에 세 Spot 종류의 서로 다른 규칙 3개), `13-mesh-node` §4의 Object role 3×3 표, `12-spot-messaging` §5.3의 큐 포화 표. | 가이드 §7.1 "각 셀에 주어·조건·결과"를 지키도록 셀을 문장 단위로 쪼갠다. 옵션·필드 설명 표는 §8.3에 따라 선언 옆 인라인 주석으로(예: 코드 예시에 등장하는 `RouteCacheMaxAge`, `InstanceSpotIdleTimeout` 같은 상수) |
| S12 | **Logical Multicast 발행(publish) 완료 계약이 `12-spot-messaging` §4.4·§4.6과 `45-internal-routing-and-cache` §7에 중복**된다(대상 목록 고정, 일부 실패 미복구, 결과값 없이 완료, target별 결과 미집계 — 거의 같은 문장). | `02-spot-messaging.ko.md` §4가 단독 소유(Application이 관찰하는 완료 계약이므로 계약 문서가 우선). `45`(→ 02-channel-transport로 인계되기 전까지는 원 위치) §7에는 링크만 남기도록 인계 메모에 포함 |
| S13 | **3주체 이상 순서인데 diagram이 없는 자리**가 다수 — cold activation 재작성 후 남을 유일한 diagram 외에, `13-mesh-node` §7.1 peer handshake/중복 connection 해소(두 MeshNode + automatic/manual 판정 + 07-channel-topology 참조), `47` §3 동시 생성 경쟁(caller A·B·Store·factory 4주체 — 이미 mermaid가 있어 예외), command 44 프로토콜(S4). | 각 소유 문서 재작성 시 가이드 §7.2에 따라 sequence diagram 추가. 물리 layer(연결)와 논리 layer(주소 선택)는 분리해서 그린다 |
| S14 | **한자어·격식체 표현**: "유휴"(11 전역, "유휴 Instance Spot"), "게시하다"(16 전역, "descriptor를 게시한다" — "공개한다"로 대체 가능), "적체"(12 §5.3, "업무 적체"). "정본" 급의 심각한 사례는 없었으나 누적되면 원칙 7.3 위반. | 재작성 시 쉬운 말로 교체("유휴"→"쓰지 않고 남아 있는", "게시"→"공개"·"알림", "적체"→"밀림") |
| S15 | **`47`의 마지막 절 "## Object queue와 host shared capacity"가 번호 없이** 있다(326행) — 다른 모든 절은 번호가 있는데 이 절만 예외. 내용도 01-execution 주제(46-internal-dispatch-loop, 50-internal-message-ownership)로 넘어가는 인계 메모에 가깝다. | `09-object-lifecycle.ko.md`에서 번호를 붙이거나(예: "## 8. 다른 주제와의 경계"), 검증 요구 절 앞의 짧은 문단으로 흡수 |
| S16 | **`15-spot-actor` §4.2가 220줄짜리 단일 절**(420~641행)에 8단계 흐름, mermaid sequence diagram, rollback 경계 재서술 3벌, 그리고 주제상 어울리지 않는 `OnClosing`/`ClosingContext`(617~640행, Spot 종료 lifecycle) 내용까지 얹혀 있다. | 재작성 시 §4.2는 join 순서만 남기고, `OnClosing`/`ClosingContext`는 별도 절(예: "종료와 lifecycle callback")로 분리 |
| S17 | **"relay-ready reply가 accepted 상태가 되기 전 실패 → 그 뒤에는 cutover 결과와 무관하게 source를 복원하지 않는다"는 불변조건이 `15-spot-actor` 안에서만 최소 6번** 거의 같은 문장으로 반복된다(353-354, 480-482, 556-558, 566-575, 582-593, 789-791; aggregate판은 753-756). | 이 불변조건을 한 곳(§4.2 8단계 표 근처)에서 규칙 문장으로 한 번만 서술하고, 나머지 5곳은 "이 규칙은 모든 이동 경로에 공통 적용된다"는 문장 + 링크로 축소(§4 S4와 같은 패턴) |
| S18 | **Actor 생성/reservation 상태 기계(Missing→Reserved→Created/Rejected/…)가 `14-actor-model` §6.4-6.5와 `15-spot-actor` §2에 문장 단위로 중복**된다 — "Location Store는 (source Node RID, source lifecycle generation, OperationId)로 식별한 operation terminal record를 원래 deadline 뒤 5분까지 유지한다" 같은 문장이 두 문서에 그대로 나온다. | `05-spot-actor-membership.ko.md`(15)가 생성 reservation 절차를 단독 소유. `04-actor-model.ko.md`(14) §6.4-6.5는 개념 3~4문장 + 링크로 축소 |
| S19 | **Actor queue gate/Yield 규칙(SpotWide 공통 gate, PerActor 개별 lane, Entry Spot Actor별 gate)이 `14-actor-model` §3과 `15-spot-actor` §3에 거의 같은 문장으로 중복**된다. | `04-actor-model.ko.md` §3(Actor queue)가 단독 소유. `05-spot-actor-membership.ko.md` §3은 "membership 상태에 따라 이 gate 규칙이 적용된다" 한 문장 + 링크로 축소 |
| S20 | **`14`와 `15`의 생성 상태 다이어그램이 실패 leaf 이름을 다르게 쓴다** — `14-actor-model` 539~549행 다이어그램은 세 번째 leaf를 `Failed`로, `15-spot-actor` 146~152행 다이어그램은 같은 자리를 `Aborted`로 표기한다. `15` 자신의 산문(155~156행)은 "Callback exception은 Failed, recovery cleanup은 terminal record를 만들지 않는 Abort"라고 **Failed와 Abort를 서로 다른 두 결과로 구분**하므로, `15`의 다이어그램 leaf 라벨 `Aborted(R1, Failure)` 자체가 자기 문서의 산문과도 정합적이지 않다. | 이것은 문서 조직 문제가 아니라 **실제 불일치 가능성**이 있어 재작성에서 임의로 통일하지 않는다 — `## spec-gap 후보`에 등록하고 실제 구현(4언어)이 Failed와 Aborted를 어떻게 구분하는지 대조한 뒤 판정 |


## 5. 규칙 등가성 대장

10개 문서 전문을 대조해 뽑은 규칙·수치·상태·오류 대장이다. 문서별로 grouping하고 원문 절 안에서
매긴 번호를 `<문서번호>-R<n>`로 유지했다(예: `11-R42`). 총 **1,882행**이다. 재작성 뒤
"새 위치" 열을 채워 누락 0·추가 보장 0을 증명한다 — 이 표에 있다고 적힌 것만으로 배치를
완료로 보지 않는다(§3.3 리뷰 절차, README §4.3).

표기: `[표 N]`은 원문의 표 셀 하나를 한 규칙으로 뽑았다는 뜻. `(검증)`/`(§N확인)`은 원문 검증
요구 절의 항목. Negative rule("~하지 않는다")도 그대로 포함했다 — 재작성에서 빠지기 가장 쉬운
규칙 종류다.

### 11-spot-model

| # | 규칙 (수치·상태·오류) | 옛 위치 | 새 위치 |
|---|---|---|---|
| 11-spot-model-R1 | 문서 범위: Entry Spot, User Spot, Instance Spot의 공통점과 차이점을 정의; 세 종류 모두 Spot이지만 생성 목적, Actor membership, 종료·relocation 계약이 다름 | 14-17 | |
| 11-spot-model-R2 | Message 전달 방법 소유 문서는 12-spot-messaging | 20 | |
| 11-spot-model-R3 | Actor callback 정확한 순서 소유 문서는 15-spot-actor | 21 | |
| 11-spot-model-R4 | User·Instance Spot의 생성과 주소 계약 소유 문서는 16-spot-address-messaging | 22-23 | |
| 11-spot-model-R5 | Entry Spot은 Object Server startup과 함께 준비된다 (등록한 Entry Spot 초기화) | 27-29,34 | |
| 11-spot-model-R6 | User Spot은 application이 manager로 명시적으로 만든다 (Create 또는 GetOrCreate) | 30,34-35 | |
| 11-spot-model-R7 | Instance Spot은 별도 create operation 없이 Instance intent를 가진 첫 message로, 대상이 Missing일 때 준비된다 | 31,35-36 | |
| 11-spot-model-R8 | [표 3] Entry Spot 주된 목적: 해당 Object Server에 배치된 Actor의 초기·기본 membership 관리 | 42 | |
| 11-spot-model-R9 | [표 3] User Spot 주된 목적: application이 명시적으로 만드는 Spot이며 Actor membership 관리 가능 | 42 | |
| 11-spot-model-R10 | [표 3] Instance Spot 주된 목적: Actor 없이 direct message와 timer 처리 | 42 | |
| 11-spot-model-R11 | [표 3] Entry Spot 등록: Object Server builder에 Spot 구현 type 등록, startup에서 초기화 | 43 | |
| 11-spot-model-R12 | [표 3] User Spot 등록: Stable type factory 등록, manager Create·GetOrCreate로 생성 | 43 | |
| 11-spot-model-R13 | [표 3] Instance Spot 등록: Stable type factory 등록, Instance intent를 가진 첫 direct call로 준비 | 43 | |
| 11-spot-model-R14 | [표 3] Entry Spot ID: Framework가 발급, caller가 fixed Spot ID 지정 불가 | 44 | |
| 11-spot-model-R15 | [표 3] User Spot ID: Create는 Framework 발급, GetOrCreate는 caller 지정 | 44 | |
| 11-spot-model-R16 | [표 3] Instance Spot ID: caller가 direct message의 target Spot ID 지정 | 44 | |
| 11-spot-model-R17 | [표 3] Entry Spot: 별도 stable-type 문자열 등록 안 함 | 45 | |
| 11-spot-model-R18 | [표 3] User Spot: UTF-8 1..255 byte stable type 필수 | 45 | |
| 11-spot-model-R19 | [표 3] Instance Spot: UTF-8 1..255 byte stable type 사용; Missing activation에서는 명시하거나 유일한 등록 type 선택 | 45 | |
| 11-spot-model-R20 | [표 3] Entry Spot Actor membership 지원: Actor 생성의 initial membership이며 JoinEntrySpot의 대상 | 46 | |
| 11-spot-model-R21 | [표 3] User Spot Actor membership 지원: Actor가 JoinSpot과 leave로 membership 변경 가능 | 46 | |
| 11-spot-model-R22 | [표 3] Instance Spot: Actor membership 지원하지 않음 | 46 | |
| 11-spot-model-R23 | [표 3] 세 Spot 모두 Direct packet 지원 | 47 | |
| 11-spot-model-R24 | [표 3] 세 Spot 모두 Timer와 outbound call 지원 | 48 | |
| 11-spot-model-R25 | [표 3] Entry Spot 기본 실행: Spot handler·timer는 Spot turn에서 직렬화, Actor는 Actor별 실행 | 49 | |
| 11-spot-model-R26 | [표 3] User Spot 기본 실행: SpotWide — Spot·member Actor·timer·lifecycle callback 전체 직렬화 | 49 | |
| 11-spot-model-R27 | [표 3] Instance Spot 기본 실행: Direct handler와 timer를 Spot 전체에서 직렬화 | 49 | |
| 11-spot-model-R28 | [표 3] Entry Spot, Instance Spot: Optional 실행 방식 제공하지 않음 | 50 | |
| 11-spot-model-R29 | [표 3] User Spot Optional 실행: PerActor — Actor별, Spot lane별, timer별로 직렬화, 서로 다른 lane은 동시 실행 가능 | 50 | |
| 11-spot-model-R30 | [표 3] Entry Spot Relocation 경계: Actor별 current turn 경계 사용 | 51 | |
| 11-spot-model-R31 | [표 3] User Spot Relocation 경계: SpotWide는 기본 임의의 안전한 turn 경계(선택적으로 application이 알린 경계), PerActor는 Actor별 current turn 경계 | 51 | |
| 11-spot-model-R32 | [표 3] Instance Spot Relocation 경계: 현재 Spot turn 경계 사용 | 51 | |
| 11-spot-model-R33 | [표 3] Entry Spot: Yield 지원하지 않음 | 52 | |
| 11-spot-model-R34 | [표 3] User Spot: Yield는 SpotWide에서만 지원, PerActor에서는 지원하지 않음 | 52 | |
| 11-spot-model-R35 | [표 3] Instance Spot: Yield 지원함 | 52 | |
| 11-spot-model-R36 | [표 3] Entry Spot, User Spot: Logical Multicast subscription 지원 | 53 | |
| 11-spot-model-R37 | [표 3] Instance Spot: Logical Multicast subscription 지원하지 않음 | 53 | |
| 11-spot-model-R38 | [표 3] Entry Spot: 명시적 close operation을 context와 manager에 제공하지 않음 | 54 | |
| 11-spot-model-R39 | [표 3] User Spot 명시적 close: exact SpotRef를 manager Close에 전달하거나 local context에서 close | 54 | |
| 11-spot-model-R40 | [표 3] Instance Spot 명시적 close: 자신의 handler나 timer context에서 close | 54 | |
| 11-spot-model-R41 | [표 3] Entry Spot Relocation: Entry Spot 자체는 이동하지 않음, Actor를 독립된 relocation unit으로 옮김 | 55 | |
| 11-spot-model-R42 | [표 3] User Spot Relocation: SpotWide는 Spot·member Actor 전체를 한 번에, PerActor는 Spot state는 옮기지 않고 Actor를 독립적으로 옮김 | 55 | |
| 11-spot-model-R43 | [표 3] Instance Spot Relocation: Actor가 없는 Spot 하나를 relocation unit으로 이동 | 55 | |
| 11-spot-model-R44 | [표 3] 세 Spot 모두 Host shutdown: Accepted turn 정리 후 HostShutdown reason으로 OnClosing 호출, 같은 shutdown closing 계약 적용 | 56 | |
| 11-spot-model-R45 | [표 3] .NET 구현 type: Entry Spot=IZLinkEntrySpot(또는 IZLinkEntrySpot<TActor>), User Spot=IZLinkSpot(또는 IZLinkSpot<TActor>), Instance Spot=IZLinkInstanceSpot | 57 | |
| 11-spot-model-R46 | Host relocation 공통 단계·sequence diagram은 30-host-relocation-flow §8이 정의 | 59-61 | |
| 11-spot-model-R47 | 세 Spot에 전달된 direct packet과 timer callback은 Spot application queue에 넣음 | 63-65 | |
| 11-spot-model-R48 | Actor에 전달된 업무 payload는 Spot queue를 거치지 않고 해당 Actor의 queue에 바로 넣음 | 66-67 | |
| 11-spot-model-R49 | Target runtime이 Restore 요청을 받으면 다음 packet dispatch 전 relocation temporary queue를 등록함 | 72-73 | |
| 11-spot-model-R50 | Dispatch 순서: (1)Object 종류·ID·ObjectGeneration 검사 (2)같은 RelocationId·target attempt의 temporary queue 확인 (3)있으면 temporary queue에 넣음 (4)없으면 기존 lookup·execution queue 경로 사용 | 76-79 | |
| 11-spot-model-R51 | Temporary queue에는 source ingress hold에서 relay한 message와 owner 변경 전후 도착 message가 함께 들어갈 수 있음 | 81-82 | |
| 11-spot-model-R52 | Target은 temporary queue의 application payload를 실행하지 않음 | 82 | |
| 11-spot-model-R53 | Actor·Spot 생성, state Restore, owner 변경, lifecycle callback이 끝난 뒤 실제 execution queue로 옮김 | 83-84 | |
| 11-spot-model-R54 | 순서: Restored work → Temporary queue work → New direct work | 86-92 | |
| 11-spot-model-R55 | 전환은 dispatch와 atomic하게 처리 | 94 | |
| 11-spot-model-R56 | 전환 전 temporary queue가 수락한 message를 실제 queue에 모두 넣은 뒤 temporary queue 등록 제거 | 94-95 | |
| 11-spot-model-R57 | 동시에 들어온 message는 temporary queue와 실제 queue 중 정확히 한 곳에만 들어감 | 95-96 | |
| 11-spot-model-R58 | 실제 queue는 전환이 끝나기 전에 application handler를 실행하지 않음 | 96-97 | |
| 11-spot-model-R59 | 저장했다가 복원한 기존 작업은 temporary queue의 message보다 먼저 처리 | 99 | |
| 11-spot-model-R60 | Temporary queue 안에서는 target dispatcher가 message를 수락한 순서 유지 | 99-100 | |
| 11-spot-model-R61 | 서로 다른 network route에서 동시에 들어온 message 사이에 별도의 전역 순서를 만들지 않음 | 100-101 | |
| 11-spot-model-R62 | SpotWide User Spot relocation: Spot과 모든 member Actor를 같은 relocation group으로 등록 | 103-104 | |
| 11-spot-model-R63 | Temporary queue의 각 record는 실제 target Spot 또는 Actor identity를 보존 | 104 | |
| 11-spot-model-R64 | 복원 후 Spot message는 Spot queue, Actor message는 해당 Actor queue로 나눠 넣으며 각 target 안의 수신 순서를 유지 | 105-107 | |
| 11-spot-model-R65 | PerActor에서는 Spot과 Actor relocation을 독립적으로 등록하여 이동하지 않는 Actor의 기존 dispatch를 막지 않음 | 106-107 | |
| 11-spot-model-R66 | 같은 Restore 요청을 다시 받으면 기존 temporary queue와 Restore 진행 상태를 사용 | 109-110 | |
| 11-spot-model-R67 | 이전 target attempt나 다른 ObjectGeneration의 queue에는 message를 넣지 않음 | 110-111 | |
| 11-spot-model-R68 | Relay-ready reply가 accepted 상태가 되기 전 명시적 abort에서만 target temporary queue를 실행하지 않고 폐기하며 source가 보관한 작업을 원래 queue로 되돌림 | 111-112 | |
| 11-spot-model-R69 | 그 뒤에는 cutover submit 결과와 관계없이 source를 복원하지 않음 | 112-113 | |
| 11-spot-model-R70 | Owner commit 뒤에는 같은 target process가 실행 중일 때만 temporary queue를 실제 queue로 옮김 | 113 | |
| 11-spot-model-R71 | Target process가 종료되면 다른 runtime이 이 작업을 자동으로 이어받지 않음 | 114 | |
| 11-spot-model-R72 | Queue는 작업이 기다리는 위치, Execution mode는 서로 다른 queue의 작업을 동시에 실행할 수 있는지를 정함 | 116-118 | |
| 11-spot-model-R73 | User Spot SpotWide mode 구조: Direct packet + Timer callback → Spot queue; Actor A/B payload → 각 Actor queue; 모두 SpotWide gate를 거쳐 One callback | 120-133 | |
| 11-spot-model-R74 | Spot queue와 Actor queue는 서로 분리, Actor payload가 Spot queue를 경유하거나 합쳐지지 않음 | 135-137 | |
| 11-spot-model-R75 | 모든 queue가 하나의 공통 execution gate를 사용하므로 같은 User Spot에서는 Spot handler·timer callback·member Actor handler 중 하나만 실행 | 137-138 | |
| 11-spot-model-R76 | Entry Spot은 Spot 작업과 Actor별 작업의 실행 범위를 분리; Instance Spot은 Actor membership 미지원이므로 Actor queue 없음 | 140-142 | |
| 11-spot-model-R77 | Entry Spot과 PerActor User Spot은 relocation에서도 같은 Actor 단위 모델 사용 | 144 | |
| 11-spot-model-R78 | Spot instance는 실행 shell이며 relocation 후 유지할 application state를 소유하지 않음 | 145-146 | |
| 11-spot-model-R79 | Actor state, Actor queue, Actor timer만 Actor 단위로 이전함 | 146-147 | |
| 11-spot-model-R80 | 여러 Actor가 공유해야 하는 state는 application이 node 밖 저장소(Redis, database, 별도 state service)에서 관리 | 147-148 | |
| 11-spot-model-R81 | Spot handler는 Spot activation scope, Actor handler는 Actor activation scope에서 각각 한 번 만들어 재사용 | 150-151 | |
| 11-spot-model-R82 | Entry Spot과 PerActor User Spot의 서로 다른 Actor는 handler instance나 scoped dependency를 공유하지 않음 | 151-152 | |
| 11-spot-model-R83 | 정확한 생성·정리·relocation 규칙은 06-framework-api §8.2가 정의 | 152-154 | |
| 11-spot-model-R84 | SpotWide User Spot은 이 제한을 받지 않음: Spot과 member Actor가 하나의 relocation aggregate이므로 Spot field·timer를 Spot relocation adapter로 함께 이전 가능 | 156-158 | |
| 11-spot-model-R85 | .NET 표기 사용, 다른 언어는 이름·비동기 표현이 다를 수 있으나 호출 조건·순서는 같음 | 162-164 | |
| 11-spot-model-R86 | Configure는 비동기 lifecycle callback이 아니라 handler를 등록하는 구성 단계 | 163-165 | |
| 11-spot-model-R87 | [표 3.2] Configure: 세 Spot 모두 O — 해당 Spot instance가 사용할 handler를 등록 | 169 | |
| 11-spot-model-R88 | [표 3.2] OnCreateAsync: Entry X, User O, Instance X — Manager가 새 User Spot을 만들 때 creation request 확인, 생성 수락 여부와 optional reply 반환; 기존 User Spot을 찾은 Existing 결과에서는 호출하지 않음 | 170 | |
| 11-spot-model-R89 | [표 3.2] OnInitializeAsync: 세 Spot 모두 O — 생성된 Spot instance의 application 초기화 완료; Instance Spot은 OnCreateAsync 없이 이 callback 사용 | 171 | |
| 11-spot-model-R90 | [표 3.2] OnClosingAsync: 세 Spot 모두 O — 아직 유효한 local Spot instance가 종료되기 전 application resource 정리; 호출 조건은 §3.4에서 구분 | 172 | |
| 11-spot-model-R91 | [표 3.2] OnActorJoinAsync: Entry X, User O¹, Instance X — 이미 존재하는 Actor가 User Spot으로 이동하려 할 때 target User Spot이 요청 승인·거부; Entry Spot 복귀는 기본 membership이므로 admission callback 사용 안 함 | 173 | |
| 11-spot-model-R92 | [표 3.2] OnJoinedActorAsync: Entry O¹, User O¹, Instance X — 일반 join의 membership commit 완료를 target Spot에 알림; Actor 최초 생성과 maintenance 복원에서는 호출하지 않음 | 174 | |
| 11-spot-model-R93 | [표 3.2] OnLeaveActorAsync: Entry O¹, User O¹, Instance X — membership commit 뒤 Actor가 빠져나간 source Spot에 알림; Actor 소멸을 뜻하지 않음 | 175 | |
| 11-spot-model-R94 | [표 3.2] OnDisconnectActorAsync: Entry O¹, User O¹, Instance X — 해당 Spot에 속한 Actor의 연결 단절을 알림 | 176 | |
| 11-spot-model-R95 | [표 3.2] OnCreateActorAsync: Entry O¹, User X, Instance X — 새 Actor의 initial Entry Spot membership 승인·거절, optional reply 반환; 일반 join callback과 구분 | 177 | |
| 11-spot-model-R96 | ¹표시 callback은 Actor type을 지정해 Actor membership을 지원하는 Entry Spot 또는 User Spot에만 적용 | 179-180 | |
| 11-spot-model-R97 | Entry Spot과 User Spot은 서로 다른 Spot instance이며 같은 Actor membership interface를 구현해도 callback은 이동 전 Spot과 이동 후 Spot에서 각각 실행 | 184-186 | |
| 11-spot-model-R98 | User Spot으로 보내는 join에서는 target User Spot이 OnActorJoinAsync로 이동을 승인 | 187-188 | |
| 11-spot-model-R99 | Entry Spot 복귀는 별도 admission 없이 membership을 commit | 188-189 | |
| 11-spot-model-R100 | 두 경우 모두 commit 뒤 target의 OnJoinedActorAsync와 source의 OnLeaveActorAsync를 실행 | 189-190 | |
| 11-spot-model-R101 | User Spot에 있던 Actor가 Entry Spot으로 돌아가더라도 Entry Spot의 OnCreateActorAsync와 OnActorJoinAsync를 호출하지 않음 | 190-192 | |
| 11-spot-model-R102 | 양방향 callback 비교와 정확한 commit 순서는 15-spot-actor §4가 정의 | 192-195 | |
| 11-spot-model-R103 | OnClosingAsync는 Actor별 callback이 아니라 Spot instance의 terminal lifecycle callback | 199-200 | |
| 11-spot-model-R104 | Framework는 OnClosingAsync 실행 시 종료 이유와 absolute deadline을 전달 | 200-201 | |
| 11-spot-model-R105 | [표 3.4] ExplicitClose: Entry X, User O, Instance O — Application이 User·Instance Spot close를 시작하고 해당 local instance를 정상 정리할 때 호출 | 205 | |
| 11-spot-model-R106 | [표 3.4] HostShutdown: 세 Spot 모두 O — Relocation 없이 host가 local Spot을 정리할 때 호출 | 206 | |
| 11-spot-model-R107 | [표 3.4] RelocationOut: Entry X, User O, Instance O — User·Instance Spot owner를 target으로 commit한 뒤 source local instance를 정리할 때 호출 | 207 | |
| 11-spot-model-R108 | [표 3.4] IdleEvicted: Entry X, User X, Instance O — Instance Spot이 유휴 기준을 넘겨 local instance를 내릴 때 호출 | 208 | |
| 11-spot-model-R109 | User Spot에 Actor membership이 남아 있어 explicit close가 false로 끝나면 OnClosingAsync를 호출하지 않음 | 210-211 | |
| 11-spot-model-R110 | Standalone Actor만 다른 Entry Spot으로 이동하는 작업도 Entry Spot instance를 닫지 않으므로 Entry Spot의 OnClosingAsync를 호출하지 않음 | 211-213 | |
| 11-spot-model-R111 | Host shutdown에서는 Actor membership과 local Spot instance가 아직 유효한 상태에서 callback 실행, callback 끝난 뒤 scope와 authority 정리 | 213-215 | |
| 11-spot-model-R112 | Entry Spot은 Object Server role을 가진 MeshNode에 등록 | 220 | |
| 11-spot-model-R113 | Framework는 startup에서 Entry Spot ID를 발급하고 instance를 초기화 | 220-221 | |
| 11-spot-model-R114 | Initialization이 끝나기 전에는 descriptor와 resolver에 Entry Spot을 게시하지 않음 | 221-222 | |
| 11-spot-model-R115 | Entry Spot ID 형식: `<prefix>-entry-<lowercase-canonical-uuid-v4>` (MeshNode diagnostic prefix + Entry Spot 전용 marker) | 224-225 | |
| 11-spot-model-R116 | MeshNode와 Entry Spot은 각각 별도의 UUID v4를 생성하며 두 UUID 값 비교로 관계를 판정하지 않음 | 225-227 | |
| 11-spot-model-R117 | 같은 MeshNode lifecycle에서는 RID를 유지하고 replacement lifecycle에서는 endpoint가 같아도 새 RID를 발급 | 226-227 | |
| 11-spot-model-R118 | Location Store가 global Spot ID active conflict를 보고하면 새 UUID·reservation을 만들지 않고 startup을 즉시 startup configuration error로 끝냄 | 229-230 | |
| 11-spot-model-R119 | MeshNode descriptor는 lifecycle generation과 exact Entry Spot ID의 mapping을 게시 | 230-231 | |
| 11-spot-model-R120 | Actor placement와 Entry Spot join은 이 mapping을 사용하며 Spot ID 문자열을 parsing하지 않음 | 231-232 | |
| 11-spot-model-R121 | Actor를 새로 만들면 Framework가 선택한 owner MeshNode의 Entry Spot이 initial membership을 처리 | 234-235 | |
| 11-spot-model-R122 | Actor 생성과 initial Entry Spot membership은 같은 Ready barrier 안에서 완료 | 235-236 | |
| 11-spot-model-R123 | Actor가 Entry Spot에 속하더라도 업무 message는 Entry Spot callback을 경유하지 않고 Actor queue로 전달 | 236-238 | |
| 11-spot-model-R124 | [표 4.2] 새 Actor의 initial membership: Target Entry Spot은 OnCreateActorAsync로 승인·거절→승인 시 membership·Ready commit; Source 없음 | 246 | |
| 11-spot-model-R125 | [표 4.2] Application이 요청한 일반 JoinEntrySpot: Target은 Admission callback 없이 membership commit→OnJoinedActorAsync; Source는 commit 뒤 OnLeaveActorAsync (source Entry Spot 또는 User Spot) | 247 | |
| 11-spot-model-R126 | [표 4.2] Host maintenance의 standalone Actor relocation: Target·Source 모두 Application membership callback을 호출하지 않음 | 248 | |
| 11-spot-model-R127 | OnCreateActorAsync는 새 Actor를 처음 Entry Spot에 배치할 때만 사용하며 생성 승인 여부와 optional reply를 반환 | 250-251 | |
| 11-spot-model-R128 | 거절하면 staging Actor와 reservation을 정리하고 Ready로 공개하지 않음 | 251-252 | |
| 11-spot-model-R129 | 이미 존재하는 Actor가 User Spot에서 돌아오거나 다른 Entry Spot에서 application join으로 이동하는 경우 OnCreateActorAsync와 OnActorJoinAsync를 호출하지 않음 | 252-254 | |
| 11-spot-model-R130 | Host Relocate가 standalone Actor를 다른 node의 Entry Spot으로 옮기는 작업은 application이 요청한 membership 변경이 아님 | 256-257 | |
| 11-spot-model-R131 | Framework는 target에서 Actor state를 복원하고 owner·membership을 commit하지만 target의 OnJoinedActorAsync나 source의 OnLeaveActorAsync를 호출하지 않음 | 257-260 | |
| 11-spot-model-R132 | Relocation 전용 application callback도 제공하지 않음 | 260 | |
| 11-spot-model-R133 | Target에서 복원 후 Target Actor가 queue 병합, regular route 전환과 lifecycle을 끝낸 뒤 message 처리 시작 | 262-264 | |
| 11-spot-model-R134 | Actor가 Session에 bind되어 있으면 target runtime은 command 44 sessionRelocationRoute commit을 one-way로 보내 binding route를 target owner로 갱신 | 264-266 | |
| 11-spot-model-R135 | Route switch와 함께 bound-session accessor가 반환하는 current Actor location snapshot도 같은 ActorId·ObjectGeneration을 유지한 채 target MeshName·NodeRid로 갱신 | 266-269 | |
| 11-spot-model-R136 | 같은 Session에 bind된 relocation 대상에 포함되지 않은 다른 Actor의 route와 physical STREAM connection은 바꾸지 않음 | 269-270 | |
| 11-spot-model-R137 | Session owner는 exact Session·binding·Actor generation과 relocation identity를 확인하고 held message를 target route로 제출한 뒤 matching seal을 해제 | 270-272 | |
| 11-spot-model-R138 | 적용 reply는 없으며 exact command 44가 SessionRelocationSealTimeout 안에 없으면 physical Session과 관련 state를 정리 | 272-273 | |
| 11-spot-model-R139 | Target Actor는 command 44 적용 reply 없이 message를 처리하며 이전 route의 message는 Message Follow route가 전달 | 273-274 | |
| 11-spot-model-R140 | Route 갱신은 같은 ObjectGeneration에만 적용 | 274-275 | |
| 11-spot-model-R141 | Application은 relocation을 알기 위해 rebind하지 않으며, 새 incarnation은 application이 명시적으로 다시 bind해야 함 | 275-276 | |
| 11-spot-model-R142 | Application은 relocation 사실을 Entry Spot lifecycle callback으로 추적하지 않음 | 278 | |
| 11-spot-model-R143 | Entry Spot은 해당 Object Server lifecycle에 속하므로 relocation participant가 아님 | 282-283 | |
| 11-spot-model-R144 | Host Relocate에서는 Entry Spot에 속한 Actor를 target node의 Entry Spot으로 옮기지만 source Entry Spot instance 자체를 옮기지 않음 | 283-285 | |
| 11-spot-model-R145 | Target Entry Spot은 target Object Server startup에서 Framework가 새 RID와 lifecycle로 준비 | 285-286 | |
| 11-spot-model-R146 | Standalone Actor 이동은 Entry Spot을 닫는 작업이 아니므로 Entry Spot의 OnClosing을 호출하지 않음 | 287-289 | |
| 11-spot-model-R147 | Host가 relocation 없이 shutdown될 때는 accepted handler와 timer turn을 정리한 뒤 local Entry Spot에 HostShutdown closing context를 전달 | 288-290 | |
| 11-spot-model-R148 | User Spot은 application이 stable type의 factory를 등록하고 manager를 사용해 명시적으로 만듦 | 294-295 | |
| 11-spot-model-R149 | Create는 caller가 stable type을 지정하고 Framework가 global Spot ID를 만듦 | 297 | |
| 11-spot-model-R150 | GetOrCreate는 caller가 global Spot ID와 stable type을 모두 지정 | 298 | |
| 11-spot-model-R151 | Actor membership을 지원하는 User Spot은 join·joined·leave·disconnect control을 자신의 Spot queue에서 다른 callback과 직렬화 | 299-300 | |
| 11-spot-model-R152 | Current Actor membership이 하나라도 남아 있으면 public close는 false로 끝나며 Framework가 member Actor를 숨겨서 이동하거나 제거하지 않음 | 301-302 | |
| 11-spot-model-R153 | SpotWide relocation은 User Spot과 seal 시점의 member Actor를 하나의 aggregate로 preflight하고 commit | 303-304 | |
| 11-spot-model-R154 | PerActor relocation은 target에 stateless Spot shell을 준비하고 Spot authority를 먼저 옮긴 뒤 member Actor를 독립된 unit으로 옮김 | 305-306 | |
| 11-spot-model-R155 | User Spot 기본 execution mode는 SpotWide; 같은 User Spot의 Spot handler·member Actor handler·timer·lifecycle callback을 전체에서 한 번에 하나만 실행 | 308-309 | |
| 11-spot-model-R156 | Factory 등록에서 PerActor 선택 시 같은 Actor·같은 Spot lane·같은 timer만 각각 직렬화, 서로 다른 lane은 동시 실행 가능 | 310-311 | |
| 11-spot-model-R157 | Execution mode는 MeshNode lifecycle을 시작하기 전에 고정하며 실행 중에는 바꾸지 않음 | 311-312 | |
| 11-spot-model-R158 | Yield는 SpotWide에서만 사용 가능; shared User Spot turn을 반납한 뒤 continuation은 같은 공통 gate를 다시 얻어 새 turn에서 재개 | 314-316 | |
| 11-spot-model-R159 | PerActor에는 shared Spot turn이 없으므로 Yield를 제공하지 않음 | 316 | |
| 11-spot-model-R160 | Spot relocation coordination mode 기본값은 FrameworkManaged; 이 mode에서는 Framework가 현재 turn이 끝난 안전한 경계를 선택하므로 application이 별도 준비 신호를 보내지 않음 | 320-322 | |
| 11-spot-model-R161 | Round·match가 끝난 뒤에만 이동할 수 있는 Spot은 factory 등록에서 ApplicationSignaled를 선택 | 324-325 | |
| 11-spot-model-R162 | Application은 안전한 turn에서 RelocationReady().Defer()를 등록하고 handler를 끝냄 | 325-326 | |
| 11-spot-model-R163 | Defer() 뒤 일반 Framework operation을 같은 turn에서 시작하면 InvalidOperation | 326-327 | |
| 11-spot-model-R164 | [표] 사용할 relocation이 없음: 처리 owner=현재 owner, Completion outcome=Continued | 334 | |
| 11-spot-model-R165 | [표] Relocation이 relay-ready accepted 전에 취소됨: 처리 owner=복원한 source owner, Completion outcome=Continued | 335 | |
| 11-spot-model-R166 | [표] Relocation이 완료됨: 처리 owner=target owner, Completion outcome=Relocated | 336 | |
| 11-spot-model-R167 | Framework는 표의 처리 owner에서 OnRelocationReadyCompleted를 다음 application job보다 먼저 호출 | 338-339 | |
| 11-spot-model-R168 | Relocated이면 target이 queue 병합과 regular route 전환을 끝내고 dispatch를 열기 전에 호출; Continued이면 source가 다음 job 전에 호출 | 339-340 | |
| 11-spot-model-R169 | Callback이 완료되면 보류한 message와 timer를 다시 처리; Application은 다음 round를 이 callback에서 시작할 수 있음 | 340-341 | |
| 11-spot-model-R170 | Callback은 Spot interface의 기본 no-op 구현; ApplicationSignaled를 선택해도 override를 강제하지 않음 | 343-344 | |
| 11-spot-model-R171 | 정상 실행에서는 readiness 등록마다 logical completion 하나를 만듦 | 344-345 | |
| 11-spot-model-R172 | Callback 실행 중 process가 종료되면 완료를 확인할 수 없으므로 recovery에서 같은 completion을 다시 호출할 수 있음; Override는 retry-safe해야 함 | 345-346 | |
| 11-spot-model-R173 | FrameworkManaged, PerActor, Entry Spot 또는 Instance Spot에서 RelocationReady().Defer()를 호출하면 queue mutation 전에 InvalidOperation으로 실패하고 completion callback을 호출하지 않음 | 348-350 | |
| 11-spot-model-R174 | Creation request, placement, SpotRef와 close의 exact generation 검사는 16-spot-address-messaging이 정의 | 352-353 | |
| 11-spot-model-R175 | 새 User Spot은 factory가 instance를 만든 뒤 Configure, OnCreateAsync, OnInitializeAsync를 거쳐 Ready 상태가 됨 | 357-358 | |
| 11-spot-model-R176 | OnCreateAsync는 creation request를 검사하고 생성 수락 여부와 optional reply를 반환 | 358-359 | |
| 11-spot-model-R177 | 같은 stable type의 Ready User Spot을 찾아 Existing으로 끝난 GetOrCreate에서는 factory와 OnCreateAsync를 실행하지 않음 | 359-361 | |
| 11-spot-model-R178 | Actor membership을 지원하는 User Spot은 일반 join에서 target이면 OnActorJoinAsync와 OnJoinedActorAsync를 실행, source이면 commit 뒤 OnLeaveActorAsync를 실행 | 363-365 | |
| 11-spot-model-R179 | Actor 연결 단절은 OnDisconnectActorAsync로 알림 | 365-366 | |
| 11-spot-model-R180 | 이 callback들은 User Spot의 선택한 execution mode에 따라 Spot lifecycle lane에서 실행 | 366-367 | |
| 11-spot-model-R181 | SpotWide User Spot을 다른 node로 relocation할 때는 Spot과 member Actor의 logical membership을 그대로 유지 | 369-370 | |
| 11-spot-model-R182 | 따라서 member Actor에 대해 Entry/User Spot의 OnActorJoinAsync, OnJoinedActorAsync, OnLeaveActorAsync를 호출하지 않음 | 370-372 | |
| 11-spot-model-R183 | Source User Spot instance를 정리할 때는 RelocationOut 이유로 OnClosingAsync를 호출 | 372-373 | |
| 11-spot-model-R184 | Member Actor가 Session에 bind되어 있으면 Spot과 Actor를 target에 복원하고 aggregate owner를 commit하고 queue 병합·regular route 전환·lifecycle 뒤 dispatch를 연 다음 target runtime이 각 Session owner에 command 44 sessionRelocationRoute commit을 one-way로 보냄 | 375-377 | |
| 11-spot-model-R185 | Session owner는 aggregate에 포함된 각 Actor의 binding route를 target owner로 갱신 | 378-379 | |
| 11-spot-model-R186 | 각 Session owner는 exact binding을 한 번 갱신하고 held message를 target route로 제출한 뒤 seal을 해제 | 382-383 | |
| 11-spot-model-R187 | 적용 reply는 없으며 timeout이면 physical Session을 정리 | 383-384 | |
| 11-spot-model-R188 | Target User Spot과 member Actor는 command 44 적용 reply 없이 message를 처리 | 384 | |
| 11-spot-model-R189 | PerActor relocation에서는 target에 같은 SpotId와 ObjectGeneration을 사용하는 private Spot shell을 먼저 준비 | 388-389 | |
| 11-spot-model-R190 | 이 shell은 Location Store의 Spot authority가 target으로 바뀌기 전까지 application 요청을 받지 않음 | 389-390 | |
| 11-spot-model-R191 | Authority가 바뀐 뒤 새 ToSpot, Actor Create와 Join은 target이 처리하고, source shell은 아직 source에 남은 Actor의 기존 작업과 relocation control만 처리 | 390-392 | |
| 11-spot-model-R192 | Actor는 normal host scheduler에서 각각 이전하며 relocation 전용 동시 unit 상한을 두지 않음 | 394 | |
| 11-spot-model-R193 | Actor의 ObjectGeneration과 logical User Spot membership은 유지하고 Actor owner generation만 바꿈 | 394-395 | |
| 11-spot-model-R194 | Infrastructure relocation은 OnActorJoinAsync, OnJoinedActorAsync, OnLeaveActorAsync 또는 OnDisconnectActorAsync를 호출하지 않음 | 395-397 | |
| 11-spot-model-R195 | 마지막 Actor와 source에서 이미 수락한 Spot 작업을 모두 정리한 뒤 source shell에 RelocationOut을 전달하고 종료 | 397-398 | |
| 11-spot-model-R196 | 이 control(command 44)은 one-way send이며 적용 reply나 재전송 journal이 없음 | 402 | |
| 11-spot-model-R197 | Instance Spot은 Actor membership이 없는 Spot; Direct packet handler, timer, outbound call은 사용 가능하나 다음은 사용 불가: Actor create·join·leave·relocation, Logical Multicast subscription, Manager Create·GetOrCreate | 407-412 | |
| 11-spot-model-R198 | Spot direct call은 기본적으로 실행 중인 Spot만 찾음; Missing RID에서 Instance Spot을 준비하려면 같은 call에 Instance intent를 명시해야 함 | 414-416 | |
| 11-spot-model-R199 | 일반 message와 Find는 hidden create를 시작하지 않음 | 416 | |
| 11-spot-model-R200 | 최초 message를 보존하는 cold activation, factory 실행, Ready barrier는 16-spot-address-messaging §4가 정의 | 416-419 | |
| 11-spot-model-R201 | Instance Spot은 application handler나 timer가 자신의 context에서 close할 수 있음 | 421 | |
| 11-spot-model-R202 | Host Relocate에서는 Actor가 없는 Spot 하나를 relocation unit으로 처리 | 422 | |
| 11-spot-model-R203 | Instance Spot의 direct handler와 timer는 하나의 Spot execution gate를 사용 | 423 | |
| 11-spot-model-R204 | Yield로 이 turn을 반납하면 다음 Instance Spot record를 실행할 수 있고, continuation은 같은 gate에서 새 turn으로 재개 | 424-425 | |
| 11-spot-model-R205 | Instance Spot은 Actor membership을 지원하지 않으므로 Actor create·join·joined·leave·disconnect callback을 제공하지 않음 | 429-430 | |
| 11-spot-model-R206 | Missing Instance Spot의 cold activation에서는 factory가 instance를 만든 뒤 Configure와 OnInitializeAsync를 실행 | 430-432 | |
| 11-spot-model-R207 | User Spot 생성에 사용하는 OnCreateAsync나 빈 creation request를 사용하지 않음 | 432-433 | |
| 11-spot-model-R208 | Activation을 시작한 첫 업무 message를 Ready 전에 durable inbox의 첫 record로 보존 | 433-434 | |
| 11-spot-model-R209 | Application이 자신의 context에서 정상 닫으면 ExplicitClose, Host가 relocation 없이 종료하면 HostShutdown, relocation commit 뒤 source instance를 정리하면 RelocationOut 이유로 OnClosingAsync 호출 | 436-438 | |
| 11-spot-model-R210 | Framework는 Instance Spot을 유휴 기준으로 정리할 수 있음. User Spot과 Entry Spot은 정리하지 않음 | 442-443 | |
| 11-spot-model-R211 | 정리한 User Spot으로 온 message는 되살아나지 못하고 실패함 — 일반 message는 없는 object를 만들지 않기 때문 (18-object-routing) | 443-444 | |
| 11-spot-model-R212 | Instance Spot은 Instance intent를 명시한 call이 다시 cold activation하므로 정리해도 다음 call에서 복구됨 | 445-446 | |
| 11-spot-model-R213 | 정리는 두 조건을 함께 만족할 때만 시작: 유휴 시간, 진행 중 작업 없음 | 448-453 | |
| 11-spot-model-R214 | 유휴 시간 조건: 마지막 application 작업 완료 이후 InstanceSpotIdleTimeout을 넘김; 기본값 0이며 0은 정리하지 않음을 뜻함 | 452 | |
| 11-spot-model-R215 | 진행 중 작업 없음 조건: application queue와 timer queue가 비어 있고, 완료를 기다리는 operation과 relocation 참여가 없음 | 453 | |
| 11-spot-model-R216 | 정리는 IdleEvicted로 OnClosingAsync를 호출한 뒤 local instance를 내리고 Location Store의 owner record를 제거 | 455-456 | |
| 11-spot-model-R217 | Application 상태를 보존하지 않으므로 유지해야 하는 상태는 OnClosingAsync에서 application이 저장 | 456-457 | |
| 11-spot-model-R218 | 정리 뒤 같은 ID로 온 Instance intent call은 새 ObjectGeneration으로 cold activation | 459 | |
| 11-spot-model-R219 | 정리 뒤 도착한 일반 message는 NotFound로 끝남 | 460 | |
| 11-spot-model-R220 | Entry Spot은 구현 type만 등록; User·Instance Spot은 stable type, factory configure callback에서 option과 relocation policy를 함께 등록; Callback은 policy를 정확히 하나 선택해야 함 | 465-467 | |
| 11-spot-model-R221 | 세 Context(User/Entry/Instance)는 공통 identity, outbound call, timer와 worker 기능을 공유 | 484 | |
| 11-spot-model-R222 | Framework는 factory를 호출하기 전에 MeshName, SpotId, ObjectGeneration, NodeRid와 owner fence가 결합된 exact Context를 만듦 | 485 | |
| 11-spot-model-R223 | Factory가 반환한 Spot은 전달받은 Context를 read-only member로 그대로 노출해야 하며, 다른 Context를 반환하면 staging Spot을 Ready로 공개하지 않음 | 486-487 | |
| 11-spot-model-R224 | Same-node operation은 Spot instance와 Context를 유지; Cross-node relocation은 SpotId·ObjectGeneration을 유지하고 target owner generation에 결합한 새 Context를 target factory에 전달하며 commit 뒤 source Context의 새 operation을 fence | 487-489 | |
| 11-spot-model-R225 | User Spot에는 Actor leave와 close가 있고 Instance Spot에는 close만 있음 | 490 | |
| 11-spot-model-R226 | Entry Spot은 close operation 대신 Actor destroy와 full Spot handler registry를 제공 | 535-536 | |
| 11-spot-model-R227 | 정확한 전체 interface와 lifecycle callback은 languages/dotnet/interfaces/05-spots.ko.md가 소유 | 549-550 | |
| 11-spot-model-R228 | 문서 경계표: 12-spot-messaging=Spot direct, Logical Multicast, queue admission과 dispatch | 556 | |
| 11-spot-model-R229 | 문서 경계표: 13-mesh-node=Object role, Entry Spot과 factory 등록, placement capability | 557 | |
| 11-spot-model-R230 | 문서 경계표: 15-spot-actor=Actor 생성, Entry·User Spot membership과 callback·commit 순서 | 558 | |
| 11-spot-model-R231 | 문서 경계표: 16-spot-address-messaging=User·Instance Spot의 ID, 생성, cold activation, route와 close | 559 | |
| 11-spot-model-R232 | 문서 경계표: 30-host-relocation-flow=세 Spot 종류의 shutdown, relocation과 recovery 순서 | 560 | |
| 11-spot-model-R233 | [검증] Entry Spot은 Object Server startup에서 Framework가 Spot ID를 발급하고 initialization 뒤에만 공개 | 564-565 | |
| 11-spot-model-R234 | [검증] Entry Spot ID가 MeshNode와 같은 diagnostic prefix, 별도 UUID v4 사용, descriptor가 lifecycle generation과 exact RID mapping을 게시 | 566-567 | |
| 11-spot-model-R235 | [검증] Replacement lifecycle은 새 Entry Spot ID를 발급하고 active authority 충돌에서 즉시 실패 | 568 | |
| 11-spot-model-R236 | [검증] Caller가 예약된 Entry Spot 형식으로 User·Instance Spot ID를 지정하면 Store와 factory 실행 전에 거부 | 569-570 | |
| 11-spot-model-R237 | [검증] User Spot manager만 명시적인 Create·GetOrCreate를 제공 | 571 | |
| 11-spot-model-R238 | [검증] Instance intent가 없는 일반 direct message와 Find는 Missing Instance Spot을 만들지 않음 | 572-573 | |
| 11-spot-model-R239 | [검증] Entry·User Spot은 Actor membership과 Logical Multicast subscription을 지원하고 Instance Spot은 둘 다 거부 | 574-575 | |
| 11-spot-model-R240 | [검증] Actor 업무 payload는 Entry·User Spot callback을 경유하지 않고 Actor queue에 직접 제출 | 576-577 | |
| 11-spot-model-R241 | [검증] Entry Spot 자체는 relocation하지 않으며 target Object Server startup에서 새 identity로 준비 | 578-579 | |
| 11-spot-model-R242 | [검증] User Spot은 member Actor와 aggregate로 이동하고 Instance Spot은 Actor가 없는 단일 relocation unit으로 이동 | 580-581 | |

### 12-spot-messaging

| # | 규칙 (수치·상태·오류) | 옛 위치 | 새 위치 |
|---|---|---|---|
| 12-spot-messaging-R1 | Spot direct는 global Spot ID 하나를 지정해 전달 대상을 정한다 | 22 | |
| 12-spot-messaging-R2 | Spot이 있으면 그 Spot을 소유한 node로 보낸다; 없고 InstanceSpot(...)을 지정했다면 새 Spot을 만들 node를 선택 | 22 | |
| 12-spot-messaging-R3 | Logical Multicast는 ChannelName과 topic을 지정, 참여 node 중 weight>0이고 ready 상태인 remote node를 먼저 선택 | 23 | |
| 12-spot-messaging-R4 | 각 수신 node는 자신의 local Spot 중 같은 ChannelName·topic으로 등록된 Spot에 전달 | 23 | |
| 12-spot-messaging-R5 | Spot direct에서 Spot이 없고 InstanceSpot(...)도 지정하지 않았다면 새 Spot을 만들지 않고 target-not-found 결과를 반환 | 25-26 | |
| 12-spot-messaging-R6 | Logical Multicast는 source가 remote Spot ID 목록을 만드는 방식이 아니다 | 28-29 | |
| 12-spot-messaging-R7 | Framework가 Channel 참여 node마다 message를 한 번 보내고, 각 node가 자신의 subscription을 검사해 받을 local Spot을 결정 | 29-31 | |
| 12-spot-messaging-R8 | Weight가 0보다 큰 node는 Logical Multicast의 remote 전달 후보에 포함된다 | 33 | |
| 12-spot-messaging-R9 | Ready 상태가 아닌 node는 weight가 양수여도 이번 전달 대상에 포함하지 않는다 | 34 | |
| 12-spot-messaging-R10 | Spot ID는 UTF-8 encoded 크기 1..255 bytes의 case-sensitive exact string | 204-205 | |
| 12-spot-messaging-R11 | 문자열 전체가 같아야 같은 ID로 본다 | 205 | |
| 12-spot-messaging-R12 | MeshName은 Spot을 처음 배치할 위치를 정할 때만 사용, Spot 식별 값에는 포함하지 않음 | 206-207 | |
| 12-spot-messaging-R13 | Spot kind는 Entry, User, Instance를 구분 | 209 | |
| 12-spot-messaging-R14 | Stable type은 application이 배포가 바뀐 뒤에도 식별할 수 있도록 정한 고정 이름 | 209-210 | |
| 12-spot-messaging-R15 | 같은 ID를 MeshName, Spot kind, stable type 중 하나라도 다르게 하여 재사용할 수 없다 | 212-216 | |
| 12-spot-messaging-R16 | Entry Spot ID는 Framework가 발급한다. 호출자는 생성하거나 create target으로 지정하지 않는다 | 218-219 | |
| 12-spot-messaging-R17 | Spot factory, Entry Spot, Spot lifecycle은 Object Server role MeshNode에만 등록 가능 | 224-225 | |
| 12-spot-messaging-R18 | Object role `None`: Object manager를 통한 Spot 생성·조회·direct messaging 제공 안 함; Spot 생성/실행/Entry Spot 제공 안 함; Location Store 불필요 | 227-229 | |
| 12-spot-messaging-R19 | Object role `Client`: 생성/조회/메시징 요청 가능; Factory/Entry Spot을 server 정보에 등록하지 않음; Location Store 필요 | 227-230 | |
| 12-spot-messaging-R20 | Object role `Server`: Client 작업 모두 가능; Factory, Entry Spot, Spot lifecycle 등록; Location Store 필요 | 227-231 | |
| 12-spot-messaging-R21 | ChannelName 기반 Logical Multicast publisher의 등록 여부는 Object role과 별도로 구성 | 233-234 | |
| 12-spot-messaging-R22 | Spot direct와 Logical Multicast는 Node·Channel 메시징과 같은 MeshNode ROUTER를 사용, 별도 ROUTER/PUB-SUB mesh 없음 | 238-239 | |
| 12-spot-messaging-R23 | Application은 target RID, endpoint, current owner 구분용 generation을 지정하지 않음 | 242-246 | |
| 12-spot-messaging-R24 | Instance Spot을 따로 생성하는 operation은 제공하지 않는다 | 251 | |
| 12-spot-messaging-R25 | Classic fanout은 PUB/SUB socket 사용, Service event fanout과 Spot Logical Multicast는 서로 다른 기능 | 258-259 | |
| 12-spot-messaging-R26 | 두 기능(Classic fanout, Logical Multicast)은 물리 연결과 구독 상태를 공유하지 않음 | 261-264 | |
| 12-spot-messaging-R27 | Framework는 먼저 최근 확인한 owner의 송신 경로를 cache(positive route cache)에서 찾는다 | 272-273 | |
| 12-spot-messaging-R28 | 정보 없으면 Location Store에 현재 owner를 묻는다 | 273-274 | |
| 12-spot-messaging-R29 | Location Store는 각 Spot에 대해 현재 owner, ObjectGeneration, lifecycle 상태를 기록 (=authority) | 276-278 | |
| 12-spot-messaging-R30 | ObjectGeneration은 같은 Spot ID로 Spot이 다시 만들어졌을 때 이전/새 Spot을 구분하는 번호 | 279-281 | |
| 12-spot-messaging-R31 | 한 generation에는 실제 Spot 하나만 존재 가능 | 281 | |
| 12-spot-messaging-R32 | Ready는 Spot 생성·초기화·Location Store 기록이 끝나 message를 받을 수 있는 상태 | 283-284 | |
| 12-spot-messaging-R33 | 일반 Spot message의 target은 SpotId | 287 | |
| 12-spot-messaging-R34 | Message를 받을 node는 자신이 current owner인지, 같은 ID의 Ready Spot 존재 여부, Spot queue 여유를 확인 | 287-289 | |
| 12-spot-messaging-R35 | `owner fence`는 이전 owner의 route를 거부하기 위해 current owner를 식별하는 값 | 289 | |
| 12-spot-messaging-R36 | 요청 시 확인한 ObjectGeneration은 route snapshot과 stale cache 구분 정보이며, application handler target 일치 조건이 아니다 | 290-291 | |
| 12-spot-messaging-R37 | 같은 owner에서 Spot 제거 후 같은 ID로 다시 만들어졌으면 queue가 수락하는 시점의 current Ready Spot에 payload를 넣는다 | 291-292 | |
| 12-spot-messaging-R38 | 실행 중인 Instance Spot이 없을 때 새로 만들어 준비하는 과정을 cold activation이라 한다 | 296-297 | |
| 12-spot-messaging-R39 | Spot direct call에 Instance intent 없고 target Spot 없으면 NotFound로 끝난다 | 299-300 | |
| 12-spot-messaging-R40 | 이 경우 Framework는 생성 정보(Spot kind, stable type, 최초 배치 위치)를 만들지 않는다 | 300-301 | |
| 12-spot-messaging-R41 | Instance intent를 명시하면 Missing Instance Spot을 cold activation 가능; 필요시 stable type과 최초 MeshName을 함께 지정 | 303-308 | |
| 12-spot-messaging-R42 | Framework는 선택한 Mesh에서 해당 stable type을 등록한 모든 eligible node를 후보로 사용 | 310-311 | |
| 12-spot-messaging-R43 | Serving 상태와 capacity를 확인한 뒤 node-wide placement weight로 target 하나를 선택 | 311-312 | |
| 12-spot-messaging-R44 | Stable type을 생략하면 Framework는 Mesh에 등록된 Instance type을 확인, 서로 다른 type이 하나뿐이면 자동 선택 | 314-316 | |
| 12-spot-messaging-R45 | 여러 MeshNode가 같은 type을 등록했어도 하나의 type으로 센다 | 316 | |
| 12-spot-messaging-R46 | 서로 다른 type이 둘 이상이면 호출자가 stable type을 명시해야 한다 | 318 | |
| 12-spot-messaging-R47 | Spot authority가 이미 존재하면 Location Store에 기록된 kind, stable type, 현재 Mesh를 사용, MeshName을 호출자에게 요구하지 않음 | 320-322 | |
| 12-spot-messaging-R48 | 표: Instance intent 없음 + Spot 없음 → Target-not-found, 생성 정보 안 만듦 | 324-327 | |
| 12-spot-messaging-R49 | 표: Instance intent 없음 + Location Store에 정보 있음 → 저장된 kind/type/Mesh와 current owner 송신경로 사용 | 324-327 | |
| 12-spot-messaging-R50 | 표: Instance intent 있음 + Spot 없음 → Source가 stable type·최초 Mesh 결정, Serving/type등록/capacity/weight로 target 선택, 최초 message+생성정보 함께 전송 | 324-327 | |
| 12-spot-messaging-R51 | 표: Instance intent 있음 + Location Store 정보 있음 → 저장된 kind/type/Mesh 사용, 기존 Spot 이동시키지 않음 | 324-327 | |
| 12-spot-messaging-R52 | Cold activation에서는 최초 application message와 Spot 생성·reply에 필요한 정보를 하나의 전달 단위(activation envelope)에 넣는다 | 329-331 | |
| 12-spot-messaging-R53 | Envelope 구성요소: operation identity, send/request 구분, source node RID와 lifecycle generation, optional source Spot ID, reply correlation, deadline, target descriptor fence, command 39 optional metadata 존재여부와 metadata frame, application payload | 331-334 | |
| 12-spot-messaging-R54 | Target transport가 envelope를 수락한 것은 message를 받았다는 뜻일 뿐, request call은 이 시점에 완료되지 않는다 | 388-390 | |
| 12-spot-messaging-R55 | Request call은 reply, remote 오류, timeout, cancellation, shutdown 중 하나가 확정될 때까지 기다린다 | 389-390 | |
| 12-spot-messaging-R56 | 하나의 deadline을 Ready Spot 조회부터 Spot 준비, handler 실행, reply까지 적용한다 | 390-392 | |
| 12-spot-messaging-R57 | Target runtime은 현재 authority Spot이 이 node에 있는지 확인; 있으면 그 queue에 제출 | 394-395 | |
| 12-spot-messaging-R58 | 없으면 complete activation envelope를 Relocation Store에 변경 불가능한 recovery root로 저장 | 395-396 | |
| 12-spot-messaging-R59 | reference, SHA-256, encoded size, retention을 확인한 다음 Location Store에 생성 가능 여부 요청 | 397-398 | |
| 12-spot-messaging-R60 | Location Store는 recovery 정보와 provider 발급 reservation fence를 Creating authority에 함께 기록 | 398-399 | |
| 12-spot-messaging-R61 | 여러 target이 동시 요청해도 생성 권한을 먼저 확보한 target만 owner로 기록하고 factory 실행 | 399-401 | |
| 12-spot-messaging-R62 | Factory/초기화 완료 후 최초 request를 다시 만들지 않고 recovery root의 message를 durable activation inbox 첫 record로 확정 | 403-405 | |
| 12-spot-messaging-R63 | 확정 전까지 handler 실행은 barrier로 차단 | 405-406 | |
| 12-spot-messaging-R64 | recovery root와 replay cursor를 유지한 Ready authority를 commit, 첫 record를 local queue 선두에 복원 후 barrier 개방 | 406-407 | |
| 12-spot-messaging-R65 | 후속 message는 이 최초 request를 추월할 수 없다 | 408 | |
| 12-spot-messaging-R66 | 최초 handler 완료를 durable 기록하고 replay cursor를 inbox sequence까지 갱신한 뒤에만 expected-version Preserve CAS로 recovery pointer 제거 | 410-412 | |
| 12-spot-messaging-R67 | Queue에 넣었다는 사실만으로 pointer를 제거해서는 안 된다 | 412-413 | |
| 12-spot-messaging-R68 | CAS 성공 후 Relocation Store의 root를 idempotent하게 삭제 | 413 | |
| 12-spot-messaging-R69 | Handler가 만든 reply는 envelope의 request 식별 정보를 사용해 원래 caller로 돌아감 | 413-414 | |
| 12-spot-messaging-R70 | Source는 Ready 뒤 두 번째 direct request를 보내지 않는다 | 415, 529 | |
| 12-spot-messaging-R71 | Target process가 Reserve 뒤 종료되면 startup의 complete authority scan이 Pending creation 정보를 다시 읽는다 | 417-419 | |
| 12-spot-messaging-R72 | 같은 reservation과 generation으로 factory/initialize/durable inbox 복원을 이어가거나 정확한 fence로 생성을 중단 | 418-419 | |
| 12-spot-messaging-R73 | Ready commit 뒤 queue 선두 복원 전 종료되었다면 recovery root와 cursor로 최초 record를 먼저 복원 | 419-421 | |
| 12-spot-messaging-R74 | 이 복원이 끝나기 전에는 해당 owner가 application message를 받도록 Serving gate를 열지 않는다 | 421-422 | |
| 12-spot-messaging-R75 | 다른 target이 생성 권한을 먼저 확보했다면 현재 target은 Spot을 만들지 않는다 | 424 | |
| 12-spot-messaging-R76 | 먼저 생성 권한을 얻은 Spot이 이미 Ready면 같은 request 식별정보/payload/reply연결정보/deadline을 유지한 채 current owner로 한 번 전달 | 425-427 | |
| 12-spot-messaging-R77 | 아직 Creating이면 해당 Spot 준비가 끝날 때까지 같은 request 처리에 합류 | 427-428 | |
| 12-spot-messaging-R78 | Ready authority가 이미 있으면 activation 흐름을 사용하지 않고, Location Store 저장된 type·Mesh·current owner route로 일반 direct payload를 보낸다 | 430-431 | |
| 12-spot-messaging-R79 | .NET 예시: InstanceSpot("ShoppingCartSpot"), InMesh("object-mesh"), Timeout(3s) — Spot 조회부터 reply까지 하나의 deadline 적용 | 438-451 | |
| 12-spot-messaging-R80 | InstanceSpot(...) 생략 시 Ready authority 없는 request는 NotFound로 끝남 | 456-457 | |
| 12-spot-messaging-R81 | Authority가 이미 있으면 저장된 current owner route 사용하므로 InMesh(...)가 기존 Spot을 이동시키지 않음 | 457-459 | |
| 12-spot-messaging-R82 | Spot direct send는 Async(...)만 제공, 즉시완료 반환하는 별도 API 없음 | 463-464 | |
| 12-spot-messaging-R83 | Owner MeshNode의 ROUTER queue가 일시적으로 가득 차면 유한한 send timeout 동안 queue가 받을 수 있을 때까지 기다림 | 466-467 | |
| 12-spot-messaging-R84 | Ready Spot에 보내는 일반 direct send는 source 송신 경로가 수락하면 결과값 없이 완료 (handler 실행 완료 의미 아님) | 469-470 | |
| 12-spot-messaging-R85 | Send timeout까지 수락 못하면 DeadlineExceeded | 471-472 | |
| 12-spot-messaging-R86 | Spot이나 route가 없으면 NotFound | 472 | |
| 12-spot-messaging-R87 | runtime 종료 중이면 ShuttingDown | 472 | |
| 12-spot-messaging-R88 | Cold activation이 필요한 submit도 송신 경로가 activation envelope를 수락하면 완료; target이 Ready 되는 과정과 handler 실행은 기다리지 않음 | 476-478 | |
| 12-spot-messaging-R89 | Activation envelope 보존 정보 표: 최초 application message(재전송 불필요), operation identity(retry/중복 구분), reply correlation, 작업 deadline, Spot global ID, 선택한 Mesh와 stable type, target descriptor fence(등록정보 변경 판별) | 480-490 | |
| 12-spot-messaging-R90 | Target runtime은 authority Spot이 이 node에 없으면 Location Store에 생성 가능 여부 요청 | 492-493 | |
| 12-spot-messaging-R91 | 경쟁 target이나 중복 envelope 있어도 생성 권한 먼저 확보한 target만 owner 기록·factory 실행 | 493-494 | |
| 12-spot-messaging-R92 | Local Spot과 remote Spot은 같은 handler와 callback 실행 규칙을 사용 | 498 | |
| 12-spot-messaging-R93 | 호출자는 owner RID, endpoint, 내부 통신용 route 정보를 만들지 않는다 | 499 | |
| 12-spot-messaging-R94 | Spot direct request가 실패해도 다른 Spot으로 자동 재전송하지 않는다 | 500 | |
| 12-spot-messaging-R95 | Application은 실패 후 같은/다른 Spot ID로 새 request 시작 가능하며 이는 Framework의 자동 재전송이 아닌 별도 operation | 504-506 | |
| 12-spot-messaging-R96 | 이전 target이 이미 실행했을 가능성 있으면 application이 중복 실행을 처리해야 함 | 506-507 | |
| 12-spot-messaging-R97 | Instance Spot을 위한 별도 create request는 없다 | 509 | |
| 12-spot-messaging-R98 | InstanceSpot(...) 지정 call은 최초 application message를 activation envelope에 포함, 별도 생성 request로 바뀌지 않으며 생성 후 application payload로 처리 | 509-512 | |
| 12-spot-messaging-R99 | Cold activation 첫 message 처리 8단계 순서(수신·authority확인→envelope저장+생성권한요청→owner기록+factory→Configure/initialize→durable inbox 첫record확정(barrier차단)→Ready authority확정→queue선두복원+barrier개방→handler완료기록+cursor갱신+pointer/root제거) | 514-527 | |
| 12-spot-messaging-R100 | Spot handler와 timer는 Channel send/request 시작 가능; ChannelName으로 현재 process에 등록된 송신 경로 선택 | 533-535 | |
| 12-spot-messaging-R101 | 현재 Spot을 소유한 MeshNode에 대상 ChannelName이 없어도 같은 process에 다른 RouteMesh의 해당 ChannelName 송신경로나 ClientServer client의 해당 ChannelName 송신경로가 있으면 사용 가능 | 537-541 | |
| 12-spot-messaging-R102 | 현재 process에 대상 송신 경로가 없으면 다른 process/MeshNode를 중계 경로로 사용하지 않음; NotFound로 끝남 | 543-544 | |
| 12-spot-messaging-R103 | Channel request 시 Framework가 유지하는 정보: Request correlation(reply가 어떤 request 결과인지), Request 시작 시점의 Spot 실행(reply대기 callback으로 복귀), Request를 시작한 Spot의 generation(다시 만들어진 새 Spot에 이전 reply 전달 안 함) | 548-554 | |
| 12-spot-messaging-R104 | Async는 모든 실행 문맥에서 사용 가능 | 556 | |
| 12-spot-messaging-R105 | Yield는 SpotWide User Spot과 Instance Spot에서만 사용 가능 | 556-557 | |
| 12-spot-messaging-R106 | Async: request를 시작한 원래 turn 유지 | 559-561 | |
| 12-spot-messaging-R107 | Yield: shared Spot turn 반환, 결과 확정되면 원래 Spot queue에 재개 작업 하나 추가 | 559-562 | |
| 12-spot-messaging-R108 | Entry Spot, PerActor User Spot, Entry Spot Actor, Node·Channel handler, owner turn 밖 client에서 Yield 호출하면 InvalidOperation으로 완료(제출·turn반환 없음) | 564-566 | |
| 12-spot-messaging-R109 | Yield는 Channel·Spot·Actor request, CPU·I/O worker call, Actor·Spot create·get-or-create call에 제공 | 568-569 | |
| 12-spot-messaging-R110 | Yield는 Actor join, send, publish, timer 등록, close, destroy에는 제공하지 않음 | 569 | |
| 12-spot-messaging-R111 | Reply를 새로운 Spot message로 다시 전달하지 않는다 | 571 | |
| 12-spot-messaging-R112 | Spot shutdown, timeout, cancellation, reply가 동시에 발생해도 request의 최종 성공/실패 결과는 하나만 선택 | 573-574 | |
| 12-spot-messaging-R113 | 이전 generation Spot에 늦게 도착한 reply는 새로 만들어진 같은 Spot ID의 Spot에 전달하지 않음 | 574-575 | |
| 12-spot-messaging-R114 | Service runtime이 하지 않는 동작: 등록되지 않은 다른 RouteMesh 검색, RouteMesh 간 message 중계, ClientServer transport에서 원래 Spot ID를 실제 연결 송신자 주소로 사용 | 577-582 | |
| 12-spot-messaging-R115 | 송신 경로가 다른 RouteMesh/ClientServer에 있어도 reply는 request를 시작한 Spot의 같은 실행과 generation으로 돌아감 | 610-611 | |
| 12-spot-messaging-R116 | Logical Multicast는 (ChannelName, topic) 조합으로 전달 범위를 정한다 | 639-641 | |
| 12-spot-messaging-R117 | ChannelName은 message를 받을 RouteMesh 참여 node를 선택; Topic은 각 수신 MeshNode에서 받을 local Spot subscription을 선택 | 643-645 | |
| 12-spot-messaging-R118 | 호출자는 MeshName이나 endpoint를 지정하지 않는다(현재 process Channel 목록에서 검색) | 647-648 | |
| 12-spot-messaging-R119 | 같은 ChannelName을 다음처럼 중복 등록하면 host startup이 실패: 서로 다른 RouteMesh에 중복, RouteMesh와 ClientServer에 중복, 서로 다른 ClientServer 송신경로에 중복 | 650-654 | |
| 12-spot-messaging-R120 | ChannelName은 물리 socket 이름이 아니라 같은 Channel 참여 MeshNode를 나타냄 | 656-657 | |
| 12-spot-messaging-R121 | Framework는 publish 하나를 한 번의 작업으로 처리; 시작 시 remote target 목록과 local Spot 목록을 고정(snapshot), publish 도중 바뀌어도 목록은 불변 | 661-664 | |
| 12-spot-messaging-R122 | Remote MeshNode는 message 수신 시점에 자신의 local subscription을 따로 검사 | 664-665 | |
| 12-spot-messaging-R123 | Publish 처리 5단계: (1)weight>0 & ready인 remote MeshNode 목록 고정 (2)각 remote에 message 한 번 제출 (3)송신 MeshNode 자신도 참여하면 자신 subscription 검사 (4)각 수신 MeshNode는 자신 local subscription만 검사 (5)일치 Spot의 application queue에 같은 message data 참조 제출 | 667-674 | |
| 12-spot-messaging-R124 | 같은 node의 여러 Spot에 전달 시 Spot 수만큼 payload를 다시 encode/복사하지 않음 | 676-677 | |
| 12-spot-messaging-R125 | Message data는 처리 중 변경 불가, 각 queue는 같은 data를 가리킴, 마지막 queue가 더 이상 사용하지 않으면 Framework가 회수 | 677-679 | |
| 12-spot-messaging-R126 | 이 data 공유 방식은 application API에 노출하지 않음 | 681 | |
| 12-spot-messaging-R127 | Framework는 remote node에 어떤 Spot이 있는지 또는 각 node queue 상태를 호출자에게 반환하지 않음 | 683-684 | |
| 12-spot-messaging-R128 | 호출자가 Node direct send를 여러 번 호출해 Logical Multicast를 직접 구현하는 방식은 공통 계약에 포함하지 않음 | 684-685 | |
| 12-spot-messaging-R129 | Logical Multicast는 publish 전용 전달 정책 option을 제공하지 않는다 | 689 | |
| 12-spot-messaging-R130 | Framework는 동시에 처리할 수 있는 publish 작업 수를 제한 | 691 | |
| 12-spot-messaging-R131 | 모든 worker 사용 중이면 유한한 send timeout까지 worker와 source-local outbound capacity를 기다림 | 691-692 | |
| 12-spot-messaging-R132 | 그 안에 확보 못하면 어떤 target에도 message를 보내지 않고 DeadlineExceeded로 실패 | 693 | |
| 12-spot-messaging-R133 | Publish 시작 전 cancellation/shutdown 확정되면 각각 typed cancellation 또는 ShuttingDown 오류로 완료 | 694-695 | |
| 12-spot-messaging-R134 | Worker가 작업 받으면: 고정한 각 remote target에 한 번 제출, 일치 local Spot queue에는 target별 즉시 제출 | 697-700 | |
| 12-spot-messaging-R135 | Local Spot queue에 용량 없으면 기다리지 않고 다음 target 처리 | 702 | |
| 12-spot-messaging-R136 | 이 실패를 publish 전용 결과나 monitoring 값으로 집계하지 않음 | 703 | |
| 12-spot-messaging-R137 | Worker와 source-local outbound capacity 확보하여 대상 목록 처리를 넘기면 publish 시작 확정 | 707-708 | |
| 12-spot-messaging-R138 | Terminal call은 이 시점에 결과값 없이 정상 완료, target별 수락 결과 기다리지 않음 | 708-709 | |
| 12-spot-messaging-R139 | 시작 이후 cancellation/shutdown 발생해도 이미 시작한 작업을 전체 실패로 바꾸지 않음 | 709-710 | |
| 12-spot-messaging-R140 | 나중 target queue에 여유 없어도 앞서 성공한 제출을 취소하지 않음 | 710-711 | |
| 12-spot-messaging-R141 | Logical Multicast는 여러 source-local 제출 대상 중 일부에만 제출될 수 있음; 수락된 제출은 유지, 수락 못한 target은 public 결과/monitoring에 미반영 | 713-715 | |
| 12-spot-messaging-R142 | worker 없어 시작 못하면 caller에게 실패 알림; 시작 후에는 이미 수락된 제출 취소 안 함, target 수락 여부 caller에 반환·monitoring 집계 안 함 | 742-744 | |
| 12-spot-messaging-R143 | 고정한 remote target·local Spot 매치 수가 모두 0이어도 publish는 정상 완료 | 748 | |
| 12-spot-messaging-R144 | Publish transaction 시작 후 일부 target queue 용량 부족·연결불가여도 전체 작업 rollback/retry 안 함 | 748-750 | |
| 12-spot-messaging-R145 | Remote target 연결 실패와 local Spot queue 용량 부족을 publish 전용 결과·monitoring 값으로 만들지 않음 | 750-751 | |
| 12-spot-messaging-R146 | Publish 완료는 subscriber handler 실행이나 업무 처리 완료의 확인이 아니다 | 755 | |
| 12-spot-messaging-R147 | Publish 완료는 필요한 worker와 source-local capacity를 확보해 작업을 시작했음을 뜻함 | 755-756 | |
| 12-spot-messaging-R148 | 수신 MeshNode의 Spot queue 제출/handler 실행·완료를 기다리지 않음 | 756-757 | |
| 12-spot-messaging-R149 | Publish는 다음을 보장하지 않음: process 종료돼도 message가 남는 durable 저장, 나중에 재전송하는 replay, exactly-once 전달 | 757-762 | |
| 12-spot-messaging-R150 | Framework는 publish마다 remote·local target 수와 결과를 monitoring snapshot/metric/runtime event로 제공하지 않음; 전체 transport/mailbox 상태는 공통 runtime monitoring으로 확인 | 763-765 | |
| 12-spot-messaging-R151 | Spot subscription 등록 값: ChannelName, topic, packet name(typed handler 선택) | 790-794 | |
| 12-spot-messaging-R152 | 등록한 Spot이 해당 ChannelName에 참여하지 않으면 host 시작 불가 | 796 | |
| 12-spot-messaging-R153 | 같은 Spot에 ChannelName·topic·message kind·packet name이 모두 같은 subscription을 두 번 등록해도 host 시작 불가 | 798-803, 820-821 | |
| 12-spot-messaging-R154 | Subscription은 Spot의 Configure()에서 등록 | 807 | |
| 12-spot-messaging-R155 | Spot control claim: Actor의 Spot 진입/이탈/lifecycle 상태 변경 시 Spot이 관리하는 상태 변경을 Spot queue에서 실행하도록 만든 작업 | 825-827 | |
| 12-spot-messaging-R156 | Spot control claim은 target의 Spot lane에 들어가 같은 lane의 handler·control callback과 queue 순서대로 실행 | 829-830 | |
| 12-spot-messaging-R157 | SpotWide User Spot에서는 member Actor와 timer도 같은 shared gate 사용 | 830-831 | |
| 12-spot-messaging-R158 | Entry Spot과 PerActor User Spot에서는 Actor별 lane과 timer별 lane을 Spot lane과 분리 | 831-832 | |
| 12-spot-messaging-R159 | Actor가 처리할 업무 message는 control claim에 넣지 않음 | 832 | |
| 12-spot-messaging-R160 | 표: Spot application queue — 들어감: Spot direct payload, 일치 Logical Multicast payload, timer callback / 안 들어감: Actor 업무 payload, Actor join·leave와 lifecycle control callback | 840-842 | |
| 12-spot-messaging-R161 | 표: Instance Spot application queue — 들어감: Spot direct payload, timer callback / 안 들어감: Actor control, Logical Multicast subscription | 843 | |
| 12-spot-messaging-R162 | 표: Actor queue — 들어감: Actor 업무 payload / 안 들어감: Spot callback을 거쳐 전달하는 Actor payload | 844 | |
| 12-spot-messaging-R163 | Actor join·leave와 lifecycle control callback은 Spot application queue가 아니라 Spot control claim으로 처리; 한도·실행순서 모두 다르므로 섞지 않음 | 846-847 | |
| 12-spot-messaging-R164 | Instance Spot의 Actor control이나 Logical Multicast subscription은 등록/준비 시 거부됨 | 848-849 | |
| 12-spot-messaging-R165 | Spot application queue와 Actor queue는 한도가 있음; 한도 초과 시 동작은 계열/대기열 위치에 따라 다름 | 851-853 | |
| 12-spot-messaging-R166 | 표(계열/포화 대기열/결과): Send·one-way + 같은 runtime outbound/Spot·Actor 대기열 → §1 따름, send timeout까지 대기, 내부 waiter까지 차면 DeadlineExceeded | 855-857 | |
| 12-spot-messaging-R167 | 표: Send·one-way + 다른 node Spot·Actor 대기열 → 결과 없음(source outbound queue 수락 시점에 이미 완료), 이후 target admission 실패는 완료 결과 안 바꿈, metric·log·trace에만 남음 | 858 | |
| 12-spot-messaging-R168 | 표: Publish(시작 전) + worker자리/source-local outbound → send timeout까지 대기, 못 얻으면 DeadlineExceeded | 859 | |
| 12-spot-messaging-R169 | 표: Publish(시작 후) + local Spot 대기열 → 기다리지 않고 건너뜀, 이미 정상완료, publish 전용 결과·관측값 미집계 | 860 | |
| 12-spot-messaging-R170 | 표: Request + 같은 runtime Spot·Actor 대기열 → 기다리지 않고 CapacityExceeded | 861 | |
| 12-spot-messaging-R171 | 표: Request + 다른 node Spot·Actor 대기열 → 기다리지 않고 Unavailable | 862 | |
| 12-spot-messaging-R172 | 표: Control claim + 같은 runtime control 한도 → CapacityExceeded | 863 | |
| 12-spot-messaging-R173 | 표: Control claim + 다른 node control 한도 → Unavailable | 864 | |
| 12-spot-messaging-R174 | Publish 두 줄이 다른 이유: 완료 시점이 그 사이에 있음(시작 전엔 돌려줄 결과 있어 대기, 시작 후엔 이미 완료해 되돌릴 것 없음) | 866-867 | |
| 12-spot-messaging-R175 | Send 계열은 반환할 결과가 없어 재시도 판단 불가하므로 대기; Request는 오류로 판단 가능하므로 안 기다림; Request 대기 처리 시 송신측 자원이 수신측 처리속도에 묶여 두 노드가 서로 막는 구간 생김 | 869-872 | |
| 12-spot-messaging-R176 | Local/remote 구분 기준: "실패한 대기열을 이 runtime이 소유하는가" | 874-875 | |
| 12-spot-messaging-R177 | Spot control claim 작업은 application queue 한도를 공유하지 않음(join·leave/lifecycle control이 업무적체로 실패하면 적체 해소 방법 자체가 사라짐) | 878-880 | |
| 12-spot-messaging-R178 | control claim도 자기 몫의 한도를 가짐(application queue와 별개, 무한 아님) — 무한이면 memory 무한 증가, application payload 실행기회 상실 | 882-884 | |
| 12-spot-messaging-R179 | 한도 초과 오류: 같은 runtime 소유 control lane 한도 초과 → CapacityExceeded, 다른 node owner 알린 포화 → Unavailable | 886-887 | |
| 12-spot-messaging-R180 | Entry Spot과 Instance Spot의 application callback은 각 Spot turn에서 순서대로 실행 | 891-892 | |
| 12-spot-messaging-R181 | User Spot 기본 SpotWide mode: Spot queue와 member Actor queue가 하나의 공통 execution gate 사용 | 892-893 | |
| 12-spot-messaging-R182 | PerActor mode: Actor별, Spot lane별, timer별 gate 구분 | 893-894 | |
| 12-spot-messaging-R183 | SpotWide User Spot과 Instance Spot의 callback이 Yield로 shared turn 반환하면 같은 Spot의 다음 application 작업이 먼저 실행될 수 있음 | 896-898 | |
| 12-spot-messaging-R184 | Yield한 callback의 나머지 코드는 기다리던 결과 확정 후 같은 gate의 새 turn에서 재개 | 898-899 | |
| 12-spot-messaging-R185 | Entry Spot과 PerActor User Spot에서는 Yield 사용 불가 | 899 | |
| 12-spot-messaging-R186 | Member Actor가 Yield하면 User Spot execution gate만 반환하고 Actor queue claim은 유지 | 901-902 | |
| 12-spot-messaging-R187 | 따라서 다른 Actor·Spot handler·timer는 실행 가능하지만 같은 Actor의 다음 job은 현재 continuation이 끝날 때까지 시작하지 않음 | 902-903 | |
| 12-spot-messaging-R188 | Actor 업무 payload는 Spot application queue나 Spot callback을 거치지 않고 Actor queue에 직접 제출 | 909-910 | |
| 12-spot-messaging-R189 | Actor가 Spot 상태를 변경해야 하면 명시적 Spot 호출을 제출해야 함 | 912-913 | |
| 12-spot-messaging-R190 | Framework 자체 상태 진행 작업은 Spot application callback과 분리 처리: Spot 준비 알림, 비동기 호출 완료처리, 송신 경로 재수신 가능 알림, Spot/Actor 이동 처리 | 918-924 | |
| 12-spot-messaging-R191 | Application callback이 다른 작업 결과를 기다리는 동안에도 위 작업들은 계속 진행 가능해야 함 | 926-927 | |
| 12-spot-messaging-R192 | Instance intent로 cold activation 시작하지 않는 call에서 target Ready authority 없으면 Spot target 오류로 끝남 | 933-934 | |
| 12-spot-messaging-R193 | Close처럼 Spot ID와 ObjectGeneration을 함께 지정하는 lifecycle 작업은 Location Store의 current generation도 확인; 다르면 이미 바뀐 Spot 참조 오류 반환 | 936-938 | |
| 12-spot-messaging-R194 | Spot direct send/request에는 이 generation 검사를 적용하지 않음 | 939 | |
| 12-spot-messaging-R195 | Request handler를 찾지 못하거나 payload 해석 못하면 reply 보낼 경로가 남아있는지 확인; 있으면 오류 reply로 request 완료 | 941-943 | |
| 12-spot-messaging-R196 | One-way Spot direct handler와 Logical Multicast handler가 실패해도 원래 호출을 request로 바꾸지 않음; handler 실패는 runtime 관측 경로에 기록 | 944-946 | |
| 12-spot-messaging-R197 | Spot 종료를 시작하면 새 application payload를 더 이상 queue에 받지 않음 | 949 | |
| 12-spot-messaging-R198 | 이미 수락한 Spot turn과 lifecycle 정리는 drain deadline 안에서 처리 | 951-952 | |
| 12-spot-messaging-R199 | 종료된 Spot의 subscription은 Logical Multicast가 현재 node에서 전달할 Spot을 찾을 때 제외 | 952-954 | |
| 12-spot-messaging-R200 | Spot direct와 Logical Multicast는 04 메시지 모델이 정의한 변경 불가능한 metadata snapshot 사용(문서 재정의 없음) | 965-968 | |
| 12-spot-messaging-R201 | 관측 정보는 구분해 제공해야 할 항목: current owner MeshName, ChannelName, Origin RID, 수락 대기와 실패, 용량 부족, Spot 전달 결과 | 974-981 | |
| 12-spot-messaging-R202 | Logical Multicast의 remote·local target 수와 target별 결과는 publish 전용 관측 정보로 집계하지 않음 | 983 | |
| 12-spot-messaging-R203 | topic과 Spot ID는 metric label로 사용하지 않음 | 984 | |
| 12-spot-messaging-R204 | 검증: Spot direct와 Logical Multicast가 MeshNode ROUTER 하나를 함께 사용 | 992 | |
| 12-spot-messaging-R205 | 검증: Spot direct는 global Spot ID만 target으로 받음 | 993 | |
| 12-spot-messaging-R206 | 검증: Spot direct가 MeshName, owner RID, generation을 application에 요구하지 않음 | 994 | |
| 12-spot-messaging-R207 | 검증: Classic fanout PUB/SUB의 연결과 구독 상태가 Logical Multicast와 섞이지 않음 | 995-996 | |
| 12-spot-messaging-R208 | 검증: Instance intent 없는 Missing Spot message가 stable type이나 MeshName을 새로 제공하지 않음 | 1000-1001 | |
| 12-spot-messaging-R209 | 검증: Instance intent 없는 Missing Spot message가 Spot 생성 정보를 만들지 않음 | 1002 | |
| 12-spot-messaging-R210 | 검증: Instance intent 있는 call만 Missing Spot을 cold activation할 수 있음 | 1003 | |
| 12-spot-messaging-R211 | 검증: cold activation 시 stable type 명시 또는 Mesh에 type 하나뿐이면 자동 선택 | 1004-1005 | |
| 12-spot-messaging-R212 | 검증: Source는 자신을 owner로 먼저 기록하지 않고 최초 message 포함 activation envelope를 선택 target에 제출 | 1006-1007 | |
| 12-spot-messaging-R213 | 검증: 생성 권한 먼저 확보한 target만 owner 기록·factory 실행 | 1008 | |
| 12-spot-messaging-R214 | 검증: Reserved authority가 reservation fence와 recovery receipt 반환, process restart 뒤에도 같은 reservation과 durable inbox 선두 복원 | 1009-1010 | |
| 12-spot-messaging-R215 | 검증: Durable inbox 첫 record를 Ready 전에 확정, queue 선두 복원 끝나기 전 Serving gate 열지 않음 | 1011-1012 | |
| 12-spot-messaging-R216 | 검증: Spot Channel 호출은 ChannelName 등록된 다른 RouteMesh/ClientServer 송신 경로 사용 가능 | 1016-1017 | |
| 12-spot-messaging-R217 | 검증: 다른 송신 경로 사용해도 원래 Spot의 Async와 허용된 실행문맥의 Yield 의미 보존 | 1018 | |
| 12-spot-messaging-R218 | 검증: 다른 송신 경로 사용해도 reply를 request 시작 generation의 Spot으로 전달 | 1019-1020 | |
| 12-spot-messaging-R219 | 검증: Remote MeshNode마다 routed message를 한 번만 전송 | 1024 | |
| 12-spot-messaging-R220 | 검증: 각 수신 MeshNode는 자신의 local subscription만 검사 | 1025 | |
| 12-spot-messaging-R221 | 검증: 사용 가능한 publish worker 있을 때만 시작, 한 publish 두 번 시작 안 함 | 1026-1027 | |
| 12-spot-messaging-R222 | 검증: Publish 시작 후 cancellation 발생해도 처음 고정한 나머지 target 처리 중단하지 않음 | 1028-1029 | |
| 12-spot-messaging-R223 | 검증: 같은 node의 여러 target Spot이 복사본 없이 같은 message data 공유 | 1030 | |
| 12-spot-messaging-R224 | 검증: Local/remote target의 선택 수, 수락 수, drop 수, unreachable 수를 publish 전용 monitoring 값으로 집계하지 않음 | 1031-1032 | |
| 12-spot-messaging-R225 | 검증: Actor payload가 Spot application queue와 Spot callback을 거치지 않음 | 1036 | |
| 12-spot-messaging-R226 | 검증: Actor join·leave와 lifecycle control만 Spot control claim으로 전달 | 1037 | |
| 12-spot-messaging-R227 | InMesh 생략 시 후보 Mesh 둘 이상이면 InvalidOperation으로 끝남(SendCall/RequestCall 공통) | 85, 102 | |
| 12-spot-messaging-R228 | Timeout(...)은 Spot을 찾는 단계부터 reply까지 하나의 deadline을 적용 | 105-106 | |

### 13-mesh-node

| # | 규칙 (수치·상태·오류) | 옛 위치 | 새 위치 |
|---|---|---|---|
| 13-mesh-node-R1 | 문서 범위: RouteMesh에 참여하는 MeshNode의 identity, object role, object 배치 조건과 startup 순서 정의 | 15-16 | |
| 13-mesh-node-R2 | MeshNode는 다른 node와 물리적으로 연결되고 Channel membership을 제공; Actor와 Spot은 이 연결을 사용하지만 MeshNode RID를 자신의 logical identity로 사용하지 않음 | 18-19 | |
| 13-mesh-node-R3 | Framework는 Actor와 Spot의 전역 logical identity를 현재 owner가 존재하는 MeshNode의 route로 연결 | 20-21 | |
| 13-mesh-node-R4 | MeshName은 어떤 물리 RouteMesh와 MeshNode descriptor namespace에 속하는지 정하며, MeshNode가 시작된 뒤에는 바꿀 수 없음 | 29 | |
| 13-mesh-node-R5 | RID는 현재 MeshNode lifecycle을 식별하는 transport identity | 30 | |
| 13-mesh-node-R6 | Endpoint는 다른 peer가 이 MeshNode의 ROUTER에 연결할 주소 | 31 | |
| 13-mesh-node-R7 | ChannelName set: Server role로 참여하는 Channel 목록, 0개 이상 등록 가능, 시작된 뒤 변경 불가 | 32 | |
| 13-mesh-node-R8 | Object role은 None, Client, Server 중 하나이며 startup 전에 정함 | 33 | |
| 13-mesh-node-R9 | Lifecycle generation은 같은 transport identity에서 서로 다른 lifecycle을 구분하는 0이 아닌 식별 값 | 34 | |
| 13-mesh-node-R10 | Descriptor revision은 같은 lifecycle 안에서 바뀔 수 있는 MeshNode descriptor 내용을 구분하는 0이 아닌 값 | 35 | |
| 13-mesh-node-R11 | MeshName은 ActorId나 SpotId의 identity에 포함되지 않음 | 37 | |
| 13-mesh-node-R12 | ActorId와 User·Instance SpotId는 Location Store가 관리하는 전체 transaction 범위에서 각각 전역 key | 37-38 | |
| 13-mesh-node-R13 | MeshName은 object를 처음 배치할 Mesh와 현재 owner에게 도달할 물리 route를 나타내는 속성 | 39-40 | |
| 13-mesh-node-R14 | 같은 process에는 같은 MeshName의 MeshNode를 하나만 등록할 수 있음 | 42-43 | |
| 13-mesh-node-R15 | 서로 다른 MeshName의 MeshNode는 여러 개 등록할 수 있지만 Framework가 RouteMesh 사이의 transport relay를 자동으로 만들지 않음 | 43-44 | |
| 13-mesh-node-R16 | ChannelName을 추가해도 별도 socket이나 endpoint가 생기지 않음 | 46 | |
| 13-mesh-node-R17 | MeshNode descriptor를 게시한 뒤에는 Channel membership, Object role, Factory, Stable type과 type capability를 바꿀 수 없음 | 46-52 | |
| 13-mesh-node-R18 | Automatic discovery를 사용하는 MeshNode의 RID는 Framework가 lifecycle마다 새로 만듦; caller는 진단용 prefix만 지정 가능; prefix 생략 시 listener 종류에 맞는 기본 prefix 사용 | 58-60 | |
| 13-mesh-node-R19 | Prefix 계약: ASCII [A-Za-z0-9._-] 문자만 사용, 길이 1..64자 | 64 | |
| 13-mesh-node-R20 | UUID 계약: RFC 4122 UUID v4 bit layout 사용하는 16-byte random value를 8-4-4-4-12 자리 36자 lowercase canonical 문자열로 표현 | 65 | |
| 13-mesh-node-R21 | Full RID 형식: `prefix-<lowercase-canonical-uuid-v4>`, UTF-8 encode 크기 255 bytes 이하 | 66 | |
| 13-mesh-node-R22 | Core binary RID, Framework prefix, Entry Spot과 caller-provided RID 전체 규칙은 10-network-listener-identity §7이 정의 | 68-69 | |
| 13-mesh-node-R23 | Prefix와 UUID를 object placement, shard 또는 재시작 뒤에도 유지되는 application identity로 해석하지 않음 | 71-72 | |
| 13-mesh-node-R24 | MeshNode descriptor owner를 확정하는 CAS는 같은 (MeshName, RID)를 현재 다른 owner가 사용하고 있는지 확인 | 74-75 | |
| 13-mesh-node-R25 | Active conflict가 확인되면 기존 descriptor를 변경하지 않고 두 번째 UUID나 claim을 만들지 않음; startup은 즉시 configuration error로 끝남 | 75-76 | |
| 13-mesh-node-R26 | Replacement lifecycle은 이전 lifecycle의 RID를 재사용하지 않고 새 RID를 만듦 | 78 | |
| 13-mesh-node-R27 | Object Server MeshNode는 같은 diagnostic prefix로 Entry Spot ID도 발급 | 82 | |
| 13-mesh-node-R28 | MeshNode RID 형식: `<prefix>-<node-uuid-v4>`; Entry Spot ID 형식: `<prefix>-entry-<lowercase-canonical-uuid-v4>` | 85-87 | |
| 13-mesh-node-R29 | MeshNode와 Entry Spot은 각각 별도의 UUID v4를 생성하며 두 UUID 값 비교로 관계를 판정하지 않음 | 89 | |
| 13-mesh-node-R30 | Entry Spot ID는 같은 MeshNode lifecycle 동안 유지하고 replacement lifecycle에서는 새로 발급 | 90 | |
| 13-mesh-node-R31 | Global Spot ID authority의 active conflict가 확인되면 두 번째 UUID나 reservation을 만들지 않고 startup을 즉시 configuration error로 끝냄 | 91-92 | |
| 13-mesh-node-R32 | Full Entry Spot ID는 UTF-8 255 bytes 이하여야 함 | 94 | |
| 13-mesh-node-R33 | Prefix를 생략하면 MeshNode automatic RID에 선택한 기본 diagnostic prefix를 Entry Spot에도 사용 | 94-95 | |
| 13-mesh-node-R34 | MeshNode descriptor는 exact Entry Spot ID를 lifecycle generation과 함께 게시 | 97 | |
| 13-mesh-node-R35 | Actor placement와 Entry Spot join은 이 mapping을 사용하며 prefix나 entry marker를 parsing해 node 관계를 계산하지 않음 | 97-98 | |
| 13-mesh-node-R36 | Prefix와 marker는 진단 정보이며 stable host identity, shard나 placement key가 아님 | 99 | |
| 13-mesh-node-R37 | Fixed RID는 Location Store의 MeshNode descriptor와 automatic discovery를 사용하지 않는 explicit manual topology에서만 허용 | 103-104 | |
| 13-mesh-node-R38 | Object role이 Client 또는 Server인 MeshNode에 fixed RID를 설정하거나 automatic mode와 fixed RID를 함께 설정하면 startup configuration error | 106-107 | |
| 13-mesh-node-R39 | MeshNode마다 object role을 한 번 선택 | 111 | |
| 13-mesh-node-R40 | [표 4] None: Logical object operation 제공 안 함, factory·Entry Spot 만들지 않음, 새 object 배치 target 후보에서 제외 | 115 | |
| 13-mesh-node-R41 | [표 4] Client: Object create, find와 message를 시작할 수 있음, factory·Entry Spot 만들지 않음, placement 후보에서 제외 | 116 | |
| 13-mesh-node-R42 | [표 4] Server: Client 기능 모두 포함, 등록한 type의 object와 Entry Spot을 host, 등록한 type과 placement 조건이 맞으면 후보가 됨 | 117 | |
| 13-mesh-node-R43 | Client와 Server role에는 Location Store가 필요함 | 119 | |
| 13-mesh-node-R44 | None을 선택하면 object manager, factory, placement 기능이나 숨겨진 local object runtime을 만들지 않음 | 119-120 | |
| 13-mesh-node-R45 | Factory와 Entry Spot은 Server builder에서만 등록 가능 | 121 | |
| 13-mesh-node-R46 | Entry Spot ID는 Framework가 발급하며 caller가 생성하거나 fixed RID를 지정할 수 없음 | 121-122 | |
| 13-mesh-node-R47 | Object Client는 Spot·Actor factory와 application Node direct handler를 등록할 수 없음 | 124-125 | |
| 13-mesh-node-R48 | Object 기능과 독립된 RouteMesh Channel Server는 같은 MeshNode에 등록할 수 있음; 이 조합은 object placement target이 아니지만 Channel target은 됨 | 125-127 | |
| 13-mesh-node-R49 | 두 MeshNode가 모두 Object Client이고 양쪽 모두 RouteMesh Channel Server membership이 없을 때만 peer connection을 만들지 않음 | 128-129 | |
| 13-mesh-node-R50 | 어느 한쪽에 Server membership이 있으면 weight가 0이어도 연결함 | 129-130 | |
| 13-mesh-node-R51 | Channel Client membership만 있는 pair는 연결하지 않음 | 130-131 | |
| 13-mesh-node-R52 | Actor, User Spot과 Instance Spot factory 등록 시 UTF-8 1..255 bytes stable type과 relocation 방식(DisableRelocation·RecreateOnRelocation·PreserveStateWith 중 하나)을 반드시 함께 지정 | 185-190 | |
| 13-mesh-node-R53 | Framework는 factory 등록 호출 안에서 configure callback을 동기적으로 한 번 실행 | 191 | |
| 13-mesh-node-R54 | Callback이 정상 반환하면 구성을 고정하며, 이후 보관해 둔 builder를 다시 호출하면 configuration error | 192-193 | |
| 13-mesh-node-R55 | Callback이 예외를 던지면 해당 factory를 등록하지 않고 그 예외를 호출자에게 전달 | 193-194 | |
| 13-mesh-node-R56 | Stable type은 대소문자를 구분하는 exact value; Framework는 normalization을 적용하지 않으며 언어의 class FQN을 wire나 Store identity로 사용하지 않음 | 196-197 | |
| 13-mesh-node-R57 | 같은 (object kind, stable type) 조합을 두 번 등록하면 startup 오류 | 197-198 | |
| 13-mesh-node-R58 | Relocation policy를 생략하는 overload나 compatibility default는 제공하지 않음 | 200 | |
| 13-mesh-node-R59 | Object Server의 MeshNode descriptor에는 node 전체에 적용할 placement weight, Actor·Spot capacity projection과 등록한 type별 capability가 포함됨 | 204-205 | |
| 13-mesh-node-R60 | Placement weight 범위 0..10000, 기본값 100; Channel weight와는 별개; 범위 밖 값은 startup 설정과 runtime 변경에서 configuration error | 211 | |
| 13-mesh-node-R61 | Node별 Actor limit: 기본값 0=제한 없음, 양수면 1..2^31-1 범위 최대 Actor 수, 음수는 startup configuration error | 212 | |
| 13-mesh-node-R62 | Node별 Spot limit: 기본값 0=제한 없음, 양수 범위 1..2^31-1, User Spot과 Instance Spot을 합산, 음수는 startup configuration error | 213 | |
| 13-mesh-node-R63 | Spot stable type별 limit: 기본값 0=제한 없음, 양수 범위 1..2^31-1, 해당 User·Instance Spot type에 적용, 음수는 startup configuration error | 214 | |
| 13-mesh-node-R64 | Entry Spot: Object Server node마다 하나로 고정, configurable Spot limit에서 제외 | 215 | |
| 13-mesh-node-R65 | Pending activation 기본값 128, object population이 아니라 동시에 진행되는 activation admission을 제한 | 216 | |
| 13-mesh-node-R66 | 새 object를 만들거나 다른 node로 옮길 때 Framework가 target MeshNode를 선택; Placement weight가 0인 MeshNode는 이 두 작업의 새 target 후보에서 제외 | 218-220 | |
| 13-mesh-node-R67 | 이미 그 MeshNode에 존재하는 object로 보내는 message는 target을 새로 선택하는 작업이 아니므로 weight만으로 차단하지 않음 | 222-223 | |
| 13-mesh-node-R68 | Weight를 0으로 바꿔도 이미 확정된 reservation을 취소하지 않음 | 223-224 | |
| 13-mesh-node-R69 | Framework는 Active count와 reserved slot을 합해 설정한 Actor·Spot limit을 먼저 검사하고 그 뒤에 weight를 적용 | 226-227 | |
| 13-mesh-node-R70 | Limit 0은 검사를 생략 | 227 | |
| 13-mesh-node-R71 | Capacity 조건을 만족하는 node가 하나도 없으면 CapacityExceeded | 228 | |
| 13-mesh-node-R72 | Descriptor의 count는 후보 선택용 projection이며 Location Store의 atomic reservation이 최종 판정 | 228-229 | |
| 13-mesh-node-R73 | 남은 후보의 positive placement weight 합계는 최소 64-bit 정수로 계산하여 overflow하지 않게 함 | 230-231 | |
| 13-mesh-node-R74 | Startup builder, runtime option, MeshNode descriptor와 monitoring snapshot은 같은 weight와 capacity 값을 사용 | 233-234 | |
| 13-mesh-node-R75 | Logical create의 caller는 target RID, predicate 또는 placement callback을 지정하지 않음 | 238-239 | |
| 13-mesh-node-R76 | InMesh를 지정한 경우에도 target node가 아니라 후보를 찾을 Mesh만 선택 | 239-240 | |
| 13-mesh-node-R77 | Target 선택 조건 순서: (1)Serving 상태 확인 (2)Current owner lease 유효성 확인 (3)요청한 object kind·stable type 등록 여부 확인 (4)Active·pending capacity 확인 (5)남은 후보의 node-wide placement weight 적용 | 244-248 | |
| 13-mesh-node-R78 | Framework는 선택한 node에 object를 배치할 자리를 예약하여 다른 생성 작업과 capacity를 중복 사용하지 않게 함 | 250-251 | |
| 13-mesh-node-R79 | Application에 target RID나 owner token을 선택하도록 요구하지 않음 | 251-252 | |
| 13-mesh-node-R80 | GetOrCreate가 이미 Ready인 object를 찾았다면 current owner의 capacity와 weight를 다시 적용하지 않음 | 254-255 | |
| 13-mesh-node-R81 | Startup 순서: (1)MeshName·object role·routing mode·endpoint·Channel set·factory·stable type·relocation policy·factory option과 capacity를 검증 | 261-262 | |
| 13-mesh-node-R82 | Startup 순서: (2)Location Store가 필요한 role이면 host owner lease를 확보하고 automatic RID의 MeshNode descriptor owner CAS를 완료 | 263-264 | |
| 13-mesh-node-R83 | Startup 순서: (3)ROUTER를 bind한 뒤 다른 peer에 게시할 실제 endpoint를 확정 | 265 | |
| 13-mesh-node-R84 | Startup 순서: (4)필요한 정보를 모두 포함한 MeshNode descriptor를 게시하고 어떤 peer와 연결해야 하는지 계산 | 266-267 | |
| 13-mesh-node-R85 | Startup 순서: (5)Peer admission, local handler와 object runtime 준비를 마친 뒤 Serving 상태와 신규 target selection을 공개 | 268-269 | |
| 13-mesh-node-R86 | Object role을 사용하는 host는 Location Store를 명시적으로 등록해야 함 | 271 | |
| 13-mesh-node-R87 | Manual mode에서는 application이 endpoint를 제공; expected RID 사용 시 expected RID와 endpoint를 함께 제공 | 273-274 | |
| 13-mesh-node-R88 | Manual mode는 object runtime을 제공하지 않음 | 275 | |
| 13-mesh-node-R89 | Peer handshake에서 교환하는 정보: MeshName, RID, Lifecycle generation, Descriptor revision, 변경할 수 없는 ChannelName set, Security identity | 281-288 | |
| 13-mesh-node-R90 | MeshName 또는 trust profile이 다르거나 같은 lifecycle identity의 중복 pipe이면 admission하지 않음 | 290-291 | |
| 13-mesh-node-R91 | Lifecycle generation은 0이 아닌 opaque equality token; 숫자 크기로 어느 lifecycle이 더 새로운지 판단하지 않음 | 293-294 | |
| 13-mesh-node-R92 | Manual topology 재연결 시 다음 3조건을 모두 만족한 뒤 다른 generation의 connection을 target selection에 포함: (1)Application 구성에 해당 peer와 연결하려는 의도 (2)인증된 connection handover 완료 (3)Service liveness 확인으로 이전 pipe 종료 확정 | 296-301 | |
| 13-mesh-node-R93 | Automatic RouteMesh에서는 RID가 더 작은 MeshNode만 connect를 시작 | 303-304 | |
| 13-mesh-node-R94 | Manual 양방향 connect 또는 automatic 연결 경합으로 중복 후보가 생기면 07-channel-topology §5.1 규칙에 따라 ready connection 하나만 유지 | 304-306 | |
| 13-mesh-node-R95 | Handshake는 Channel별 weight도 전달 | 310 | |
| 13-mesh-node-R96 | Channel weight를 실행 중에 바꾸면 lifecycle generation은 유지하고 descriptor revision만 증가 | 310-311 | |
| 13-mesh-node-R97 | Peer는 현재 generation에서 더 큰 revision이 붙은 전체 weight snapshot만 적용 | 313 | |
| 13-mesh-node-R98 | Weight 변경은 connection을 다시 만들거나 application message를 replay하지 않으며, node-wide placement weight도 바꾸지 않음 | 314-315 | |
| 13-mesh-node-R99 | [표 7.3] Node direct: Caller가 지정한 MeshName 안에서 exact target RID로 한 번 제출; Object Client RID는 application Node direct target이 아님 | 321 | |
| 13-mesh-node-R100 | [표 7.3] Channel: Process-local ChannelName index가 RouteMesh를 정함; ready 상태이고 Channel weight>0인 Server 중 하나를 weight 비율에 따라 선택 | 322 | |
| 13-mesh-node-R101 | [표 7.3] Logical Multicast: 해당 ChannelName에 참여하고 ready 상태이며 Channel weight>0인 remote MeshNode를 모두 선택; 각 수신 MeshNode는 local Spot 중 ChannelName·topic 조건이 일치하는 subscription에 message 전달 | 323 | |
| 13-mesh-node-R102 | [표 7.3] Actor direct: Global ActorId의 current Ready authority 확인 후 current owner route로 제출 | 324 | |
| 13-mesh-node-R103 | [표 7.3] Spot direct: Global SpotId의 current Ready authority 확인 후 current owner route로 제출 | 325 | |
| 13-mesh-node-R104 | Actor·Spot direct는 logical ID만 target으로 사용; ObjectGeneration 사용처는 18-object-routing §2.5가 정함 | 327-330 | |
| 13-mesh-node-R105 | Target 선택과 message submit은 하나의 operation; Framework가 선택한 RID 목록을 application에 반환한 뒤 별도 send를 요구하지 않음 | 332-333 | |
| 13-mesh-node-R106 | Node·Channel·Actor·Spot send와 request는 같은 MeshNode ROUTER를 사용 | 335 | |
| 13-mesh-node-R107 | Classic fanout은 별도의 PUB/SUB socket 계약이며 MeshNode membership에 포함하지 않음 | 336 | |
| 13-mesh-node-R108 | Node direct는 exact MeshName과 RID가 operation 의미에 포함되는 infrastructure, 진단 또는 manual topology에 사용 | 338-339 | |
| 13-mesh-node-R109 | 여러 node가 같은 기능을 제공하는 application request에는 ChannelName을 사용 | 339-340 | |
| 13-mesh-node-R110 | Actor와 Spot 메시징은 global ActorId 또는 SpotId를 target으로 사용; Caller는 NodeRid나 MeshName을 target으로 넘기지 않음 | 342-343 | |
| 13-mesh-node-R111 | 기존 Actor·Spot의 current MeshName과 NodeRid는 Location Store authority가 제공 | 345-346 | |
| 13-mesh-node-R112 | Missing Instance Spot에서만 Spot direct fluent call의 Instance intent에 optional initial Mesh와 stable type을 지정할 수 있음 | 346-347 | |
| 13-mesh-node-R113 | Initial Mesh는 cold activation placement에만 사용하며 기존 owner의 현재 Mesh를 제한하거나 이동시키지 않음 | 347-348 | |
| 13-mesh-node-R114 | Application payload는 owner의 application turn에서 직렬로 처리 | 350 | |
| 13-mesh-node-R115 | Request completion과 liveness·admission·relocation·reply recovery service control은 기존 Completion connection에서 받음 | 350-351 | |
| 13-mesh-node-R116 | Core HWM 재시도는 binding operation별 completion으로 끝나며 Framework send-ready callback이나 waiter는 없음 | 352 | |
| 13-mesh-node-R117 | Location reconcile과 reservation 같은 Framework 내부 작업도 application handler가 대기 중이어도 진행 | 353-354 | |
| 13-mesh-node-R118 | Actor·Spot lifecycle application callback은 application turn에서 실행 | 354 | |
| 13-mesh-node-R119 | Transport readiness callback에서 application handler를 직접 실행하지 않음 | 355 | |
| 13-mesh-node-R120 | ChannelName handler와 RID direct handler는 서로 다른 namespace를 사용 | 357 | |
| 13-mesh-node-R121 | Channel handler context는 ChannelName과 reply source identity를 내부에 보존; 업무 코드에 MeshName이나 물리 route 선택을 노출하지 않음 | 359-361 | |
| 13-mesh-node-R122 | RID direct handler context는 direct route의 MeshName과 source RID를 제공 | 361-362 | |
| 13-mesh-node-R123 | Spot Logical Multicast는 (ChannelName, topic filter) subscription을 node-local로 검사 | 363-364 | |
| 13-mesh-node-R124 | 송신 MeshNode는 target Channel의 remote node마다 routed message를 한 번 제출 | 364-365 | |
| 13-mesh-node-R125 | 수신 MeshNode는 일치하는 local Spot마다 같은 immutable message storage의 reference를 확보하여 Spot queue에 넣음 | 365-366 | |
| 13-mesh-node-R126 | Relocating node는 다음 신규 작업의 target에서 제외: Channel selection, Object create와 membership, Relocation | 370-374 | |
| 13-mesh-node-R127 | 아직 relocation permit을 얻지 못한 unit의 existing owner message와 timer는 계속 처리 | 376-377 | |
| 13-mesh-node-R128 | Unit별 seal을 마치고 source의 application dispatch를 모두 닫으면 Draining으로 전환 | 377-378 | |
| 13-mesh-node-R129 | 이미 reservation을 끝낸 create, accepted message, completion과 relocation barrier는 정해진 deadline과 fence에 따라 terminal 상태까지 진행 | 380-381 | |
| 13-mesh-node-R130 | 전체 종료와 handoff 순서는 30-host-relocation-flow가 정의 | 381-382 | |
| 13-mesh-node-R131 | Shutdown은 새 relocation을 시작하지 않음 | 384 | |
| 13-mesh-node-R132 | Relocate는 등록한 relocation policy에 따라 Actor, User Spot aggregate와 Instance Spot을 이전 | 384-385 | |
| 13-mesh-node-R133 | Node weight를 0으로 바꾸거나 drain을 시작했다는 이유로 기존 object를 숨겨 다시 만들거나 application payload를 다른 owner에게 새 operation으로 제출하지 않음 | 387-388 | |
| 13-mesh-node-R134 | Runtime snapshot과 event 제공 정보: MeshName·RID·lifecycle generation·endpoint, Object role과 node-wide placement weight, Active·pending·maximum capacity, Type capability와 reservation failure, Drain state | 394-398 | |
| 13-mesh-node-R135 | RID와 endpoint는 진단 정보로만 사용하며 metric label로 사용하지 않음 | 400-401 | |
| 13-mesh-node-R136 | [검증] 같은 process의 중복 MeshName과 잘못된 object role 구성이 startup에서 실패 | 405 | |
| 13-mesh-node-R137 | [검증] None, Client, Server가 manager, factory와 placement capability를 계약대로 제한 | 406 | |
| 13-mesh-node-R138 | [검증] Object role과 Location Store, automatic discovery와 fixed RID의 잘못된 조합이 startup에서 실패 | 407 | |
| 13-mesh-node-R139 | [검증] Automatic RID가 prefix와 lowercase canonical UUID v4 형식을 따르고 active conflict에서 두 번째 claim 없이 startup configuration error로 실패 | 408-409 | |
| 13-mesh-node-R140 | [검증] Replacement lifecycle이 새 RID를 사용 | 410 | |
| 13-mesh-node-R141 | [검증] Entry Spot ID가 MeshNode와 같은 diagnostic prefix, 별도 UUID v4를 사용하며 descriptor가 exact lifecycle mapping을 게시 | 411-412 | |
| 13-mesh-node-R142 | [검증] Replacement lifecycle이 새 Entry Spot ID를 발급하고 Entry Spot authority 충돌에서 즉시 실패 | 413 | |
| 13-mesh-node-R143 | [검증] Stable type 중복과 relocation policy 생략이 startup에서 실패 | 414 | |
| 13-mesh-node-R144 | [검증] Placement weight는 0, 기본값 100과 상한 10000을 허용하고 -1과 10001은 startup 설정과 runtime 변경에서 거부 | 415-416 | |
| 13-mesh-node-R145 | [검증] Capacity가 weight보다 먼저 적용되고 weight 0이 existing object와 accepted reservation을 취소하지 않음 | 417 | |
| 13-mesh-node-R146 | [검증] Channel weight 변경이 placement weight를 바꾸지 않음 | 418 | |
| 13-mesh-node-R147 | [검증] Channel select-one이 Channel weight와 drain을 반영하고 Node direct에는 영향을 주지 않음 | 419 | |
| 13-mesh-node-R148 | [검증] Logical Multicast가 remote node마다 한 번 전송되고 node-local Spot queue가 immutable storage를 공유 | 420 | |
| 13-mesh-node-R149 | [검증] ChannelName handler와 RID direct handler의 namespace 및 context가 구분됨 | 421 | |
| 13-mesh-node-R150 | [검증] Draining node가 새 placement target이 되지 않고 accepted operation은 terminal 상태까지 진행 | 422 | |
| 13-mesh-node-R151 | [검증] Actor·Spot application 호출이 NodeRid나 owner token을 target으로 요구하지 않음 | 423 | |

### 14-actor-model

| # | 규칙 (수치·상태·오류) | 옛 위치 | 새 위치 |
|---|---|---|---|
| 14-actor-model-R1 | 이 장은 Actor의 identity, 위치, message queue, lifecycle과 session binding을 정의한다 | 9-16 | |
| 14-actor-model-R2 | Actor가 어디에 있든 application payload는 Actor 자신의 queue에 제출한다 | 18-19 | |
| 14-actor-model-R3 | Queue에 들어온 handler를 실행할 gate는 현재 Spot membership과 User Spot execution mode가 결정한다 | 19-20 | |
| 14-actor-model-R4 | 관련 계약 소유 문서 표: MeshNode route/peer admission → 13장, Spot membership transaction/relocation → 15장, STREAM session 연동 → 20장, Payload/metadata → 04장, Callback 실행/completion → 05장 | 22-30 | |
| 14-actor-model-R5 | Actor는 Location Store namespace 전체에서 유일한 logical ActorId로 식별하는 stateful object다 | 36-37 | |
| 14-actor-model-R6 | ActorId는 UTF-8 1..255 bytes이며 대소문자를 구분하는 exact value다 | 39 | |
| 14-actor-model-R7 | Framework는 Unicode normalization이나 case folding을 적용하지 않는다 | 40 | |
| 14-actor-model-R8 | MeshName은 Actor identity에 포함되지 않는다 | 42-43 | |
| 14-actor-model-R9 | 같은 ActorId를 서로 다른 Mesh마다 중복 생성할 수 없다 | 43 | |
| 14-actor-model-R10 | Actor type은 UTF-8 1..255 bytes의 stable name이다 | 45 | |
| 14-actor-model-R11 | 언어의 class 이름이나 generic type 이름을 Store/wire identity로 사용하지 않는다 | 46-47 | |
| 14-actor-model-R12 | 같은 server에 같은 stable type을 두 번 등록하면 startup 오류다 | 47-48 | |
| 14-actor-model-R13 | ActorRef는 특정 시점의 Actor 위치를 나타내는 변경할 수 없는 snapshot이다 | 52 | |
| 14-actor-model-R14 | ActorRef 필드: ActorId(logical identity), ObjectGeneration(0이 아닌 unsigned 63-bit), 현재 MeshName, 현재 NodeRid | 54-59 | |
| 14-actor-model-R15 | ObjectGeneration은 같은 ActorId의 서로 다른 logical incarnation을 구분한다 | 57 | |
| 14-actor-model-R16 | RecreateOnRelocation은 같은 incarnation을 계속 쓰므로 ObjectGeneration을 바꾸지 않는다 | 57 | |
| 14-actor-model-R17 | ActorRef는 Actor message의 target으로 사용하는 값이 아니다 | 61-62 | |
| 14-actor-model-R18 | Actor가 이동/재생성되면 이전 ActorRef는 stale할 수 있다 | 62 | |
| 14-actor-model-R19 | ObjectGeneration은 JSON에서 decimal string으로 표현한다 | 64 | |
| 14-actor-model-R20 | 별도의 ActorRefSnapshot public type은 제공하지 않는다 | 65 | |
| 14-actor-model-R21 | Actor는 Spot membership과 STREAM binding, 두 상태를 서로 독립적으로 관리한다 | 69 | |
| 14-actor-model-R22 | Spot membership 가능 상태: Entry Spot, user Spot, 이동 중 | 73 | |
| 14-actor-model-R23 | STREAM binding 가능 상태: unbound, bound | 74 | |
| 14-actor-model-R24 | Actor가 user Spot에 존재하기 위해 bound session이 필요하지 않다 | 76-77 | |
| 14-actor-model-R25 | Session bind/unbind는 Actor의 현재 Spot을 바꾸지 않는다 | 77 | |
| 14-actor-model-R26 | 한 Actor는 동시에 session 하나에만 bind할 수 있다 | 79 | |
| 14-actor-model-R27 | 한 session에는 여러 Actor를 bind할 수 있다 | 80 | |
| 14-actor-model-R28 | 모든 Actor application payload는 target Actor의 application queue에 직접 제출한다 (Entry/user Spot/remote MeshNode 모두 동일) | 84-86 | |
| 14-actor-model-R29 | 같은 Actor queue가 수락한 payload는 Actor turn에서 순서대로 처리한다 | 88 | |
| 14-actor-model-R30 | Entry Spot Actor와 PerActor User Spot Actor는 Actor별 gate를 사용하며 서로 다른 Actor는 독립 실행 가능하다 | 89-90 | |
| 14-actor-model-R31 | SpotWide User Spot의 member Actor는 User Spot 공통 execution gate를 사용, 같은 User Spot의 Actor·Spot handler·timer·lifecycle callback은 전체에서 한 번에 하나만 실행한다 | 91-93 | |
| 14-actor-model-R32 | Yield는 SpotWide User Spot의 공통 gate 실행 중일 때만 사용 가능; Entry Spot Actor와 PerActor User Spot Actor에는 제공하지 않는다 | 94-95 | |
| 14-actor-model-R33 | SpotWide member Actor가 Yield해도 현재 Actor queue head claim은 유지한다; 다른 Actor·Spot handler·timer는 반납된 gate 사용 가능하나 같은 Actor의 다음 payload는 continuation 종료까지 시작하지 않는다 | 96-98 | |
| 14-actor-model-R34 | Actor send/request, STREAM session relay, Actor 간 호출은 모두 같은 Actor queue로 들어간다 | 99-100 | |
| 14-actor-model-R35 | Actor payload를 Spot application queue에 넣거나 Spot callback으로 변환하지 않는다 | 101 | |
| 14-actor-model-R36 | Actor Join은 JoinSpot(...) 또는 JoinEntrySpot(...) call의 Defer()로 등록한다 | 109 | |
| 14-actor-model-R37 | Defer()는 현재 handler가 끝난 뒤 Join을 실행하도록 예약하는 동기 terminal이다 | 110 | |
| 14-actor-model-R38 | 호출한 자리에서는 target을 찾거나 Store에 접근하지 않는다 | 111 | |
| 14-actor-model-R39 | 현재 handler에 변경할 수 없는 Join 요청을 기록하고 이 Actor의 다음 message가 Join보다 먼저 실행되지 않도록 비활성 queue barrier만 등록한다 | 111-113 | |
| 14-actor-model-R40 | Join call에는 Async, await, submit, coroutine terminal과 Yield를 제공하지 않는다 | 115-116 | |
| 14-actor-model-R41 | Defer() 자체도 Spot gate나 Actor queue claim을 반납하지 않는다 | 116 | |
| 14-actor-model-R42 | 현재 handler는 계속 실행하며, 마지막 awaited continuation까지 정상적으로 끝나야 Framework가 barrier를 활성화하고 Join을 시작한다 | 117-118 | |
| 14-actor-model-R43 | Handler가 exception이나 cancellation으로 끝나면 그 handler가 등록한 비활성 barrier를 모두 폐기한다 | 118-119 | |
| 14-actor-model-R44 | Registration은 Actor exact generation, current membership, immutable request snapshot, absolute deadline과 non-zero 128-bit operation ID를 고정한다 | 121-122 | |
| 14-actor-model-R45 | 한 handler는 Join을 최대 64개까지 등록할 수 있다 | 124 | |
| 14-actor-model-R46 | Join request 하나의 encoded 크기는 최대 1 MiB | 124-125 | |
| 14-actor-model-R47 | 같은 handler가 등록한 모든 Join request의 합계는 최대 8 MiB | 125-126 | |
| 14-actor-model-R48 | Request를 생략하면 empty ZLinkMessage를 고정한다 | 126 | |
| 14-actor-model-R49 | 각 Defer()는 request를 변경할 수 없는 snapshot으로 만들고 monotonic clock 기준 absolute deadline을 계산한다 | 127-128 | |
| 14-actor-model-R50 | Timeout 기본값은 5초 | 128 | |
| 14-actor-model-R51 | 명시한 timeout 값은 millisecond로 올림한 1..INT_MAX 범위의 유한한 값이어야 한다 | 128-129 | |
| 14-actor-model-R52 | 제한을 넘긴 현재 registration은 일부 record를 남기지 않고 동기 startup configuration error로 실패한다 | 129-130 | |
| 14-actor-model-R53 | Cross-node Join의 application reply도 최대 1 MiB | 132 | |
| 14-actor-model-R54 | Request와 reply의 크기 제한은 서로 독립적 | 132-133 | |
| 14-actor-model-R55 | Crash recovery를 위해 request/reply 크기 제한을 하나의 1 MiB로 합치지 않는다 | 133-134 | |
| 14-actor-model-R56 | Actor send/request handler와 User·Entry Spot의 packet·request·subscription·timer handler에서 local member Actor의 Join을 등록할 수 있다 | 136-137 | |
| 14-actor-model-R57 | Factory, Configure, lifecycle callback, relocation adapter, detached task, Instance Spot handler, Framework가 관리하지 않는 thread에서는 Join 등록이 InvalidOperation이다 | 137-138 | |
| 14-actor-model-R58 | 같은 call의 두 번째 Defer()는 InvalidOperation | 138-139 | |
| 14-actor-model-R59 | 같은 Actor의 다른 pending membership transition이 있으면 Unavailable | 139 | |
| 14-actor-model-R60 | Handler registration scope: handler 실행 중과 Framework가 추적하는 awaited continuation 동안 열려 있음 | 141-143 | |
| 14-actor-model-R61 | Scope가 닫힌 뒤 Defer() 호출하면 InvalidOperation | 143 | |
| 14-actor-model-R62 | Detached task에서 Defer() 호출은 계약 위반이며 Framework는 모든 언어에서 이 오용을 handler 종료 전에 발견한다고 보장하지 않는다 | 144-146 | |
| 14-actor-model-R63 | Handler turn, 비활성 barrier와 scope는 현재 process 메모리에만 유지한다 | 148-149 | |
| 14-actor-model-R64 | Join 실행이나 Location Store commit 전에 process가 종료되면 이 registration과 completion을 재생하지 않으며 source authority와 membership을 그대로 유지한다 | 149-150 | |
| 14-actor-model-R65 | Registration 뒤 source seal 전에 도착한 payload는 barrier 뒤의 Actor queue가 수락한다 | 152-153 | |
| 14-actor-model-R66 | Cross-node relocation에서는 이 payload도 accepted journal과 아직 실행하지 않은 queue 작업과 함께 target으로 옮긴다 | 153-154 | |
| 14-actor-model-R67 | Source seal 뒤 commit 전 payload는 relocation ingress hold에 보관한다 | 156-157 | |
| 14-actor-model-R68 | Commit 끝난 뒤 이전 owner에 도착한 payload는 Message Follow로 새 owner에 전달한다 | 157-158 | |
| 14-actor-model-R69 | 같은 handler가 barrier 등록한 Actor에 request를 보내고 reply 기다리면 순환 대기가 생기며 Framework는 제출 전에 InvalidOperation으로 거부한다 | 160-162 | |
| 14-actor-model-R70 | Join과 maintenance 경쟁 시 먼저 확정한 제어 상태를 따른다 | 164 | |
| 14-actor-model-R71 | Join claim이 Relocate보다 먼저면 maintenance는 Join이 terminal 상태가 될 때까지 기다린다 | 164-165 | |
| 14-actor-model-R72 | Relocate seal이 먼저면 Join은 Unavailable로 실패 | 166 | |
| 14-actor-model-R73 | Shutdown admission seal이 먼저면 Join은 ShuttingDown으로 실패 | 166-167 | |
| 14-actor-model-R74 | Actor가 이미 요청한 User Spot에 속해 있거나 Entry Spot Actor가 다시 JoinEntrySpot을 호출하면 실제 위치를 바꾸지 않고 Accepted completion을 실행한다 | 169-171 | |
| 14-actor-model-R75 | 이 no-op case에서는 Location Store와 membership을 변경하지 않으며 join·joined·leave lifecycle callback도 실행하지 않는다 | 171-172 | |
| 14-actor-model-R76 | Request handler가 application reply를 encoding하지 못하면 handler failure로 처리하여 비활성 barrier를 폐기한다 | 174-175 | |
| 14-actor-model-R77 | Encoding이 끝난 뒤 caller가 연결을 종료했거나 transport가 reply를 수락하지 못한 경우 이미 등록한 Join을 취소하지 않는다 | 175-177 | |
| 14-actor-model-R78 | Actor handler는 Actor 자신의 mutable state를 소유한다 | 179 | |
| 14-actor-model-R79 | Room/stage/zone 등 Spot 소유 상태를 읽거나 바꾸려면 Actor handler가 명시적인 Spot send/request를 제출해야 한다; 이 작업은 target Spot turn에서 실행 | 179-181 | |
| 14-actor-model-R80 | Actor handler는 containing Spot object를 받는다 | 183 | |
| 14-actor-model-R81 | SpotWide에서는 shared gate 안에서 Spot state를 사용할 수 있다 | 183-184 | |
| 14-actor-model-R82 | PerActor와 Entry에서는 containing Spot의 mutable state를 직접 공유하지 않고 명시적 Spot send/request를 사용한다 | 184-185 | |
| 14-actor-model-R83 | Actor Ready notification, request 완료, relocation 단계 전환, session binding 진행은 Framework가 전용 queue에서 처리하며 업무 handler queue와 분리되어 있다 | 187-190 | |
| 14-actor-model-R84 | Spot은 Actor application payload를 처리하지 않는다 | 194 | |
| 14-actor-model-R85 | Spot turn에서 처리하는 Actor 관련 작업은 membership과 lifecycle control뿐이다 | 194-195 | |
| 14-actor-model-R86 | Control 작업 표: Join(membership 허용 판단/갱신), Leave(membership 해제/상태 정리), Relocation prepare·commit·abort(일관된 상태 변경), Actor lifecycle notification(후속 작업 실행) | 197-202 | |
| 14-actor-model-R87 | Framework는 lifecycle 작업을 target Spot의 전용 queue에 넣고 같은 Spot의 다른 callback과 하나씩 실행하여 두 callback이 Spot 상태를 동시에 바꾸지 않는다 | 204-205 | |
| 14-actor-model-R88 | Actor가 소유한 상태를 바꾸는 lifecycle 작업도 Actor의 전용 queue에서 하나씩 실행한다 | 207-208 | |
| 14-actor-model-R89 | Actor·Spot 양쪽 상태를 함께 바꾸는 순서와 오래된 owner 변경 거부 규칙은 15장(Spot Actor)이 정의한다 | 208-209 | |
| 14-actor-model-R90 | Lifecycle queue와 application payload queue가 함께 실행 가능하면 lifecycle queue를 먼저 실행한다 | 211-212 | |
| 14-actor-model-R91 | 이 우선순위는 Join이 끝나기 전에 그 Actor 앞으로 온 payload를 실행하거나, leave 확정 뒤 payload를 실행하는 것을 막기 위함 | 212-213 | |
| 14-actor-model-R92 | 이 우선순위는 두 queue 사이에만 적용하며 각 queue 안의 수락 순서는 바꾸지 않는다 | 213-214 | |
| 14-actor-model-R93 | 이 우선순위는 절대 우선순위가 아니다 | 216-217 | |
| 14-actor-model-R94 | 서로 다른 두 상한이 관여하며 섞으면 안 된다: owner 점유 상한(서로 다른 owner 사이, 06장 정의), lifecycle 연속 실행 상한(같은 owner 안 두 lane 사이, 이 절 정의) | 219-222 | |
| 14-actor-model-R95 | owner 점유 상한에 도달하면 그 owner 전체가 turn을 놓고 다른 ready owner가 실행한다 | 224 | |
| 14-actor-model-R96 | owner 점유 상한만으로는 lane 사이 굶주림을 막지 못한다 — 같은 우선순위 규칙이 lifecycle을 다시 고르기 때문 | 225-226 | |
| 14-actor-model-R97 | lifecycle lane에 연속 실행 상한과 양보 부채를 따로 둔다 | 228 | |
| 14-actor-model-R98 | 연속 실행 상한은 lifecycle lane을 연속으로 고른 turn 수로 센다 (시간이 아니라 turn 수) | 230-232 | |
| 14-actor-model-R99 | 규칙1: lifecycle lane을 고를 때마다 연속 횟수를 하나 올린다 | 234 | |
| 14-actor-model-R100 | 규칙2: 연속 횟수가 상한에 도달하면 그 owner에 양보 부채를 표시하고 횟수를 0으로 되돌린다 | 235 | |
| 14-actor-model-R101 | 규칙3: 양보 부채가 있는 owner는 application lane이 ready인 한 이 owner가 turn을 얻을 때 application lane을 먼저 실행한다 | 236-237 | |
| 14-actor-model-R102 | 규칙4: application turn을 한 번 실행하면 부채를 지운다 | 238 | |
| 14-actor-model-R103 | 경계조건: lifecycle lane이 비어 application lane을 골랐으면 연속 횟수를 0으로 되돌린다 | 244 | |
| 14-actor-model-R104 | 경계조건: 부채가 있는데 application lane이 ready 아니면 부채를 유지한 채 lifecycle lane을 계속 실행한다(굶주림 없음) | 245 | |
| 14-actor-model-R105 | 경계조건: 다른 owner에게 양보했다가 돌아오면 부채와 연속 횟수를 그대로 유지한다 | 246 | |
| 14-actor-model-R106 | 경계조건: owner가 종료/이동하면 부채와 연속 횟수를 함께 버린다 | 247 | |
| 14-actor-model-R107 | 부채가 붙는 자리는 execution mode에 따라 다르다(12장 §5.4 참조) | 249-250 | |
| 14-actor-model-R108 | SpotWide User Spot/Entry Spot/Instance Spot: 부채는 공유 execution gate 하나에 붙으며 그 gate의 아무 application 작업이나 해소한다 | 252-254 | |
| 14-actor-model-R109 | PerActor User Spot: 부채는 gate마다 따로(Actor gate, Spot lane gate, timer gate); Actor lifecycle 부채는 그 Actor의 application 작업만, Spot lifecycle 부채는 Spot lane의 application 작업만 해소한다 | 255 | |
| 14-actor-model-R110 | PerActor에서 부채를 gate 단위로 두지 않으면 한 Actor의 lifecycle 폭주가 다른 Actor의 turn으로 해소된 것처럼 계산되어 그 Actor 작업이 계속 밀린다 | 257-258 | |
| 14-actor-model-R111 | 이 보장은 아직 정성적이다 — owner 점유 상한에 값/허용범위가 정해지지 않아 "몇 ms 안에 실행"을 판정할 수 없다 | 260-262 | |
| 14-actor-model-R112 | 값이 정해지기 전까지 검증 가능한 것은 "lifecycle 작업이 계속 도착해도 application turn이 실행되기는 한다"까지다 | 262 | |
| 14-actor-model-R113 | Actor send/request의 target은 global ActorId다 | 266 | |
| 14-actor-model-R114 | Framework는 Ready 상태 현재 incarnation과 authority가 가리키는 owner route를 positive route cache 또는 Location Store에서 찾은 뒤 owner fence 확인, target queue에 message 제출한다 | 266-269 | |
| 14-actor-model-R115 | Resolve할 때 확인한 ObjectGeneration은 route snapshot과 stale cache를 구분하는 정보이며 Actor handler의 target 일치 조건이 아니다 | 269-271 | |
| 14-actor-model-R116 | Local Actor와 remote Actor는 handler 실행과 completion에 같은 의미를 사용한다 | 273 | |
| 14-actor-model-R117 | Caller는 MeshName, ActorRef, Owner RID, 현재 Spot ID를 Actor message target으로 지정하지 않는다 | 275-280 | |
| 14-actor-model-R118 | Missing, Creating과 Store failure 결과는 negative cache에 저장하지 않는다 | 284 | |
| 14-actor-model-R119 | Ready 상태 현재 위치를 보관하는 positive cache도 current owner lease의 local admission deadline과 공개 RouteCacheMaxAge 안에서만 사용한다 | 285-286 | |
| 14-actor-model-R120 | 더 큰 StoreVersion, stale result 또는 Store recovery event를 확인하면 positive cache를 즉시 무효화한다 | 287-288 | |
| 14-actor-model-R121 | ObjectGeneration은 Actor direct message의 target 일치 조건이 아니다 | 289 | |
| 14-actor-model-R122 | Resolve 뒤 같은 owner에서 Actor가 destroy되고 같은 ActorId로 다시 만들어졌다면 target queue가 수락하는 시점의 current Ready Actor가 message를 처리한다 | 290-291 | |
| 14-actor-model-R123 | Resolve한 owner가 더 이상 해당 ActorId를 소유하지 않으면 현재 operation은 stale route 오류로 끝낸다 | 292-293 | |
| 14-actor-model-R124 | Framework는 Location Store에서 새 owner를 찾아 같은 operation을 자동으로 다시 보내지 않는다 | 293-294 | |
| 14-actor-model-R125 | Request timeout이나 실행 여부를 알 수 없는 실패가 발생해도 Framework가 자동으로 재전송하지 않는다 | 295-296 | |
| 14-actor-model-R126 | Actor direct messaging은 session binding을 만들거나 바꾸지 않는다 | 297 | |
| 14-actor-model-R127 | Framework는 Actor type, message kind와 packet name으로 handler를 선택한다 | 301 | |
| 14-actor-model-R128 | 같은 Actor handler namespace에 같은 key를 두 번 등록하면 startup 오류다 | 302 | |
| 14-actor-model-R129 | Handler instance와 scoped dependency는 hosting Spot이 아니라 해당 Actor activation이 소유한다 | 305-306 | |
| 14-actor-model-R130 | 서로 다른 Actor가 같은 handler instance를 공유하지 않으며 relocation과 cross-node Join 뒤 target Actor activation에서 다시 만든다 | 306-307 | |
| 14-actor-model-R131 | Actor와 Actor Context는 composition 관계다 | 311 | |
| 14-actor-model-R132 | Framework는 factory 호출 전에 ActorId, ObjectGeneration, current MeshName, nullable current SpotId, bound-session capability를 가진 exact Context를 만든다 | 311-313 | |
| 14-actor-model-R133 | Factory는 ID를 별도 인자로 받지 않고 이 Context만 받는다 | 313 | |
| 14-actor-model-R134 | 반환한 Actor는 전달받은 Context를 read-only Context member로 그대로 노출해야 하며 Configure()는 Context 인자를 받지 않는다 | 314 | |
| 14-actor-model-R135 | 다른 Context를 반환하면 staging Actor를 Ready로 공개하지 않는다 | 315 | |
| 14-actor-model-R136 | Same-node Join은 Actor instance와 Context를 유지하고 membership commit에서 SpotId만 바꾼다 | 317-318 | |
| 14-actor-model-R137 | Cross-node Join은 Actor ID와 ObjectGeneration을 유지하되 target owner와 membership에 결합한 새 Context를 target factory에 전달한다 | 318-319 | |
| 14-actor-model-R138 | Commit 뒤 source Context의 identity는 source leave callback까지 읽을 수 있지만 새 send/request/session mutation/Join은 Unavailable로 끝나며 current target으로 자동 전달하지 않는다 | 319-321 | |
| 14-actor-model-R139 | Object Server는 Actor stable type, Factory, factory configure callback에서 선택하는 relocation policy를 함께 등록한다 | 327-331 | |
| 14-actor-model-R140 | Relocation policy를 생략하는 overload나 compatibility default는 제공하지 않는다 | 333 | |
| 14-actor-model-R141 | PreserveStateWith 선택 시 해당 Actor type에 맞는 ActorRelocationAdapter를 같은 등록에서 제공해야 한다 | 335-336 | |
| 14-actor-model-R142 | Adapter는 Actor 상태를 application만 해석하는 byte sequence로 저장/복원; Framework는 이 byte sequence 내용을 해석하지 않으며 별도 state contract ID도 관리하지 않는다 | 336-338 | |
| 14-actor-model-R143 | PreserveStateWith는 source handler가 정상적으로 끝난 시점의 application state를 capture하여 target Actor에 복원한다 | 340-341 | |
| 14-actor-model-R144 | RecreateOnRelocation은 target에서 Actor 객체를 다시 만들지만 application state를 복원하지 않는다; 대신 Framework 소유 실행 전 queue와 timer 정보는 이동 후에도 유지 | 341-343 | |
| 14-actor-model-R145 | 두 policy 모두 같은 logical Actor의 이동이므로 ObjectGeneration을 바꾸지 않는다 | 343-344 | |
| 14-actor-model-R146 | Cross-node 이동에서 owner가 바뀌면 AuthorityOwnerGeneration만 증가한다 | 344-345 | |
| 14-actor-model-R147 | 이동하는 Actor가 Session에 bind되어 있으면 target에서 Actor를 복원, owner·membership commit, queue 병합, regular route 전환, lifecycle callback을 끝낸 뒤 Actor dispatch를 연다 | 347-348 | |
| 14-actor-model-R148 | 그 뒤 target runtime이 command 44 sessionRelocationRoute commit을 one-way로 Session owner에 보내 binding route를 target owner로 갱신한다 | 349-350 | |
| 14-actor-model-R149 | binding route는 Session owner가 현재 Actor owner에 message를 보낼 때 사용하는 전달 경로다 | 350-351 | |
| 14-actor-model-R150 | Route switch와 함께 bound-session accessor가 반환하는 current Actor location snapshot도 같은 ActorId·ObjectGeneration을 유지한 채 target MeshName·NodeRid로 갱신한다 | 351-353 | |
| 14-actor-model-R151 | 같은 Session에서 relocation 대상에 포함되지 않은 다른 Actor의 route와 physical STREAM connection은 유지한다 | 353-354 | |
| 14-actor-model-R152 | Session owner는 exact Session·binding·Actor generation과 relocation identity를 확인하고 held message를 target route로 제출한 뒤 matching seal을 해제한다 | 354-355 | |
| 14-actor-model-R153 | 적용 reply는 없다 | 356 | |
| 14-actor-model-R154 | exact command 44가 기본 3,000ms SessionRelocationSealTimeout 안에 없으면 physical Session과 관련 state를 정리한다 | 356-357 | |
| 14-actor-model-R155 | Target Actor는 command 44 적용 reply 없이 message를 처리한다 | 357-358 | |
| 14-actor-model-R156 | Route update는 같은 ObjectGeneration의 relocation에만 허용하며 application은 relocation을 알기 위해 rebind하지 않는다 | 358-359 | |
| 14-actor-model-R157 | 새 incarnation은 explicit bind가 필요하다 | 359 | |
| 14-actor-model-R158 | Actor manager의 Create와 GetOrCreate는 한 번만 제출할 수 있는 fluent call이다 | 391 | |
| 14-actor-model-R159 | Create/GetOrCreate required 값: ActorId, Stable Actor type | 394-396 | |
| 14-actor-model-R160 | Create/GetOrCreate optional 값: InMesh, Encoded creation request, Timeout | 397-401 | |
| 14-actor-model-R161 | Caller는 target RID, predicate, factory class 또는 placement callback을 지정할 수 없다 | 403-404 | |
| 14-actor-model-R162 | 같은 option을 두 번 설정하면 InvalidOperation | 406 | |
| 14-actor-model-R163 | Terminal submit을 두 번 실행하면 InvalidOperation | 406-407 | |
| 14-actor-model-R164 | Terminal submit을 시작할 때 end-to-end deadline 하나를 고정하며 resolve, reservation, factory 실행과 Ready barrier 전체에 적용된다 | 409-410 | |
| 14-actor-model-R165 | InMesh 지정 시 해당 Mesh 사용; 생략 시 규칙: Object Client/Server role Mesh가 하나면 자동 선택, 후보 없으면 NotConfigured, 후보 둘 이상이면 InvalidOperation, InMesh로 지정한 Mesh가 없으면 NotFound | 474-481 | |
| 14-actor-model-R166 | Framework의 target 후보 검사 순서: 1)Object role 확인 2)stable type 등록 여부 확인 3)active·pending capacity 확인 4)남은 후보에 node-wide placement weight 적용 | 483-488 | |
| 14-actor-model-R167 | Caller는 target node나 endpoint를 선택하지 않는다 | 490 | |
| 14-actor-model-R168 | Encoded creation request는 최대 1 MiB | 494 | |
| 14-actor-model-R169 | Framework는 reservation 시작 전 내용이 바뀌지 않는 creation request reference와 hash를 durable creation intent에 기록한다 | 494-496 | |
| 14-actor-model-R170 | 이 정보는 Actor가 Ready가 되거나 fence가 적용된 실패 정리를 마칠 때까지 유지한다 | 496-497 | |
| 14-actor-model-R171 | 여러 node가 동시 생성 시도해도 authority CAS에서 이긴 node만 creation request를 factory에 전달한다 | 499-500 | |
| 14-actor-model-R172 | Factory는 같은 (ActorId, ObjectGeneration, creation attempt)에 대해 한 번 이상 실행될 수 있다; 따라서 같은 attempt 재실행에도 안전해야 한다 | 502-503 | |
| 14-actor-model-R173 | Factory가 만든 Actor는 아직 외부에 공개되지 않은 staging instance다 | 505 | |
| 14-actor-model-R174 | Entry Spot의 OnCreateActor가 creation request를 확인하고 Accepted 또는 Rejected와 optional application reply를 반환한다 | 505-507 | |
| 14-actor-model-R175 | Accepted이면 initial Entry membership과 Ready authority를 commit하고 Created를 publish; 이 최초 생성 과정에서는 OnActorJoin과 joined notification을 호출하지 않는다 | 509-511 | |
| 14-actor-model-R176 | Rejected이면 Ready authority와 message admission을 만들지 않는다; Staging Actor, Creating authority와 reserved capacity를 정리하고 Rejected를 publish | 512-513 | |
| 14-actor-model-R177 | Callback exception은 application이 선택한 Rejected가 아니라 typed creation failure로 처리하고 attempt를 Aborted로 끝낸다 | 514-515 | |
| 14-actor-model-R178 | Actor를 Ready로 공개한 뒤 destroy하는 방식으로 거절을 구현하지 않는다 | 517-518 | |
| 14-actor-model-R179 | 거절된 Actor는 Find로 조회할 수 없고 message를 받을 수 없으며 active capacity를 소비하지 않는다 | 518-519 | |
| 14-actor-model-R180 | Exclusive Create 실행 시 같은 type의 Ready Actor가 이미 있으면 AlreadyExists로 끝난다 | 523-524 | |
| 14-actor-model-R181 | 다른 type의 Actor가 있으면 TypeMismatch로 끝난다 | 524-525 | |
| 14-actor-model-R182 | GetOrCreate는 같은 type의 Ready Actor 있으면 새 reservation과 callback 실행 없이 현재 incarnation을 Existing으로 반환한다 | 527-528 | |
| 14-actor-model-R183 | 같은 type Actor가 Creating이면 그 상태가 끝날 때까지 bounded backoff로 authority를 다시 확인한다 | 528-529 | |
| 14-actor-model-R184 | 현재 attempt가 Ready로 끝나면 Existing 반환; 거절/실패 정리로 Missing이 되면 남은 deadline 안 새 reservation 경쟁 | 529-531 | |
| 14-actor-model-R185 | 동일 ActorId에 여러 process가 동시 GetOrCreate 호출해도 Location Store의 reservation CAS에 성공한 caller만 생성 실행을 소유한다 | 533-535 | |
| 14-actor-model-R186 | 서로 다른 operation은 앞선 attempt의 terminal state나 application reply를 공유하지 않는다 | 535-536 | |
| 14-actor-model-R187 | 앞선 attempt가 Rejected 또는 실패로 끝나면 다음 reservation winner가 자신의 creation request로 factory와 callback을 실행한다 | 536-537 | |
| 14-actor-model-R188 | 상태 다이어그램: Missing → Reserved(R1) → {Created(R1,ActorRef,ReplyRef?) / Rejected(R1,ReplyRef?) / Failed(R1,Failure)}; Creating(R1) observed by B → Ready:Existing 또는 Missing after cleanup: Reserve(R2) and run B callback | 539-549 | |
| 14-actor-model-R189 | Created 상태 의미: Callback이 생성을 승인했고 Actor와 initial Entry membership이 Ready로 commit됐다 | 555 | |
| 14-actor-model-R190 | Rejected 상태 의미: Callback이 정상적으로 생성 요청을 거절했다. Ready authority와 active capacity는 만들지 않는다 | 556 | |
| 14-actor-model-R191 | Failed 상태 의미: Node 종료, timeout 또는 callback exception으로 정상적인 application 결과를 만들지 못했다 | 557 | |
| 14-actor-model-R192 | Existing 상태 의미: 이미 Ready인 Actor를 조회한 결과. 새 reservation과 callback 실행이 없다 | 558 | |
| 14-actor-model-R193 | Location Store는 (source Node RID, source lifecycle generation, OperationId)로 식별한 operation terminal record를 원래 deadline 뒤 5분까지 유지한다 | 560-561 | |
| 14-actor-model-R194 | 같은 operation의 중복 전달만 이 record를 읽어 이전 결과를 재사용한다 | 561-562 | |
| 14-actor-model-R195 | 새 operation은 retained terminal record를 읽지 않고 current authority를 기준으로 다시 판단한다 | 562-563 | |
| 14-actor-model-R196 | Terminal record에는 request correlation이나 reply route가 없는 creation-operation-terminal-v1 semantic envelope와 SHA-256을 저장한다 | 565-566 | |
| 14-actor-model-R197 | 같은 operation 재처리 시 Framework는 현재 request의 correlation과 reply route로 새 command reply를 encode한다 | 566-568 | |
| 14-actor-model-R198 | Envelope에는 Created·Rejected·failure 결과와 optional application reply를 포함하며 encoded size는 최대 1 MiB | 568-569 | |
| 14-actor-model-R199 | Actor 생성 terminal을 보존하기 위해 Relocation Store를 사용하지 않는다 | 570 | |
| 14-actor-model-R200 | Creating 상태를 재확인하던 caller가 deadline에 도달하면 DeadlineExceeded로 끝나지만 생성 attempt가 실패했다고 간주하지 않는다 | 572-574 | |
| 14-actor-model-R201 | 다음 call은 Location Store의 current authority와 retained terminal record를 다시 확인한다 | 574-575 | |
| 14-actor-model-R202 | Manager의 Find(ActorId)는 Ready 상태인 current authority의 ActorRef를 반환한다 | 579 | |
| 14-actor-model-R203 | Find는 Actor 생성을 시작하지 않으며 별도의 Actor directory도 제공하지 않는다 | 580 | |
| 14-actor-model-R204 | Actor를 user Spot으로 옮기는 join·leave·relocation은 15장의 fencing과 barrier를 따른다 | 584-585 | |
| 14-actor-model-R205 | 이동 중에 수락한 payload를 이전 Spot callback으로 보내지 않는다; Payload는 Actor queue에서 순서를 유지한다 | 587-588 | |
| 14-actor-model-R206 | Actor 종료는 새로운 payload admission을 닫고 session binding과 location ownership을 정리한다 | 592-593 | |
| 14-actor-model-R207 | Bound session의 연결이 종료되었다는 이유만으로 Actor를 자동 종료하거나 현재 Spot에서 자동 leave하지 않는다 | 593-594 | |
| 14-actor-model-R208 | Lifecycle 종료를 허용하는 정확한 상태와 transaction은 15장(Spot Actor)이 정의한다 | 596-597 | |
| 14-actor-model-R209 | Actor destroy는 exact ActorRef를 받는다 | 599 | |
| 14-actor-model-R210 | Actor가 user Spot에 있으면 먼저 leave 또는 Entry Spot join을 완료해야 한다 | 599-600 | |
| 14-actor-model-R211 | Destroy는 membership 이동이 아니다. 따라서 성공 과정에서 OnLeaveActor를 다시 호출하지 않는다 | 602-603 | |
| 14-actor-model-R212 | Destroy 순서: 1)새 payload admission을 닫는다 2)진행 중인 lifecycle 작업을 정리한다 3)Session binding을 제거한다 4)Location ownership과 registry entry를 제거한다 | 605-610 | |
| 14-actor-model-R213 | 같은 incarnation이 이미 없으면 Idempotent false를 반환한다 | 613-614 | |
| 14-actor-model-R214 | 같은 ActorId의 다른 generation이 있으면 InvalidOperation으로 끝난다 | 615 | |
| 14-actor-model-R215 | Actor가 이동을 위한 seal 상태면 Unavailable로 끝난다 | 616 | |
| 14-actor-model-R216 | Framework는 current ActorRef를 다시 찾아 새 incarnation을 종료하지 않는다 | 618 | |
| 14-actor-model-R217 | Session binding은 Actor와 현재 STREAM session 사이의 runtime 관계다. Binding token은 재연결과 늦게 도착한 이전 session 작업을 구분한다 | 622-623 | |
| 14-actor-model-R218 | Actor handler는 현재 bound session을 사용해 Client로 one-way push 전송, Session 연결 종료 요청을 할 수 있다 | 625-628 | |
| 14-actor-model-R219 | Session에서 들어와 Actor로 향하는 payload도 Actor queue에 직접 제출한다 | 630-631 | |
| 14-actor-model-R220 | Spot membership은 route와 lifecycle 검증에 사용할 수 있지만 payload를 Spot callback으로 보내는 근거로 사용하지 않는다 | 631-632 | |
| 14-actor-model-R221 | Bind, rebind, disconnect와 request correlation은 20장(Session Actor Dispatch)이 정의한다 | 634-635 | |
| 14-actor-model-R222 | 실패 표: Logical ActorId에 Ready authority가 없으면 Actor target 오류로 끝난다 | 643 | |
| 14-actor-model-R223 | 실패 표: Exact-ref operation에서 mapping이 없으면 Unavailable | 644 | |
| 14-actor-model-R224 | 실패 표: Exact-ref의 generation이 current generation과 다르면 InvalidOperation | 645 | |
| 14-actor-model-R225 | 실패 표: Actor가 commit 전 seal 상태면 Unavailable | 646 | |
| 14-actor-model-R226 | 실패 표: Bound session이 필요한 작업에 유효한 binding이 없으면 InvalidOperation | 647 | |
| 14-actor-model-R227 | Handler가 없거나 decode가 실패하거나 application handler가 예외를 반환하면 request는 복원 가능한 reply route로 오류를 반환한다 | 649-650 | |
| 14-actor-model-R228 | One-way message는 runtime 관측 경로에 오류를 기록한다 | 650-651 | |
| 14-actor-model-R229 | Drain 중에는 새로운 Actor 생성과 membership 배정을 막는다 | 653 | |
| 14-actor-model-R230 | 이미 수락한 Actor turn과 control transaction은 deadline까지 진행한다 | 654 | |
| 14-actor-model-R231 | Runtime은 다음을 서로 구분하여 관측할 수 있어야 한다: Current MeshName과 Actor type, Application queue와 control backlog, ObjectGeneration, Membership state, Session-binding state, Dispatch 결과 | 658-665 | |
| 14-actor-model-R232 | ActorId는 metric label로 사용하지 않는다 | 667 | |
| 14-actor-model-R233 | Contract test: Entry Spot과 user Spot의 Actor payload가 모두 Actor queue로 직접 전달된다 | 671 | |
| 14-actor-model-R234 | Contract test: Actor payload가 Spot callback이나 Spot application queue를 거치지 않는다 | 672 | |
| 14-actor-model-R235 | Contract test: Spot lifecycle 전용 queue에는 join·leave·relocation과 lifecycle control만 넣으며 Actor 업무 payload를 넣지 않는다 | 673-674 | |
| 14-actor-model-R236 | Contract test: Inbound dispatch가 Actor application instance를 찾기 전에 현재 relocation temporary queue 등록 여부를 확인, 있으면 그 queue에 넣고 없으면 기존 Actor dispatch 사용 | 675-676 | |
| 14-actor-model-R237 | Contract test: Restore 중 도착한 message를 temporary queue에서 실행하지 않는다. Commit 뒤 saved work, boundary 전 relay와 나머지 temporary 작업을 실제 Actor queue에 순서대로 넣고 regular route로 전환. 필요한 lifecycle 작업 끝낸 뒤 dispatch를 연다 | 677-679 | |
| 14-actor-model-R238 | Contract test: Temporary queue 제거와 regular route 전환을 atomic하게 처리하여 message가 중복되거나 누락되지 않게 한다 | 680-681 | |
| 14-actor-model-R239 | Contract test: Relocation Restore가 relay-ready reply accepted 전에 명시적으로 실패하면 target temporary queue를 실행하지 않고 폐기하며 source가 소유한 원본을 되돌린다. 그 뒤에는 cutover submit 결과와 관계없이 source를 복원하지 않는다 | 682-684 | |
| 14-actor-model-R240 | Contract test: 같은 RelocationId, target attempt와 owner generation의 Restore를 여러 번 받아도 temporary queue와 application instance를 한 번만 만든다. 이전 attempt의 temporary queue는 사용하지 않는다 | 685-686 | |
| 14-actor-model-R241 | Contract test: 같은 Actor의 payload가 ingress 종류와 관계없이 Actor queue 수락 순서대로 실행된다 | 687 | |
| 14-actor-model-R242 | Contract test: SpotWide member Actor가 Yield하면 User Spot gate만 반납하고 Actor queue claim을 유지한다. 다른 Actor·Spot·timer는 진행하지만 같은 Actor의 다음 job은 진행하지 않는다 | 688-689 | |
| 14-actor-model-R243 | Contract test: 같은 Actor 자신에게 보낸 request가 Yield 뒤에도 현재 job을 앞질러 실행하거나 inline으로 재진입하지 않는다 | 690-691 | |
| 14-actor-model-R244 | Contract test: Actor Join은 Async와 Yield를 제공하지 않고 handler 안에서 동기 Defer()로 등록한다. 결과는 Actor completion callback으로 전달한다 | 692-693 | |
| 14-actor-model-R245 | Contract test: Defer()는 target 조회나 Store I/O 없이 Join intent와 비활성 barrier만 등록하며 handler가 정상적으로 끝난 뒤에만 Join을 실행한다 | 694-696 | |
| 14-actor-model-R246 | Contract test: Handler당 최대 64개, request 하나당 최대 1 MiB, request 합계 최대 8 MiB와 기본 5초 timeout을 적용한다 | 697-698 | |
| 14-actor-model-R247 | Contract test: Same-node Join, cross-node Join과 RecreateOnRelocation policy에서 같은 logical incarnation의 ObjectGeneration을 유지한다 | 699-701 | |
| 14-actor-model-R248 | Contract test: Actor handler가 mutable Spot state에 직접 접근하지 않고 명시적인 Spot 호출을 사용한다 | 702 | |
| 14-actor-model-R249 | Contract test: Session bind와 Spot membership이 독립적으로 바뀌며 서로를 암묵적으로 변경하지 않는다 | 703 | |
| 14-actor-model-R250 | Contract test: 같은 ActorId를 서로 다른 MeshName에 중복 생성하지 않는다 | 704 | |
| 14-actor-model-R251 | Contract test: Actor messaging이 ActorId만 받고 owner route와 generation을 application에 요구하지 않는다 | 705 | |
| 14-actor-model-R252 | Contract test: 동시에 같은 Actor 생성을 요청해도 생성 권한을 얻지 못한 target은 factory를 추가로 실행하지 않고 같은 attempt의 완료를 기다린다 | 706-707 | |
| 14-actor-model-R253 | Contract test: Creating을 관찰한 서로 다른 operation은 Ready 뒤 Existing을 받고, rejection·failure cleanup 뒤 새 reservation을 경쟁하며 앞선 application reply를 공유하지 않는다 | 708-709 | |
| 14-actor-model-R254 | Contract test: 같은 source Node RID·lifecycle generation·OperationId의 재전송만 correlation-free semantic terminal envelope를 읽고 현재 correlation·reply route로 reply를 다시 encode한다 | 710-711 | |
| 14-actor-model-R255 | Contract test: Rejected와 Aborted가 Ready authority와 active capacity를 만들지 않고 reserved capacity를 반환한다 | 712-713 | |
| 14-actor-model-R256 | Contract test: Terminal record가 original deadline 뒤 5분 동안 같은 operation의 replay를 허용하고, TTL 뒤 Ready authority가 없으면 새 reservation으로 다시 생성할 수 있다 | 714-715 | |
| 14-actor-model-R257 | Contract test: Destroy가 exact generation을 검사하고 새 incarnation으로 retarget하지 않는다 | 716 | |

### 15-spot-actor

| # | 규칙 (수치·상태·오류) | 옛 위치 | 새 위치 |
|---|---|---|---|
| 15-spot-actor-R1 | 이 문서는 Actor 생성, Spot membership, relocation과 aggregate relocation을 정의한다 | 13-15 | |
| 15-spot-actor-R2 | 자동 failover(Process 종료 뒤 다른 runtime이 relocation을 이어받는 것)는 계약에 포함하지 않는다 | 9-10, 14-15 | |
| 15-spot-actor-R3 | Core는 raw socket과 transport만 제공한다. Object의 membership, relocation 상태와 lifecycle은 각 언어의 Framework runtime이 관리한다 | 17-18 | |
| 15-spot-actor-R4 | ActorId와 Entry·User·Instance Spot ID는 Location Store namespace 전체에서 전역인 logical key다 | 22-23 | |
| 15-spot-actor-R5 | MeshName은 authority key에는 포함되지 않는다 | 23-24 | |
| 15-spot-actor-R6 | Location Store는 각 logical key마다 현재 object를 어느 node가 처리하는지와 Actor가 어느 Spot에 속하는지를 기록한다(=authority) | 26-27 | |
| 15-spot-actor-R7 | Object가 다른 node로 이동하면 logical key는 그대로 유지하면서 현재 owner 정보만 새 값으로 바꾼다 | 28-29 | |
| 15-spot-actor-R8 | ActorRef와 SpotRef의 ObjectGeneration은 0이 아닌 unsigned 63-bit conceptual value다 | 31-32 | |
| 15-spot-actor-R9 | 같은 incarnation에서 membership이나 owner MeshNode가 바뀌어도 ObjectGeneration은 유지한다 | 32-33 | |
| 15-spot-actor-R10 | 대신 provider가 더 큰 AuthorityOwnerGeneration을 발급하여 새 owner를 구분한다 | 33-34 | |
| 15-spot-actor-R11 | Authority 기록 항목: Current owner, Spot membership, ObjectGeneration, AuthorityOwnerGeneration, StoreVersion, Exact owner lease | 36-45 | |
| 15-spot-actor-R12 | Runtime route cache는 authority row의 snapshot일 뿐이며 cache만으로 current authority를 결정하지 않는다 | 47-48 | |
| 15-spot-actor-R13 | Join, leave, relocation, destroy와 close는 expected StoreVersion, generation과 owner lease를 검증하는 transaction만 사용한다 | 49-50 | |
| 15-spot-actor-R14 | Object Client 또는 Server role은 Location Store가 필수다 | 52 | |
| 15-spot-actor-R15 | Store가 없으면 startup에서 거부하며 hidden local Store, runtime-local object manager와 같은 이름의 축소된 의미를 만들지 않는다 | 52-54 | |
| 15-spot-actor-R16 | Object role이 None인 manual topology는 Node direct와 Channel operation만 사용할 수 있다 | 54 | |
| 15-spot-actor-R17 | 여러 node가 같은 Actor/Spot을 동시에 만들려고 해도 factory는 생성 권한을 얻은 한 곳에서만 시작해야 한다 | 58-59 | |
| 15-spot-actor-R18 | Framework는 Location Store에 생성할 object와 target node의 capacity를 함께 예약(=placement reservation)하여 이 권한을 하나로 확정한다 | 59-61 | |
| 15-spot-actor-R19 | Actor·User Spot manager create: Coordinator가 target transport로 요청 보내기 전에 reservation을 요청한다 | 68 | |
| 15-spot-actor-R20 | Instance Spot direct cold activation: Source가 최초 message와 생성 정보를 target에 먼저 보내고, target에 사용할 Spot이 없으면 target이 Location Store에 자신에게 만들어도 되는지 요청한다 | 69 | |
| 15-spot-actor-R21 | 두 방식의 공통 결과: Location Store가 한 target에만 생성 권한을 주고 다른 target은 별도 factory를 시작하지 않는다 | 71-72 | |
| 15-spot-actor-R22 | Object factory를 등록하고 같은 type인지 비교할 때 사용하는 변경되지 않는 이름을 stable type이라 한다 | 74-75 | |
| 15-spot-actor-R23 | Remote User Spot manager create는 reservation 뒤 exact target에 별도 terminal service operation을 보낸다 | 77 | |
| 15-spot-actor-R24 | 이 operation은 source/target node lifecycle, global Spot key·stable type, provider가 발급한 reservation, StoreVersion과 deadline을 고정한다 | 78-79 | |
| 15-spot-actor-R25 | Target은 Reserved allocation의 pendingCreation을 Location Store에서 exact read한 뒤 factory·initialize·Commit을 실행한다 | 80-81 | |
| 15-spot-actor-R26 | Location row polling이나 application control packet은 terminal result가 아니다 | 81-82 | |
| 15-spot-actor-R27 | Remote User Spot close도 exact SpotRef, owner generation·StoreVersion과 target lifecycle을 가진 별도 terminal service operation이다 | 84-85 | |
| 15-spot-actor-R28 | Target은 active Actor membership과 relocation 상태를 admission 전에 확인한다 | 85-86 | |
| 15-spot-actor-R29 | 7-step reservation flow: 1)global key/stable type/optional Mesh·placement/durable creation input 고정 후 positive weight 후보 선택 2)coordinator 또는 target activation registry가 Reserve 호출 3)Store Reserve가 Missing→Creating 전환 4)예약 성공한 target만 factory/initialize 실행 5)승인 시 terminal Commit이 fence를 Ready로 바꾸고 Reserved→Active 전환, Created publish 6)거절 시 terminal Commit이 Ready/active capacity 안 만들고 Creating authority/reserved 정리, Rejected publish 7)Node 종료/timeout/callback exception시 Abort가 Creating authority와 Reserved allocation 정리, Aborted failure publish | 88-108 | |
| 15-spot-actor-R30 | Instance Spot은 별도 application 생성 승인이 없으며 envelope에 포함된 first message를 activation barrier 뒤 local queue에 한 번 제출한다 | 101-102 | |
| 15-spot-actor-R31 | Reservation에 들어가는 정보: object kind, global key, stable type, target descriptor, typed capacity bundle, exact owner lease, StoreVersion | 111-113 | |
| 15-spot-actor-R32 | 고정 만료 시간인 TTL로 생성 권한을 판단하지 않는다 | 113-114 | |
| 15-spot-actor-R33 | Store에 기록한 Creating 상태와 target owner lease를 함께 확인하여 생성 복구, 다른 target의 인계와 취소 여부를 결정한다 | 114-115 | |
| 15-spot-actor-R34 | Actor와 Spot은 이 공통 reservation operation을 함께 사용한다 | 115-116 | |
| 15-spot-actor-R35 | Encoded creation request는 최대 1 MiB | 118 | |
| 15-spot-actor-R36 | Framework는 reservation 전에 변경할 수 없는 content reference와 hash를 creation intent에 기록하고 object가 Ready가 되거나 실패한 생성을 정리할 때까지 유지한다 | 118-120 | |
| 15-spot-actor-R37 | 생성 권한을 얻은 target만 이 request를 factory에 전달한다 | 120-121 | |
| 15-spot-actor-R38 | Factory와 initialize는 (logical key, ObjectGeneration, attempt) 기준으로 한 번 이상 실행될 수 있으므로 같은 입력의 재실행을 안전하게 처리해야 한다 | 121-123 | |
| 15-spot-actor-R39 | Actor factory가 만든 staging instance는 Entry Spot의 OnCreateActor에 전달한다 | 125 | |
| 15-spot-actor-R40 | Callback은 승인 여부와 optional reply를 반환한다 | 125-126 | |
| 15-spot-actor-R41 | 승인하면 initial Entry membership·Ready authority·active capacity와 Created terminal record를 함께 공개한다 | 126-127 | |
| 15-spot-actor-R42 | 거절하면 Ready와 message admission을 열지 않고 Creating authority·pending capacity를 정리하면서 Rejected terminal record를 공개한다 | 127-128 | |
| 15-spot-actor-R43 | Callback exception은 application rejection이 아니라 기존 typed creation failure다 | 129 | |
| 15-spot-actor-R44 | 동시에 요청했지만 생성 권한을 얻지 못한 caller는 다른 factory를 시작하지 않는다 | 131 | |
| 15-spot-actor-R45 | 서로 다른 operation은 authority 변경을 기다린다 | 132 | |
| 15-spot-actor-R46 | Authority가 Ready가 되면 Existing을 받고, callback rejection·failure cleanup으로 Missing이 되면 새 reservation을 경쟁하여 자신의 creation request를 처리한다 | 132-134 | |
| 15-spot-actor-R47 | 앞선 operation의 Rejected state나 application reply는 공유하지 않는다 | 134-135 | |
| 15-spot-actor-R48 | Terminal call의 deadline 하나가 resolve, 대기, reservation, factory와 Ready 준비 전체에 적용된다 | 136-137 | |
| 15-spot-actor-R49 | Deadline이 끝나면 DeadlineExceeded다 | 137-138 | |
| 15-spot-actor-R50 | 다음 call은 Store의 현재 authority를 다시 확인하여 중단된 attempt를 정리하거나 이어간다 | 138-139 | |
| 15-spot-actor-R51 | Missing, Creating과 Store failure는 negative cache에 저장하지 않는다 | 139-140 | |
| 15-spot-actor-R52 | 동일한 ActorId에 여러 process가 동시에 GetOrCreate를 호출하면 Location Store의 reservation CAS winner만 factory와 OnCreateActor를 실행한다 | 142-143 | |
| 15-spot-actor-R53 | 같은 Actor가 Creating이면 다른 caller는 새 reservation을 만들지 않고 authority 변경을 기다린다 | 144 | |
| 15-spot-actor-R54 | State diagram: Missing → Reserved(R1) → {Created(R1, ActorRef, ReplyRef?) / Rejected(R1, ReplyRef?) / Aborted(R1, Failure)} — 이 문서의 세 번째 leaf는 Aborted(14-actor-model은 Failed) | 146-152 | |
| 15-spot-actor-R55 | Created와 Rejected는 reservation winner operation의 정상 terminal result다 | 154-155 | |
| 15-spot-actor-R56 | Callback exception은 Failed, recovery cleanup은 terminal record를 만들지 않는 Abort다 | 155-156 | |
| 15-spot-actor-R57 | Existing은 Ready Actor를 찾은 다른 operation의 조회 결과이며 새 reservation이나 callback을 만들지 않는다 | 156-157 | |
| 15-spot-actor-R58 | Created terminal publish는 Ready authority와 active capacity 전환을 함께 수행한다 | 159 | |
| 15-spot-actor-R59 | Rejected terminal publish는 Ready authority와 active capacity를 만들지 않고 Creating authority와 reserved capacity를 정리한다 | 160-161 | |
| 15-spot-actor-R60 | Terminal record는 exact source Node RID·lifecycle generation·OperationId로 식별하며 같은 operation의 재전송에만 사용한다 | 161-162 | |
| 15-spot-actor-R61 | Request correlation과 reply route가 없는 creation-operation-terminal-v1 semantic envelope와 SHA-256을 original deadline 뒤 5분까지 보존한다 | 163-164 | |
| 15-spot-actor-R62 | Framework는 재전송 시 현재 request의 correlation과 reply route로 새 command reply를 encode한다 | 165 | |
| 15-spot-actor-R63 | Entry Spot은 startup initialization을 마치기 전 descriptor와 resolver에 publish하지 않는다 | 167 | |
| 15-spot-actor-R64 | Actor creation은 initial Entry Spot membership과 Ready barrier를 같은 lifecycle에서 완료하며 OnActorJoin과 OnJoinedActor를 호출하지 않는다 | 168-169 | |
| 15-spot-actor-R65 | 세 Spot 종류의 생성 방식/기능 차이와 Entry Spot의 전체 역할은 11장(Spot 모델)이 정의한다; 이 절은 Entry·User Spot이 Actor membership을 처리하는 순서만 정의한다 | 173-175 | |
| 15-spot-actor-R66 | Entry Spot의 Actor는 Actor별 execution gate를 사용한다 | 177 | |
| 15-spot-actor-R67 | User Spot의 기본 SpotWide mode에서는 Spot handler, member Actor handler, timer와 lifecycle callback이 User Spot 공통 execution gate를 사용한다 | 178-179 | |
| 15-spot-actor-R68 | Factory 등록에서 PerActor를 선택하면 Actor별 gate, Spot lane과 timer별 gate를 구분하며 서로 다른 gate는 동시에 실행할 수 있다 | 179-181 | |
| 15-spot-actor-R69 | Actor를 만들면 selected owner MeshNode의 Entry Spot이 initial membership을 처리한다 | 183 | |
| 15-spot-actor-R70 | Actor 업무 message는 Actor queue로 직접 전달하며 Entry Spot이나 User Spot callback을 경유하지 않는다 | 184 | |
| 15-spot-actor-R71 | Actor payload를 Actor queue에 넣는 위치와 handler 실행 권한을 결정하는 gate는 서로 다른 계약이다 | 186 | |
| 15-spot-actor-R72 | Yield는 shared gate를 사용하는 SpotWide User Spot에서만 허용한다 | 187-188 | |
| 15-spot-actor-R73 | Entry Spot Actor와 PerActor User Spot에서는 현재 turn을 유지하는 Async만 사용한다 | 188-189 | |
| 15-spot-actor-R74 | Actor Join call은 execution mode와 관계없이 동기 Defer()만 제공한다 | 191 | |
| 15-spot-actor-R75 | Handler는 Defer()를 호출해 Join을 예약하고 barrier를 등록한다 | 192 | |
| 15-spot-actor-R76 | Handler의 마지막 continuation이 정상적으로 끝난 뒤 Framework가 실제 Join을 실행한다 | 192-193 | |
| 15-spot-actor-R77 | Join에는 Async·await·submit과 Yield를 제공하지 않는다 | 193-194 | |
| 15-spot-actor-R78 | Request나 worker의 Yield와 달리 Defer()는 Spot gate와 Actor queue claim을 반납하지 않는다 | 194-195 | |
| 15-spot-actor-R79 | 한 handler는 Join을 최대 64개까지 등록할 수 있다 | 197 | |
| 15-spot-actor-R80 | Join request 하나는 encoded 최대 1 MiB이고, 합계는 최대 8 MiB다 | 197-198 | |
| 15-spot-actor-R81 | 제한을 넘긴 현재 registration은 일부 record를 남기지 않고 동기 startup configuration error로 실패한다 | 199-200 | |
| 15-spot-actor-R82 | Timeout을 생략하면 5초를 사용한다 | 202 | |
| 15-spot-actor-R83 | 명시 값은 millisecond로 올림한 1..INT_MAX 범위의 유한한 값이어야 한다 | 202-203 | |
| 15-spot-actor-R84 | Framework는 Defer()를 호출한 시점에 monotonic clock으로 absolute deadline을 한 번 계산한다 | 203-204 | |
| 15-spot-actor-R85 | 따라서 handler가 Defer() 뒤에 계속 실행한 시간도 Join timeout에 포함된다 | 204-205 | |
| 15-spot-actor-R86 | User Spot은 Join 요청 처리, joined, leave와 disconnected lifecycle control을 해당 Spot control queue에서 직렬화한다 | 207-208 | |
| 15-spot-actor-R87 | 같은 Spot의 packet·timer turn과 callback 순서는 Spot turn이 정한다 | 208-209 | |
| 15-spot-actor-R88 | Instance Spot은 Actor membership target이 아니다 | 209 | |
| 15-spot-actor-R89 | Actor disconnected callback은 physical Session disconnect의 current binding snapshot 또는 public NotifyDisconnected의 명시적 logical notification에서 실행된다 | 211-212 | |
| 15-spot-actor-R90 | Framework는 exact binding identity마다 최대 한 번 실행하며 Actor destroy, leave 또는 membership 변경으로 해석하지 않는다 | 212-213 | |
| 15-spot-actor-R91 | 한 Actor callback failure는 다른 binding 통지와 Session cleanup을 막지 않는다 | 213-214 | |
| 15-spot-actor-R92 | 일반 User Spot Close는 current Actor membership이 하나라도 있으면 public false로 끝나고 admission과 authority를 유지한다 | 216-217 | |
| 15-spot-actor-R93 | Caller가 명시적 leave 또는 destroy를 끝낸 뒤에만 close할 수 있다 | 217 | |
| 15-spot-actor-R94 | Framework는 Actor를 숨겨서 이동하거나 파괴하지 않는다 | 218 | |
| 15-spot-actor-R95 | JoinSpot은 이동할 User Spot의 global Spot ID를 받는다 | 222 | |
| 15-spot-actor-R96 | JoinEntrySpot은 target node RID를 받지 않는다; Framework가 사용할 target Spot과 owner node를 찾는다 | 222-223 | |
| 15-spot-actor-R97 | Actor와 target Spot의 owner node가 다르면 같은 Join operation 안에서 Actor relocation도 수행한다 | 224 | |
| 15-spot-actor-R98 | Application은 relocation 단계, target node, state adapter 또는 owner token을 직접 지정하지 않는다 | 226-227 | |
| 15-spot-actor-R99 | Join request는 선택 사항이다. 생략하면 target User Spot의 OnActorJoin callback에 빈 request를 전달한다 | 267-268 | |
| 15-spot-actor-R100 | Defer()를 호출하면 Framework가 request의 변경할 수 없는 복사본과 absolute deadline을 저장한다 | 268-269 | |
| 15-spot-actor-R101 | 이 request는 Join admission을 판단할 때만 사용하며 relocation state payload로 재사용하지 않는다 | 269-270 | |
| 15-spot-actor-R102 | Defer()는 현재 handler의 registration scope가 열려 있을 때만 호출할 수 있다 | 272 | |
| 15-spot-actor-R103 | Framework가 추적하는 awaited continuation도 같은 scope를 사용한다 | 273 | |
| 15-spot-actor-R104 | Handler가 끝난 뒤 scope가 닫힌 상태에서 호출하면 InvalidOperation | 273-274 | |
| 15-spot-actor-R105 | Handler가 시작한 작업을 기다리지 않고 background의 detached task에서 호출하는 것은 application contract 위반이다 | 274-275 | |
| 15-spot-actor-R106 | Framework는 모든 언어에서 이런 호출을 scope가 닫히기 전에 항상 발견한다고 보장하지 않는다 | 276 | |
| 15-spot-actor-R107 | Handler가 정상적으로 끝나면 Framework는 등록한 barrier를 모두 활성화한다 | 278 | |
| 15-spot-actor-R108 | Handler가 exception, cancellation 또는 request reply encoding failure로 끝나면 barrier를 모두 폐기한다 | 278-280 | |
| 15-spot-actor-R109 | Reply encoding이 끝난 뒤 caller가 연결을 종료했거나 transport가 reply를 수락하지 못해도 Join은 취소하지 않는다 | 280-281 | |
| 15-spot-actor-R110 | Framework는 Join 결과를 0이 아닌 128-bit OperationId와 함께 Actor Join completion callback으로 application에 알린다 | 283-284 | |
| 15-spot-actor-R111 | Accepted는 위치 변경을 commit한 target Actor가 받는다 | 285 | |
| 15-spot-actor-R112 | Rejected와 relay-ready reply가 accepted 상태가 되기 전 Failed는 기존 source Actor가 받는다 | 286 | |
| 15-spot-actor-R113 | 그 뒤 target process가 종료되면 completion callback을 다른 runtime에서 다시 실행하지 않는다 | 287-288 | |
| 15-spot-actor-R114 | Target의 OnJoinedActor callback이 끝나기 전에는 completion callback이나 뒤에 대기한 application payload를 실행하지 않는다 | 290-291 | |
| 15-spot-actor-R115 | Source의 OnLeaveActor notification은 one-way으로 보낸다 | 291-292 | |
| 15-spot-actor-R116 | 이 notification의 완료나 실패는 completion을 막지 않는다 | 292 | |
| 15-spot-actor-R117 | Source에 남은 resource의 별도 cleanup 단계를 Join 절차에 추가하지 않는다 | 293 | |
| 15-spot-actor-R118 | Bound Session 유무와 관계없이 target의 OnJoinedActor callback이 끝나면 target Actor가 Join completion callback으로 Accepted 결과를 받는다 | 295-296 | |
| 15-spot-actor-R119 | 이 callback이 끝난 뒤 target Actor가 대기 중인 message를 처리한다 | 296-297 | |
| 15-spot-actor-R120 | Bound Session이 있으면 relocation을 시작하기 전에 Session owner가 해당 binding을 seal한다 | 297-298 | |
| 15-spot-actor-R121 | Target 준비와 target-only Location Store CAS가 끝난 뒤 Session owner는 binding route와 current ActorRef 위치 snapshot을 target으로 바꾸고 seal을 해제한다 | 298-299 | |
| 15-spot-actor-R122 | Session은 target을 선택하거나 Location Store를 변경하지 않는다 | 300 | |
| 15-spot-actor-R123 | 갱신 전의 route로 도착한 message는 source의 Message Follow route가 target Actor에 전달한다 | 300-301 | |
| 15-spot-actor-R124 | Same-node와 cross-node Join의 completion, OperationId, optional reply와 retry cursor는 현재 source와 target process가 실행되는 동안만 보존한다 | 303-304 | |
| 15-spot-actor-R125 | Relocation payload는 source가 memory에 유지해 정상 handoff에서 state와 queue를 target으로 직접 전달하는 데 사용하며, process 종료 뒤 completion을 자동 replay하는 근거로 사용하지 않는다 | 304-306 | |
| 15-spot-actor-R126 | OperationId는 application이 completion callback 재시도를 같은 작업으로 구분할 때 사용하는 idempotency ID다 | 308-309 | |
| 15-spot-actor-R127 | RelocationId, placement reservation ID, aggregate commit ID와는 다른 값이다 | 309-310 | |
| 15-spot-actor-R128 | Cross-node Accepted의 Relocation manifest에도 별도 field로 저장한다 | 311 | |
| 15-spot-actor-R129 | Completion outcome table: Accepted(target Actor받음, same-target no-op은 현재 Actor; Current ActorRef+optional reply), Rejected(source Actor받음; optional reply), Failed(source Actor 또는 이후 target Actor; typed Framework error kind) | 313-317 | |
| 15-spot-actor-R130 | Target은 queue 개방과 lifecycle 완료 뒤 source에 별도의 완료 reply를 보내지 않는다 | 319 | |
| 15-spot-actor-R131 | Source는 cutover를 one-way로 보낸 뒤 Message Follow 정리를 진행한다 | 319-320 | |
| 15-spot-actor-R132 | Bound Actor라면 target runtime이 Session owner에 target route 적용과 seal 해제를 one-way로 알린다 | 320-321 | |
| 15-spot-actor-R133 | Target의 OnActorJoin callback이 정상적으로 거절한 결과는 오류가 아니므로 Failed가 아니라 Rejected다 | 323-325 | |
| 15-spot-actor-R134 | Failed.Kind 표: NotFound(요청 User Spot 없음), Unavailable(이동 가능 Entry Spot/호환 target node 없음), CapacityExceeded(target node 수용 부족), Rejected(relocation policy가 cross-node 이동 금지), DeadlineExceeded(deadline까지 commit 못함), InternalFailure(Capture·factory·restore·staging 내부 오류), DataLost(chunk 조립/checksum 검증 실패), InvalidOperation(Actor generation 불일치), Unavailable(owner/membership fence 다름 or 이동 중), ShuttingDown(runtime shutdown이 먼저 시작되어 relay-ready accepted 전 중단) | 327-338 | |
| 15-spot-actor-R135 | Accepted는 위치와 membership 변경이 commit되었다는 뜻; Completion callback 실행까지 끝났다는 뜻은 아니다 | 340-341 | |
| 15-spot-actor-R136 | Framework는 lifecycle callback과 source membership cleanup을 처리한 뒤 completion callback을 실행한다 | 341-342 | |
| 15-spot-actor-R137 | Completion이 계속 실패하면 Actor를 sealed 상태로 유지하고 barrier 뒤의 일반 message를 실행하지 않는다 | 342-343 | |
| 15-spot-actor-R138 | Same-node join은 relocation이 아니므로 relocation policy가 DisableRelocation이어도 허용한다 | 345 | |
| 15-spot-actor-R139 | Actor가 이미 요청한 User Spot에 속해있거나 Entry Spot Actor가 다시 JoinEntrySpot 호출하면 Framework는 실제 이동 없이 Accepted completion을 제출한다 | 347-349 | |
| 15-spot-actor-R140 | 이 경우 Location Store, membership과 capacity를 변경하지 않는다. 또한 OnActorJoin, OnJoinedActor와 OnLeaveActor를 호출하지 않는다 | 349-351 | |
| 15-spot-actor-R141 | Join과 host maintenance가 동시에 시작되면 먼저 seal하거나 claim한 작업이 우선한다 | 352 | |
| 15-spot-actor-R142 | Join claim이 Relocate보다 먼저면 maintenance는 Join이 terminal 상태가 될 때까지 기다린다 | 353-354 | |
| 15-spot-actor-R143 | Relocate seal이 먼저면 Join은 Unavailable로 끝난다 | 354 | |
| 15-spot-actor-R144 | Shutdown admission seal이 먼저면 Join은 ShuttingDown으로 끝난다 | 355 | |
| 15-spot-actor-R145 | 같은 handler가 barrier를 등록한 Actor에 request를 보내고 reply를 기다리면 순환이 생길 수 있어 Framework는 제출 전에 InvalidOperation으로 거부한다 | 357-359 | |
| 15-spot-actor-R146 | User Spot은 OnActorJoin에서 Actor를 받을지 결정한다. Entry Spot에는 이 callback이 없다 | 363-364 | |
| 15-spot-actor-R147 | 일반적인 same-node membership 변경에서는 commit 뒤 target Spot의 OnJoinedActor와 source Spot의 OnLeaveActor를 실행한다 | 364-365 | |
| 15-spot-actor-R148 | Cross-node Join에서는 restore 요청과 source relay를 먼저 처리한다 | 366 | |
| 15-spot-actor-R149 | Target restore와 membership commit이 끝난 뒤 target Spot의 OnJoinedActor를 호출하고, source Spot에는 OnLeaveActor를 one-way으로 보낸다 | 366-368 | |
| 15-spot-actor-R150 | 새 Actor를 처음 Entry Spot에 배치할 때는 Entry Spot의 OnCreateActor를 사용한다 | 370 | |
| 15-spot-actor-R151 | Entry Spot에서 User Spot으로 이동할 때는 target User Spot의 OnActorJoin으로 admission을 결정한다 | 371 | |
| 15-spot-actor-R152 | User Spot에서 Entry Spot으로 복귀할 때는 admission 없이 membership을 commit한다 | 372 | |
| 15-spot-actor-R153 | 두 일반 이동은 commit 뒤 target의 OnJoinedActor와 source의 OnLeaveActor를 사용한다 | 372-373 | |
| 15-spot-actor-R154 | User Spot에서 Entry Spot으로 복귀하는 Actor는 새 Actor가 아니다. Target Entry Spot에서 OnCreateActor와 OnActorJoin을 호출하지 않고 OnJoinedActor만 실행하며, source User Spot에서 OnLeaveActor를 실행한다 | 416-418 | |
| 15-spot-actor-R155 | Owner 전환, ordered relay, target queue 병합과 Location Store CAS의 전체 순서는 28장이 단일 기준이다 | 422-423 | |
| 15-spot-actor-R156 | Handler가 JoinSpot(...) 또는 JoinEntrySpot(...) 호출 후 반환된 call 객체에서 Defer() 호출하면 Framework는 handler가 정상적으로 끝난 뒤 다음 순서로 Join을 실행한다(8-step flow) | 427-478 | |
| 15-spot-actor-R157 | Step1: Defer()는 Join을 등록할 뿐이며 handler가 정상적으로 끝나기 전에는 Actor를 만들거나 message를 보내지 않는다 | 430-432 | |
| 15-spot-actor-R158 | Step2: Target이 User Spot이면 target의 OnActorJoin에 ActorId와 join request를 전달한다 | 433-434 | |
| 15-spot-actor-R159 | Step2: Target은 Accepted 반환 전에 해당 ActorId와 ObjectGeneration의 relocation temporary queue 등록과 factory 실행 준비를 함께 끝낸다 | 434-437 | |
| 15-spot-actor-R160 | Step2: factory의 stable type은 wire로 받은 type이 아니라 Actor의 Location Store Authority row(allocation.stableType, ActorId로 키잉)에서 해석하고 join request의 actor route fence와 대조한다 — 교차 언어 wire 형식은 Actor stable type을 싣지 않는다 | 437-440 | |
| 15-spot-actor-R161 | Step2: 이후 Restore 요청에서 이 준비를 반복하지 않으므로 왕복 수는 그대로이고 seal 뒤 처리 시간만 줄어든다 | 440-441 | |
| 15-spot-actor-R162 | Step2: 승인 reply에는 relocation state chunk의 target 유효 수신 상한을 함께 싣는다 — 이 값은 재계산에도 낮아지지 않는 안정 하한 기반의 보수값이다 | 441-443 | |
| 15-spot-actor-R163 | Step2: Accepted면 계속, Rejected면 target이 같은 처리 안에서 등록한 temporary queue와 준비한 factory 자원을 제거하며 source membership을 유지한 채 끝낸다 | 444-446 | |
| 15-spot-actor-R164 | Step2: Target이 Entry Spot이면 OnActorJoin을 호출하지 않는다 | 446 | |
| 15-spot-actor-R165 | Step3: Framework가 relocation policy와 target capacity를 확인한다. 이동을 진행할 수 있으면 source Actor의 새 message 처리를 잠시 막고, application state와 현재 Actor queue를 capture해 source memory에 유지한다 | 447-449 | |
| 15-spot-actor-R166 | Step3: Relocation payload는 저장소를 거치지 않고 source에서 target으로 직접 전달한다 | 449-450 | |
| 15-spot-actor-R167 | Step3: DisableRelocation이면 이 단계에서 거부한다 | 450 | |
| 15-spot-actor-R168 | Step4: Source runtime이 target runtime에 Actor Restore 요청을 먼저 보낸다 | 451-452 | |
| 15-spot-actor-R169 | Step4: Restore 요청에는 payload 전체 크기, chunk 수와 checksum을 담은 manifest가 포함되고, payload는 같은 ordered 연결로 chunk 단위로 뒤따라 전송된다 | 452-453 | |
| 15-spot-actor-R170 | Step4: Target dispatcher는 승인 처리에서 등록한 relocation temporary queue를 사용하고, 승인 왕복이 없는 Entry Spot join에서는 다음 packet을 dispatch하기 전에 temporary queue를 등록한다 | 454-456 | |
| 15-spot-actor-R171 | Step4: 이후 같은 Actor로 들어오는 message는 application instance를 찾지 않고 temporary queue에 넣는다 | 456-457 | |
| 15-spot-actor-R172 | Step4: Target은 조립한 payload의 checksum을 확인한 뒤 Actor를 만들고 application state와 기존 queue를 복원하지만 아직 application 작업을 실행하지 않는다 | 457-459 | |
| 15-spot-actor-R173 | Step5: Source seal 뒤에 도착한 message는 source runtime의 ingress hold에 보관한다 | 460-461 | |
| 15-spot-actor-R174 | Step5: 이 hold에는 relocation 자체가 정하는 record 수나 byte 상한이 없다 | 461-462 | |
| 15-spot-actor-R175 | Step5: Target이 temporary queue와 Restore 준비 완료를 알리면 source runtime은 hold의 message를 같은 ordered TCP connection으로 relay한다 | 462-463 | |
| 15-spot-actor-R176 | Step5: Target dispatcher는 이 message를 temporary queue group의 boundary 전 relay 구간에 넣는다 | 463 | |
| 15-spot-actor-R177 | Step6: Source는 relay lane의 현재 prefix를 보낸 뒤 같은 connection에 cutover를 one-way로 보낸다 | 464-465 | |
| 15-spot-actor-R178 | Step6: 이후 도착한 message는 boundary 뒤 구간에 넣으므로 mailbox가 비기를 기다리지 않는다 | 465 | |
| 15-spot-actor-R179 | Step6: Target은 Actor Restore 뒤 cutover를 받으면 membership, owner, capacity와 generation을 LocationStore에서 한 번에 CAS한다. 이 CAS는 target만 수행한다 | 466-468 | |
| 15-spot-actor-R180 | Step6: Relay 준비 reply 뒤 cutover 대기 시간(RelocationCutoverWaitTimeout, 기본 1,000ms) 동안 cutover와 재전송이 모두 오지 않아도 Warning을 기록하고 CAS를 진행한다 | 468 | |
| 15-spot-actor-R181 | Step6: 성공하면 target이 새 owner가 되고, 실패하면 target queue를 열지 않는다 | 469 | |
| 15-spot-actor-R182 | Step7: CAS 뒤 저장된 기존 Actor 작업, boundary 전 relay와 나머지 temporary 작업을 실제 Actor queue에 순서대로 넣고 regular route로 전환하되 dispatch는 닫아 둔다 | 470-472 | |
| 15-spot-actor-R183 | Step7: 그다음 Target Spot의 OnJoinedActor를 호출하고 source Spot에는 OnLeaveActor를 one-way로 보낸 뒤 Actor의 Join completion callback을 끝낸다. 이 lifecycle 뒤 dispatch를 연다 | 472-473 | |
| 15-spot-actor-R184 | Step7: Target은 source에 완료 reply를 보내지 않는다 | 473-474 | |
| 15-spot-actor-R185 | Step8: Actor가 Session에 bind되어 있으면 target runtime이 CAS와 queue 개방 뒤 Session owner에 target route 적용과 seal 해제를 one-way로 알린다 | 475-477 | |
| 15-spot-actor-R186 | Step8: Session owner는 기본 3,000ms의 SessionRelocationSealTimeout 안에 exact update를 받으면 route를 바꾸고 held message를 제출한 뒤 seal을 해제한다. Timeout이면 physical STREAM connection을 종료하고 Session state를 정리한다 | 477-478 | |
| 15-spot-actor-R187 | 승인이 Accepted여도 그 뒤의 relocation policy 검사(DisableRelocation), capacity 충돌이나 state 호환성 실패로 이동이 시작되지 않을 수 있다 | 480-482 | |
| 15-spot-actor-R188 | 준비 자원은 RelocationId와 target attempt를 포함한 exact identity로 식별하며, 이동이 시작되지 않으면 준비 유효시간이 지난 뒤 target이 제거한다 | 482-483 | |
| 15-spot-actor-R189 | 준비만 된 target이 owner가 되는 경로는 없고, temporary queue가 등록되어 있어도 Location Store CAS 전에는 application handler를 실행하지 않는다 | 483-485 | |
| 15-spot-actor-R190 | 같은 object의 relocation temporary queue는 하나만 존재한다 | 487 | |
| 15-spot-actor-R191 | 기존 준비가 정리되기 전에 다른 exact identity의 승인이나 Restore가 도착하면 target은 기존 준비 상태를 먼저 abort·정리한 뒤 새 identity의 준비를 만든다 — 나중 attempt가 유효하며, 이전 identity의 늦은 chunk와 Restore는 조립에 연결하지 않고 폐기한다 | 487-490 | |
| 15-spot-actor-R192 | JoinEntrySpot은 target에서 OnActorJoin을 호출하지 않아 승인 왕복 자체가 없으므로 이 준비 동승이 없다 | 492-493 | |
| 15-spot-actor-R193 | Entry Spot join은 Restore 요청에서 준비를 수행하고, 협상한 chunk 상한이 없으므로 어느 배치에서도 보장되는 32 KiB의 보수 chunk 크기(chunk 하나의 encoded 크기)로 전송한다 | 493-495 | |
| 15-spot-actor-R194 | Accepted와 Rejected는 동시에 발생하지 않는 서로 다른 결과이므로 다이어그램에서 alt로 나눈다 | 497-498 | |
| 15-spot-actor-R195 | Bound Session이 있는지는 선택 사항이므로 그 부분만 opt로 표시한다 | 498 | |
| 15-spot-actor-R196 | OnActorJoin이 Rejected를 반환하거나 relay-ready reply가 accepted 상태가 되기 전에 명시적으로 실패하면 source membership을 유지한다 | 556-558 | |
| 15-spot-actor-R197 | OnLeaveActor는 owner commit 뒤에만 보내므로 이 source 복구 경로에서는 호출하지 않는다 | 558 | |
| 15-spot-actor-R198 | User Spot target은 승인 처리에서 등록한 relocation temporary queue를 사용하고, Entry Spot join에서는 Restore 요청을 받은 target이 temporary queue를 먼저 등록한다 | 558-560 | |
| 15-spot-actor-R199 | 그동안 도착한 message와 request는 temporary queue에서 기다리며 실제 Actor queue로 옮기기 전에는 실행하지 않는다 | 560-561 | |
| 15-spot-actor-R200 | Target commit 뒤에 실패하면 source로 rollback하지 않는다 | 561-562 | |
| 15-spot-actor-R201 | 같은 target process가 실행 중일 때만 deadline 안에서 다시 시도하며, process가 종료되면 relocation을 자동으로 이어받지 않는다 | 562-564 | |
| 15-spot-actor-R202 | Relay-ready reply가 accepted 상태가 되기 전 reject, timeout 또는 Capture·Restore failure가 명시적으로 발생하면 target application instance를 공개하지 않는다 | 566-567 | |
| 15-spot-actor-R203 | Target이 받은 relay record는 staging 사본이므로 temporary queue에서 실행하거나 terminal 결과를 만들지 않고 폐기한다 | 567-568 | |
| 15-spot-actor-R204 | Source가 ingress hold의 request와 one-way message를 원래 Actor queue에 도착 순서대로 되돌린다 | 568-569 | |
| 15-spot-actor-R205 | Queue가 비면 해당 temporary queue 등록을 제거한다 | 569 | |
| 15-spot-actor-R206 | 이때 source owner, state와 membership을 그대로 유지한다 | 570 | |
| 15-spot-actor-R207 | Relay-ready 뒤 timeout, aggregate commit conflict 또는 cutover submit failure는 source rollback 조건이 아니다 | 571-572 | |
| 15-spot-actor-R208 | 같은 target process가 실행 중이면 확정된 위치정보와 source가 memory에 유지한 payload의 재전송으로 deadline 안에서 다시 시도할 수 있다 | 572-574 | |
| 15-spot-actor-R209 | Target process가 종료되면 다른 runtime이 자동 복구하지 않는다 | 574-575 | |
| 15-spot-actor-R210 | ObjectGeneration은 그대로 유지한다. Cross-node 이동으로 owner가 바뀌므로 AuthorityOwnerGeneration만 증가한다 | 577-578 | |
| 15-spot-actor-R211 | Target Context는 기존 ObjectGeneration과 새 owner generation을 사용한다 | 578-579 | |
| 15-spot-actor-R212 | Location Store CAS가 성공하면 Source Context가 더 이상 operation을 실행하지 못하도록 차단한다 | 579-580 | |
| 15-spot-actor-R213 | Defer() 뒤 source seal 전에 도착한 message는 현재 Actor queue와 함께 capture하여 source memory에 유지한 relocation payload에 포함한다 | 582-584 | |
| 15-spot-actor-R214 | Source seal 뒤 도착한 message는 ingress hold에 임시로 보관한다. 이 hold에는 relocation 전용 record 수나 byte 상한이 없다 | 584-585 | |
| 15-spot-actor-R215 | Source runtime은 hold의 record와 이후 이전 route로 들어오는 record를 target temporary queue로 계속 relay한다 | 587-588 | |
| 15-spot-actor-R216 | Relay-ready reply가 accepted 상태가 되기 전에 명시적으로 중단하면 hold의 record를 도착 순서대로 source queue에 되돌리고 target temporary queue를 폐기한다 | 588-589 | |
| 15-spot-actor-R217 | 그 뒤에는 cutover submit 결과와 관계없이 source를 복원하지 않는다 | 589-590 | |
| 15-spot-actor-R218 | Owner commit이 성공하면 저장된 기존 작업 뒤에 boundary 전 relay와 나머지 temporary record를 순서대로 옮긴다 | 590-591 | |
| 15-spot-actor-R219 | Source는 target의 완료 reply를 기다리지 않는다 | 591 | |
| 15-spot-actor-R220 | Cutover를 보낸 뒤 ingress hold를 Message Follow relay로 전환하고 정해진 Message Follow 기간이 끝나면 원본을 제거한다 | 592-593 | |
| 15-spot-actor-R221 | Application이 요청한 User Spot join에서는 target User Spot의 OnActorJoin으로 먼저 admission을 결정한다 | 595-596 | |
| 15-spot-actor-R222 | User Spot에서 Entry Spot으로 복귀할 때는 OnActorJoin을 호출하지 않고 membership을 바로 commit한다 | 599-600 | |
| 15-spot-actor-R223 | 이 callback들은 application이 요청한 logical membership 변경에만 사용한다 | 601-602 | |
| 15-spot-actor-R224 | Entry Spot 자체는 relocation participant가 아니며 mesh spot routing으로 주소를 지정할 수도 없다 | 604-605 | |
| 15-spot-actor-R225 | Node의 Entry Spot을 대상으로 하는 framework notification은 node 단위로 전달되며 수신 node가 local Entry Spot에 dispatch한다 | 606-607 | |
| 15-spot-actor-R226 | Host Relocate로 source Entry Spot의 Actor를 target node의 Entry Spot으로 옮길 때 Framework는 Actor adapter로 state를 복원한다 | 607-609 | |
| 15-spot-actor-R227 | Owner, membership, queue, timer와 session route도 target으로 이전한다 | 610 | |
| 15-spot-actor-R228 | 이 infrastructure relocation에서는 target의 OnJoinedActor와 source의 OnLeaveActor를 호출하지 않으며 Relocation 전용 application callback도 제공하지 않는다 | 611-612 | |
| 15-spot-actor-R229 | Target Actor dispatch는 Restore 중 들어오는 message를 relocation temporary queue에 보관한다 | 612-613 | |
| 15-spot-actor-R230 | Commit 뒤 저장된 queue와 timer, boundary 전 relay와 나머지 temporary message를 실제 Actor queue에 순서대로 넣는다 | 613-614 | |
| 15-spot-actor-R231 | 전환 뒤 Message Follow와 target direct message는 기존 Actor queue 경로를 사용한다 | 614-615 | |
| 15-spot-actor-R232 | Spot의 terminal lifecycle callback은 OnClosing(ClosingContext)이다 | 617 | |
| 15-spot-actor-R233 | Actor는 항상 Entry 또는 User Spot에 속하므로 Actor별 closing callback을 제공하지 않는다 | 617-618 | |
| 15-spot-actor-R234 | ClosingContext는 닫힌 reason과 operation의 absolute deadline을 제공한다 | 618-619 | |
| 15-spot-actor-R235 | ClosingContext reason table: 0=ExplicitClose(Application이 User·Instance Spot close 시작), 1=HostShutdown(Relocation 없이 host Shutdown이 local Entry·User·Instance Spot 정리), 2=RelocationOut(User·Instance Spot owner commit 뒤 source local instance 정리) | 621-625 | |
| 15-spot-actor-R236 | Standalone Actor 이동은 Entry Spot 자체를 닫지 않으므로 Entry Spot의 OnClosing을 호출하지 않는다 | 627-628 | |
| 15-spot-actor-R237 | Infrastructure relocation에서는 Actor membership callback도 호출하지 않는다 | 628-629 | |
| 15-spot-actor-R238 | User Spot에 Actor membership이 남아 explicit close가 거부되면 OnClosing을 호출하지 않는다 | 629-630 | |
| 15-spot-actor-R239 | Host Shutdown에서는 accepted handler와 timer turn을 terminal 상태로 만든 뒤, Actor membership과 local instance가 아직 유효한 상태에서 Spot OnClosing을 호출한다 | 630-631 | |
| 15-spot-actor-R240 | Callback 완료 뒤 Actor·Spot scope를 dispose하고 Location authority와 resource를 정리한다 | 631-632 | |
| 15-spot-actor-R241 | 언어 runtime에 표준 cooperative cancellation 표현이 있으면 callback에 남은 cleanup budget을 함께 전달할 수 있다 | 634-635 | |
| 15-spot-actor-R242 | Spot closing만을 위한 별도 Framework cancellation 타입을 만들지는 않는다 | 635 | |
| 15-spot-actor-R243 | 표준 표현이 없는 언어에서는 ClosingContext의 deadline만 전달하고 Framework가 deadline에 callback completion 대기를 끝낸다 | 636 | |
| 15-spot-actor-R244 | Application은 callback 이후 context와 cancellation signal을 보관하지 않는다 | 636-637 | |
| 15-spot-actor-R245 | HostShutdown은 callback failure로 relocation나 rollback을 시작하지 않는다 | 637-638 | |
| 15-spot-actor-R246 | Callback exception은 ForceStopped/TeardownFailed, deadline 만료는 ForceStopped/DeadlineExceeded로 끝난다 | 638-639 | |
| 15-spot-actor-R247 | Process crash와 SIGKILL에서는 callback 실행을 보장하지 않는다 | 639 | |
| 15-spot-actor-R248 | Actor·User Spot·Instance Spot의 Object Server factory는 relocation policy 중 하나를 반드시 등록한다: DisableRelocation, RecreateOnRelocation, PreserveStateWith | 644-650 | |
| 15-spot-actor-R249 | DisableRelocation: Cross-node relocation을 capture 전에 거부하고 source owner와 admission을 유지한다 | 648 | |
| 15-spot-actor-R250 | RecreateOnRelocation: Target factory를 실행하고 Framework queue·timer 정보는 유지하지만 application state payload는 전달하지 않는다. 새 application 객체를 만들더라도 같은 logical incarnation이므로 ObjectGeneration을 유지한다 | 649 | |
| 15-spot-actor-R251 | PreserveStateWith: Handler가 정상적으로 끝난 경계의 application state를 relocation adapter로 opaque byte sequence에 capture하고 target에 복원한다. Framework queue·timer 정보도 함께 유지한다 | 650 | |
| 15-spot-actor-R252 | Actor는 ActorRelocationAdapter를 사용한다. SpotWide User Spot과 Instance Spot은 SpotRelocationAdapter를 사용한다 | 652-653 | |
| 15-spot-actor-R253 | PerActor User Spot의 Spot shell은 application state를 이전하지 않으므로 RecreateOnRelocation policy만 허용하며 Spot adapter를 등록하면 startup configuration error다 | 653-655 | |
| 15-spot-actor-R254 | 두 adapter의 operation 이름은 Capture와 Restore다 | 657 | |
| 15-spot-actor-R255 | Capture는 source instance를 받아 byte sequence를 반환하고, Restore는 target factory가 만든 instance와 byte sequence를 받아 상태를 적용한다. Instance를 반환하지 않는다 | 657-659 | |
| 15-spot-actor-R256 | Application은 byte format, version, compatibility와 migration을 관리한다 | 661 | |
| 15-spot-actor-R257 | Framework는 state contract ID, generic state type, serialization profile과 message codec을 relocation adapter 계약에 추가하지 않는다 | 661-662 | |
| 15-spot-actor-R258 | Framework는 application bytes를 그대로 opaque payload로 source에서 target으로 직접 전송하고, payload를 나눈 chunk와 전체 checksum만 Framework가 검증한다 | 662-664 | |
| 15-spot-actor-R259 | Capture가 반환하는 byte sequence에는 relocation이 별도 크기 상한을 추가하지 않는다 | 669 | |
| 15-spot-actor-R260 | 전송 중 연결 점유는 chunk 크기와 in-flight 예산이 제한하며, 예산보다 큰 payload도 chunk가 순서대로 흘러가며 시작하고 완료할 수 있다 | 669-671 | |
| 15-spot-actor-R261 | 빈 byte sequence는 유효한 application state이고 null result는 adapter contract 위반이다 | 671-672 | |
| 15-spot-actor-R262 | Callback이 성공하면 Framework가 결과를 즉시 복사하거나 소유권을 넘겨받으므로 application은 그 뒤 결과를 바꾸지 않는다 | 672-673 | |
| 15-spot-actor-R263 | Restore에 전달한 bytes는 callback이 완료될 때까지만 유효하고 callback이 보관하려면 직접 복사해야 한다 | 673-674 | |
| 15-spot-actor-R264 | Join과 host maintenance는 같은 factory relocation 구성과 adapter registration을 사용한다 | 676 | |
| 15-spot-actor-R265 | PreserveStateWith를 선택한 Actor가 다른 node의 User Spot·Entry Spot으로 join하거나 maintenance로 이동할 때 Actor adapter를 호출한다 | 677-678 | |
| 15-spot-actor-R266 | User Spot aggregate relocation에서는 PreserveStateWith로 등록한 Spot과 각 member Actor의 adapter를 각각 호출한다 | 679-680 | |
| 15-spot-actor-R267 | Same-node join, DisableRelocation 거부와 RecreateOnRelocation에서는 adapter를 호출하지 않는다 | 680-681 | |
| 15-spot-actor-R268 | Operation별 policy, 생략 overload와 별도 adapter registry를 제공하지 않는다 | 681 | |
| 15-spot-actor-R269 | Policy와 adapter registration은 startup 뒤 바뀌지 않는다 | 682 | |
| 15-spot-actor-R270 | Host Relocate가 User Spot을 이전할 때는 해당 Spot과 seal 시점의 current member Actor 전체를 하나의 aggregate로 처리한다 | 691-692 | |
| 15-spot-actor-R271 | Application은 aggregate에 포함할 participant나 relocation phase를 선택하지 않는다 | 693 | |
| 15-spot-actor-R272 | Host가 Relocating으로 전환되면 Framework는 aggregate의 Spot control queue에 infrastructure intent notification을 예약한다. 이 notification은 application callback이 아니다 | 695-696 | |
| 15-spot-actor-R273 | Notification을 처리한 turn 경계에서 permit을 얻지 못하면 seal하지 않고 다음 notification을 예약하므로 Spot과 member Actor는 application message와 timer를 계속 처리한다 | 696-697 | |
| 15-spot-actor-R274 | Aggregate ID는 non-zero 128-bit value다 | 699 | |
| 15-spot-actor-R275 | Aggregate에 포함할 수 있는 Actor 총수에는 고정 상한을 두지 않는다 | 699-700 | |
| 15-spot-actor-R276 | 실제 총수는 source에 존재하는 membership과 target이 광고한 population capacity로 제한한다 | 700-701 | |
| 15-spot-actor-R277 | Framework는 participant 전체를 record 하나에 넣지 않는다 | 703 | |
| 15-spot-actor-R278 | Object kind, global key, ObjectGeneration, owner fence와 policy를 정렬한 뒤 Location Store에 여러 immutable inventory chunk로 저장한다 | 703-704 | |
| 15-spot-actor-R279 | Leaf chunk 하나에는 최대 1,024개를 저장하며 encoded 크기는 1 MiB를 넘지 않는다 | 705-706 | |
| 15-spot-actor-R280 | 목록이 leaf 하나에 들어가지 않으면 index chunk를 추가하여 tree를 만든다 | 706-707 | |
| 15-spot-actor-R281 | Aggregate authority에 두는 값: AggregateId와 generation, Participant count, Inventory root와 digest, Owner | 707-714 | |
| 15-spot-actor-R282 | 복원할 payload는 저장소가 아니라 source memory가 원본이므로 authority가 가리키지 않는다 | 714 | |
| 15-spot-actor-R283 | SpotWide User Spot에 속한 Actor의 current owner는 User Spot aggregate authority를 따른다 | 729 | |
| 15-spot-actor-R284 | Actor별 membership record는 해당 aggregate를 가리키며 relocation 때 owner를 하나씩 공개하지 않는다 | 730-731 | |
| 15-spot-actor-R285 | Aggregate 6-step flow: 1)Spot queue turn 경계에서 source User Spot의 join·leave와 모든 participant application admission을 reversible하게 seal 2)Exact participant inventory를 immutable tree로 저장하고 root·count·digest 검증 3)모든 relocation 구성/target type·state 보존 adapter capability 확인 4)PreserveStateWith participant의 state, 실행하지 않은 message queue와 timer registration·pending tick을 capture하고 target factory·restore를 admission이 닫힌 상태로 준비 5)준비를 끝낸 target이 Location Store의 단일 CAS로 aggregate owner, generation과 inventory root를 전환 6)Authority commit 뒤 target lifecycle callback, 저장한 message replay와 Framework timer 자동 복원을 끝내고 target User Spot과 member Actor가 message를 처리하기 시작 | 733-746 | |
| 15-spot-actor-R286 | Step6: Bound Actor의 target runtime은 bound Actor마다 Session owner에 target route 적용과 seal 해제를 one-way로 알린다. 같은 Session에서 aggregate 밖 Actor의 route와 physical STREAM connection은 유지한다 | 743-745 | |
| 15-spot-actor-R287 | 4번의 restore는 5번 aggregate commit 전에 끝나야 한다 | 747 | |
| 15-spot-actor-R288 | SpotWide User Spot aggregate는 logical membership을 그대로 이동하므로 member Actor에 대한 application membership callback을 호출하지 않는다 | 747-749 | |
| 15-spot-actor-R289 | Spot·Actor adapter의 restore와 Spot lifecycle callback만 target admission 전에 끝낸다 | 749-750 | |
| 15-spot-actor-R290 | Commit 전 새 inventory tree와 target staging은 resolver에 보이지 않는다 | 752 | |
| 15-spot-actor-R291 | Participant 하나라도 relay-ready reply가 accepted 상태가 되기 전에 실패하면 target staging을 폐기하고 aggregate 전체 source 상태를 유지한다 | 753-754 | |
| 15-spot-actor-R292 | 그 뒤에는 cutover submit 결과와 관계없이 일부 participant도 source로 되돌리지 않고 같은 aggregate identity와 inventory root를 유지한다 | 754-756 | |
| 15-spot-actor-R293 | 같은 target process가 실행 중일 때만 aggregate 전체를 계속 처리하며, process가 종료되면 다른 runtime이 이어받지 않는다 | 756-757 | |
| 15-spot-actor-R294 | PerActor User Spot은 aggregate owner 변경을 사용하지 않는다 | 759 | |
| 15-spot-actor-R295 | Framework는 target에 runtime-private Spot shell을 준비하고 Spot queue의 현재 turn과 진행 중인 Create·Join을 끝낸 뒤 Location Store의 Spot authority를 target으로 CAS한다 | 760-762 | |
| 15-spot-actor-R296 | Public SpotId와 ObjectGeneration은 바꾸지 않으며 임시 public SpotId를 만들거나 target activation 뒤 SpotId를 다시 지정하지 않는다 | 762-763 | |
| 15-spot-actor-R297 | Spot authority commit 뒤 새 ToSpot, Actor Create와 Join은 target으로 보낸다 | 765 | |
| 15-spot-actor-R298 | Source shell은 이미 source에 남은 Actor의 handler와 relocation control만 실행한다 | 766 | |
| 15-spot-actor-R299 | 각 Actor는 독립된 relocation unit이며 Actor queue를 seal한 뒤 state, 실행하지 않은 queue, timer와 bound-session current Actor location snapshot을 target으로 옮긴다 | 767-769 | |
| 15-spot-actor-R300 | Snapshot은 같은 ActorId·ObjectGeneration을 유지하고 target MeshName·NodeRid를 제공한다 | 769-770 | |
| 15-spot-actor-R301 | Actor별 owner CAS가 성공하면 이전 owner로 도착한 message를 같은 operation identity, ObjectGeneration, deadline, request correlation과 reply route로 target에 relay한다 | 770-772 | |
| 15-spot-actor-R302 | 마지막 Actor가 target owner가 되고 source가 이미 수락한 Spot 작업과 relay를 모두 끝내면 source shell을 RelocationOut으로 닫는다 | 774-775 | |
| 15-spot-actor-R303 | Relocation 중에는 일부 Actor가 source에 있고 일부가 target에 있을 수 있다. 이 분산 상태는 같은 relocation operation에서만 허용하며 steady 상태에서는 Spot authority와 모든 member Actor owner가 같아야 한다 | 775-778 | |
| 15-spot-actor-R304 | SpotWide User Spot이 application-signaled relocation 경계를 사용하면 RelocationReady().Defer()가 현재 turn 뒤에 Framework-owned barrier를 등록한다 | 780-781 | |
| 15-spot-actor-R305 | Framework는 aggregate CAS, queue 병합과 regular route 전환 뒤 dispatch를 열기 전에 target owner에서 Spot의 기본 no-op OnRelocationReadyCompleted callback을 호출한다 | 781-783 | |
| 15-spot-actor-R306 | 이 callback은 Actor membership 변경 callback이 아니며 member Actor에 전달하지 않는다 | 783-784 | |
| 15-spot-actor-R307 | Callback을 override한 application은 다음 round나 match를 여기서 시작할 수 있다 | 784-785 | |
| 15-spot-actor-R308 | Relay-ready reply가 accepted 상태가 되기 전 명시적 failure는 Aborted CAS, route와 source location snapshot 취소 확인, relocation reservation·target staging 정리와 source 상태 복원을 끝낸 뒤 source admission을 다시 연다 | 789-790 | |
| 15-spot-actor-R309 | 이 경계 뒤에는 cutover submit 성공·실패와 관계없이 source를 복원하지 않는다 | 791 | |
| 15-spot-actor-R310 | Cutover 뒤 Location Store 변경 결과를 받지 못하면 target은 성공이나 실패를 추측하지 않고 같은 authority를 다시 읽는다 | 792-793 | |
| 15-spot-actor-R311 | Exact target owner가 아니면 Restore 유효시간까지 같은 fence로 retry한다 | 793 | |
| 15-spot-actor-R312 | 그 안에 owner 변경을 확인하지 못하면 location_update_failed를 기록하고 target Actor 또는 Spot과 temporary queue를 제거하며 Session route update를 보내지 않는다 | 793-795 | |
| 15-spot-actor-R313 | Capture가 실패하면 Restore 요청을 보내지 않고 source를 유지한다 | 797-798 | |
| 15-spot-actor-R314 | Restore가 실패하면 target staging instance와 temporary queue를 폐기한다 | 798 | |
| 15-spot-actor-R315 | 같은 source와 target process가 계속 실행 중이고 deadline이 남아 있으면 새 instance를 만들어 같은 payload의 Restore를 다시 시도할 수 있다 | 798-800 | |
| 15-spot-actor-R316 | 재-Restore의 payload 원본은 저장소가 아니라 source memory이며, source가 같은 payload를 다시 전송한다 | 800-801 | |
| 15-spot-actor-R317 | 다른 target을 자동 선택하지 않으며, deadline까지 성공하지 못하면 source를 유지하고 StateIncompatible 또는 원인에 맞는 Failed 결과로 끝낸다 | 801-802 | |
| 15-spot-actor-R318 | Owner와 membership commit 뒤 failure는 source rollback 조건이 아니다 | 804 | |
| 15-spot-actor-R319 | 같은 target process가 실행 중이면 target admission을 닫은 상태로 lifecycle callback이나 dispatch 전환을 deadline 안에서 다시 시도할 수 있다 | 805-806 | |
| 15-spot-actor-R320 | Source나 target process가 종료되면 다른 runtime이 relocation을 이어받지 않는다 | 806 | |
| 15-spot-actor-R321 | Commit 뒤 target이 종료되면 authority는 target을 유지하지만 object는 unavailable 상태가 된다 | 807-808 | |
| 15-spot-actor-R322 | 자동 target replacement와 process 재시작 뒤 relocation 재개는 이 계약의 범위 밖이다 | 808-809 | |
| 15-spot-actor-R323 | 같은 process 안의 재시도 때문에 factory, Restore와 lifecycle callback은 두 번 이상 호출될 수 있다 | 811-812 | |
| 15-spot-actor-R324 | Callback은 같은 object generation과 입력을 다시 받아도 수렴해야 하며 exactly-once external side effect를 가정하면 안 된다 | 812-813 | |
| 15-spot-actor-R325 | Process pause 뒤 재개한 이전 owner는 stale AuthorityOwnerGeneration, owner lease와 local admission deadline 때문에 message, timer, phase update와 cleanup을 수행하지 못한다 | 813-815 | |
| 15-spot-actor-R326 | Commit 뒤 source는 MessageFollowDuration 안에서 이전 주소로 도착한 message를 committed target으로 relay한다 | 819-820 | |
| 15-spot-actor-R327 | Relay는 Store를 읽거나 application handler를 실행하지 않으며 original operation identity, generation, payload와 reply route를 유지한다 | 820-821 | |
| 15-spot-actor-R328 | Route duration이 끝난 뒤 이전 주소로 도착한 request는 Unavailable로 끝난다 | 821-822 | |
| 15-spot-actor-R329 | Message Follow는 Session route update를 기다리기 위한 재전송 queue가 아니다 | 824 | |
| 15-spot-actor-R330 | Target이 Session route update를 보내더라도, 이미 이전 주소로 전송된 server message를 처리하기 위해 정해진 기간 동안 유지한다 | 824-826 | |
| 15-spot-actor-R331 | 서로 다른 connection의 message 사이에 전역 순서는 보장하지 않는다 | 826-827 | |
| 15-spot-actor-R332 | Actor가 이동해도 physical STREAM connection, Session identity와 ObjectGeneration은 유지된다 | 831 | |
| 15-spot-actor-R333 | Relocation을 시작하기 전에 Session owner가 해당 Actor binding을 seal하고 Session request와 push를 보관한다 | 832-833 | |
| 15-spot-actor-R334 | Target은 Actor·Spot 준비 뒤 cutover를 받거나 cutover 대기 시간 fallback이 끝나면 Location Store CAS를 수행한다 | 835-836 | |
| 15-spot-actor-R335 | CAS, target queue 개방과 lifecycle 완료 뒤 target runtime이 Session owner에 one-way route update를 보낸다 | 836-837 | |
| 15-spot-actor-R336 | Session owner는 exact update를 받으면 binding route와 current ActorRef location snapshot을 target으로 바꾸고, 보관한 Session message를 제출한 뒤 seal을 해제한다 | 837-839 | |
| 15-spot-actor-R337 | SessionRelocationSealTimeout의 기본값은 3,000ms이며 timeout이면 physical Session을 종료한다 | 839 | |
| 15-spot-actor-R338 | Session은 target을 선택하거나 Location Store를 변경하지 않는다 | 840 | |
| 15-spot-actor-R339 | Session owner는 current Session identity, binding generation, ActorId·ObjectGeneration과 relocation identity만 검증한다 | 842-843 | |
| 15-spot-actor-R340 | AuthorityOwnerGeneration, numeric high-water 또는 Actor Location mirror를 다시 검증하지 않는다 | 843-844 | |
| 15-spot-actor-R341 | Transport peer와 node generation은 transport 경계가, owner 변경은 target의 Location Store CAS가 각각 한 번 검증한다 | 844-845 | |
| 15-spot-actor-R342 | Late·duplicate cutover 또는 Session route update는 Warning만 남기고 route, seal 또는 authority를 다시 바꾸지 않는다 | 847-848 | |
| 15-spot-actor-R343 | Same Session의 다른 Actor route와 physical connection은 영향을 받지 않는다 | 848 | |
| 15-spot-actor-R344 | Contract test: Object role이 Store 없이 startup하지 않고 hidden local manager를 만들지 않는다 | 855 | |
| 15-spot-actor-R345 | Contract test: Creation reservation이 global key authority와 pending capacity를 atomic하게 고정한다 | 856 | |
| 15-spot-actor-R346 | Contract test: 동시에 같은 Actor 생성을 요청해도 reservation CAS winner만 factory와 creation callback을 실행하며 loser는 authority 변경을 기다린다 | 857-858 | |
| 15-spot-actor-R347 | Contract test: 서로 다른 operation은 Ready 뒤 Existing을 받고 cleanup 뒤 새 reservation을 경쟁하며, 같은 source lifecycle·OperationId의 재전송만 terminal을 replay한다 | 859-860 | |
| 15-spot-actor-R348 | Contract test: Rejected와 Aborted가 Ready authority와 active capacity를 만들지 않고 pending capacity를 반환한다 | 861-862 | |
| 15-spot-actor-R349 | Contract test: Terminal record가 original deadline 뒤 5분 동안 같은 operation의 replay를 허용하고, TTL 뒤 Ready authority가 없으면 새 reservation으로 다시 생성할 수 있다 | 863-864 | |
| 15-spot-actor-R350 | Contract test: Target User Spot의 OnActorJoin이 Capture보다 먼저 실행되고 relay-ready reply가 accepted되기 전 명시 failure가 source 전체를 유지한다. 그 뒤에는 source를 복원하지 않는다 | 865-866 | |
| 15-spot-actor-R351 | Contract test: Actor join은 execution mode와 관계없이 Yield를 제공하지 않는다 | 867 | |
| 15-spot-actor-R352 | Contract test: Defer()가 target 조회나 Store I/O 없이 현재 handler에 Join 등록과 비활성 barrier만 남기고, handler의 마지막 continuation이 정상 종료한 뒤 실행한다 | 868-869 | |
| 15-spot-actor-R353 | Contract test: Handler가 실패하면 해당 handler가 등록한 barrier를 모두 폐기한다 | 870 | |
| 15-spot-actor-R354 | Contract test: Handler당 Join 64개, request 하나당 1 MiB, request 합계 8 MiB 제한을 적용하고 초과한 registration이 partial record 없이 동기 실패한다 | 871-872 | |
| 15-spot-actor-R355 | Contract test: Timeout 생략 시 5초를 사용하고 Defer() 시점에 monotonic absolute deadline을 고정한다 | 873-874 | |
| 15-spot-actor-R356 | Contract test: Registration scope가 닫힌 뒤 Defer()를 거부하며 detached task의 호출을 application contract 위반으로 처리한다 | 875-876 | |
| 15-spot-actor-R357 | Contract test: SpotWide member Actor의 request·worker Yield가 Actor queue claim을 유지하여 같은 Actor의 다음 job보다 continuation을 먼저 완료한다 | 877-878 | |
| 15-spot-actor-R358 | Contract test: 바리어가 걸린 Actor를 같은 handler에서 awaited request하면 InvalidOperation으로 거부한다 | 879-880 | |
| 15-spot-actor-R359 | Contract test: Join과 Relocate·Shutdown 경합에서 먼저 확정한 claim·seal에 따라 wait, Unavailable 또는 ShuttingDown으로 끝난다 | 881-882 | |
| 15-spot-actor-R360 | Contract test: Same-target User Spot Join과 Entry Spot Actor의 JoinEntrySpot을 Store mutation과 lifecycle callback이 없는 Accepted로 완료한다 | 883-884 | |
| 15-spot-actor-R361 | Contract test: Reply encoding 실패는 barrier를 폐기하지만 encoding 뒤 caller disconnect나 transport admission 실패는 Join을 취소하지 않는다 | 885-886 | |
| 15-spot-actor-R362 | Contract test: Cross-node join이 shared factory policy를 사용하며 same-node join은 DisableRelocation으로 차단하지 않는다 | 887 | |
| 15-spot-actor-R363 | Contract test: Same-node Join, cross-node Join과 RecreateOnRelocation에서 Actor ObjectGeneration을 유지하고 cross-node owner 변경에서만 AuthorityOwnerGeneration을 증가시킨다 | 888-889 | |
| 15-spot-actor-R364 | Contract test: Actor authority, source·target membership, capacity와 aggregate generation을 bounded aggregate commit 하나로 확정하며 후처리를 위해 같은 aggregate를 다시 commit하지 않는다 | 890-892 | |
| 15-spot-actor-R365 | Contract test: Same-node와 cross-node Join completion은 source와 target process가 실행되는 동안만 전달한다. Process 재시작 뒤 completion replay는 보장하지 않는다 | 893-894 | |
| 15-spot-actor-R366 | Contract test: Public Actor Join OperationId를 completion idempotency에만 사용하고 RelocationId, reservation ID와 aggregate commit ID를 재사용하지 않는다 | 895-897 | |
| 15-spot-actor-R367 | Contract test: Defer() 뒤 source seal 전 message는 barrier 뒤 Actor queue에 두고, seal 뒤 message만 relocation ingress hold에 보관한다 | 898-899 | |
| 15-spot-actor-R368 | Contract test: Cross-node Join의 target이 Actor instance보다 먼저 relocation temporary queue를 등록한다 — User Spot join은 OnActorJoin 승인 처리에서, Entry Spot join은 Restore 요청 처리에서 등록한다. Restore 중 message는 이 queue에서 application handler를 실행하지 않는다 | 900-902 | |
| 15-spot-actor-R369 | Contract test: Join 승인이 Rejected이면 target에 temporary queue와 준비한 factory 자원이 남지 않는다 | 903 | |
| 15-spot-actor-R370 | Contract test: 승인 왕복에서 준비를 마친 target도 Location Store CAS 전에는 application handler를 실행하지 않으며, 이동이 시작되지 않으면 준비 유효시간 뒤 준비 자원이 제거된다 | 904-905 | |
| 15-spot-actor-R371 | Contract test: 기존 준비가 남은 상태에서 다른 exact identity의 승인·Restore가 도착하면 기존 준비가 먼저 정리되고 나중 attempt가 유효하다 | 906-907 | |
| 15-spot-actor-R372 | Contract test: 승인 reply가 target의 유효 수신 chunk 상한을 실어 보내고, 승인 왕복이 없는 JoinEntrySpot은 32 KiB의 보수 chunk 크기를 사용한다 | 908-909 | |
| 15-spot-actor-R373 | Contract test: Restore 재시도의 payload 원본이 저장소가 아니라 source memory 재전송이다 | 910 | |
| 15-spot-actor-R374 | Contract test: 저장한 기존 Actor 작업을 실제 Actor queue에 먼저 넣고 temporary queue의 작업을 그 뒤에 옮긴 다음 기존 dispatch 경로로 atomic하게 전환한다 | 911-912 | |
| 15-spot-actor-R375 | Contract test: Relay-ready reply가 accepted 상태가 되기 전 abort에서만 target temporary queue를 실행하지 않고 폐기하며 source 원본만 다시 처리한다 | 913-914 | |
| 15-spot-actor-R376 | Contract test: RelocationId, target attempt와 owner generation이 같은 중복 Restore는 작업을 다시 시작하지 않고 기존 temporary queue와 진행 상태를 사용한다 | 915-916 | |
| 15-spot-actor-R377 | Contract test: Membership commit 뒤 OnJoinedActor, one-way OnLeaveActor, completion callback 순서를 지키고 completion callback 뒤에 일반 message를 실행한다 | 917-918 | |
| 15-spot-actor-R378 | Contract test: PreserveStateWith는 handler 종료 경계의 application state와 Framework queue·timer를 복원하고, RecreateOnRelocation은 application state 없이 Framework queue·timer만 복원한다 | 919-920 | |
| 15-spot-actor-R379 | Contract test: User Spot과 member Actor는 target이 실행하는 Location Store conditional batch CAS 한 번으로 함께 전환된다 | 921-922 | |
| 15-spot-actor-R380 | Contract test: Commit 뒤 failure가 participant 일부를 source로 rollback하지 않는다 | 923 | |
| 15-spot-actor-R381 | Contract test: Message Follow는 commit된 route만 사용한다. Relay queue에는 relocation 전용 record 수나 byte 상한을 두지 않으며, operation identity를 그대로 보존한다 | 924-926 | |
| 15-spot-actor-R382 | Contract test: Bound STREAM connection은 이동하지 않는다. Session owner 한 곳에서 current Session, binding generation과 relocation identity를 확인하고 해당 Actor route와 location snapshot만 target으로 바꾼다. ActorId·ObjectGeneration은 유지한다 | 927-929 | |

### 16-spot-address-messaging

| # | 규칙 (수치·상태·오류) | 옛 위치 | 새 위치 |
|---|---|---|---|
| 16-spot-address-messaging-R1 | User Spot과 Instance Spot은 같은 logical address와 placement lifecycle을 사용 | 18 | |
| 16-spot-address-messaging-R2 | User Spot만 Actor membership을 지원 | 19 | |
| 16-spot-address-messaging-R3 | Core는 raw socket과 transport만 제공; Spot kind·type, logical address, owner claim, generation, activation/maintenance authority는 해석하지 않음 | 21-22 | |
| 16-spot-address-messaging-R4 | Framework가 target 선택, Store transaction, route cache, activation barrier를 관리 | 24-25 | |
| 16-spot-address-messaging-R5 | User·Instance Spot은 Location Store namespace 전체에서 유일한 Spot ID로 식별 | 29 | |
| 16-spot-address-messaging-R6 | Spot ID는 UTF-8 encoded 크기 1..255 bytes case-sensitive exact string | 30 | |
| 16-spot-address-messaging-R7 | Spot ID는 transport routing identity가 아님; MeshNode의 NodeRid만 Core RoutingId 사용 | 31-32 | |
| 16-spot-address-messaging-R8 | Framework는 Spot address를 Core routing ID로 변환하거나 Spot ID 문자열을 parse해 owner node를 추론하지 않음 | 32-33 | |
| 16-spot-address-messaging-R9 | Location Store에서 current authority를 조회하고 그 결과의 NodeRid를 transport route로 사용 | 33-34 | |
| 16-spot-address-messaging-R10 | Service wire에서 Spot ID와 이로부터 파생한 field는 text8 또는 optional-text8로 encode | 36-37 | |
| 16-spot-address-messaging-R11 | Node RID field만 rid 또는 optional-rid encoding 사용 | 37 | |
| 16-spot-address-messaging-R12 | Framework는 이전 binary Spot address를 자동 decode하거나 base64·replacement character string으로 변환하지 않음 | 37-38 | |
| 16-spot-address-messaging-R13 | Invalid UTF-8, 0-byte, 256-byte 이상 값은 application admission과 Store mutation 전에 protocol/configuration failure로 거부 | 39-40 | |
| 16-spot-address-messaging-R14 | MeshName은 Spot 처음 배치 위치 결정에만 사용, identity에 미포함 | 42-43 | |
| 16-spot-address-messaging-R15 | 같은 Spot ID를 MeshName, Spot kind, stable type만 다르게 하여 동시에 사용 불가 | 43-44 | |
| 16-spot-address-messaging-R16 | User·Instance Spot type은 UTF-8 1..255 bytes case-sensitive stable name | 46 | |
| 16-spot-address-messaging-R17 | Framework는 normalization/case folding 적용 안 함, 언어 class FQN을 Store/wire identity로 사용 안 함 | 46-48 | |
| 16-spot-address-messaging-R18 | 같은 Object Server에 같은 stable type을 중복 등록하면 startup 오류 | 48-49 | |
| 16-spot-address-messaging-R19 | Entry Spot ID는 Framework가 발급, caller가 create 대상으로 지정 안 함 | 49 | |
| 16-spot-address-messaging-R20 | `<diagnostic-prefix>-entry-<lowercase-canonical-uuid-v4>` 형식은 Framework 발급 Entry Spot ID 전용 예약 | 51-52 | |
| 16-spot-address-messaging-R21 | UUID 부분은 MeshNode RID와 별도로 만드는 RFC 4122 UUID v4 값 | 52 | |
| 16-spot-address-messaging-R22 | Caller가 지정한 User·Instance Spot ID가 이 예약 형식과 일치하면 Location Store reservation/factory 시작 전 InvalidOperation으로 거부 | 53-54 | |
| 16-spot-address-messaging-R23 | Framework는 Spot ID 문자열로 MeshNode 관계를 계산하지 않고 MeshNode descriptor가 게시한 exact Entry Spot ID mapping을 사용 | 54-55 | |
| 16-spot-address-messaging-R24 | Entry Spot ID는 같은 Object Server lifecycle 동안 유지 | 57 | |
| 16-spot-address-messaging-R25 | Endpoint가 같은 replacement lifecycle에서도 새 MeshNode RID와 새 Entry Spot ID를 각각 발급 | 57-58 | |
| 16-spot-address-messaging-R26 | Framework는 full MeshNode RID를 이어 붙여 Entry Spot ID를 만들지 않음 | 58-59 | |
| 16-spot-address-messaging-R27 | Object Server descriptor의 NewClaim은 (MeshName, NodeRid) descriptor identity와 EntrySpotId의 global Spot identity claim을 exact owner lease·lifecycle에 연결해 하나의 Location Store transaction에서 생성 | 61-63 | |
| 16-spot-address-messaging-R28 | 둘 중 하나라도 active claim과 충돌하면 descriptor, Entry claim, index를 모두 변경하지 않고 첫 claim에서 startup configuration error 반환 | 63-64 | |
| 16-spot-address-messaging-R29 | 두 번째 Entry UUID나 claim은 만들지 않음 | 64 | |
| 16-spot-address-messaging-R30 | Descriptor remove와 owner cleanup은 저장된 descriptor의 exact owner lease·lifecycle이 요청과 일치할 때만 연결된 Entry claim을 같은 transaction에서 해제 | 66-68 | |
| 16-spot-address-messaging-R31 | 이전 lifecycle의 stale cleanup은 replacement lifecycle의 descriptor나 Entry claim을 삭제할 수 없음 | 67-68 | |
| 16-spot-address-messaging-R32 | EntrySpotId는 descriptor immutable field와 immutable digest에 포함, Renew나 mutable descriptor update로 바꿀 수 없음 | 68-69 | |
| 16-spot-address-messaging-R33 | User·Instance Spot의 generic Reserve도 같은 global namespace를 검사하므로 active Entry Spot ID를 caller-created Spot authority로 사용할 수 없음 | 69-71 | |
| 16-spot-address-messaging-R34 | SpotRef는 조회 시점 위치를 나타내는 변경 불가 snapshot: global SpotId, non-zero unsigned 63-bit ObjectGeneration, 조회 시점 MeshName과 NodeRid | 73-78 | |
| 16-spot-address-messaging-R35 | ObjectGeneration은 JSON에서 decimal string으로 표현 | 79 | |
| 16-spot-address-messaging-R36 | SpotRef는 messaging target이나 owner capability가 아님 | 80 | |
| 16-spot-address-messaging-R37 | Owner가 이동하면 MeshName과 NodeRid가 현재 위치와 다를 수 있음 | 81 | |
| 16-spot-address-messaging-R38 | 현재 위치 확인하려면 Spot ID로 다시 조회; SpotHandle, 별도 resolver handle, InstanceSpotAddress는 제공하지 않음 | 83-84 | |
| 16-spot-address-messaging-R39 | Instance Spot은 Actor membership이 없는 Spot | 86 | |
| 16-spot-address-messaging-R40 | Instance Spot에서 Direct packet handler, timer, outbound call은 사용 가능; Actor create·join·leave·relocation과 Logical Multicast subscription은 사용 불가 | 86-87 | |
| 16-spot-address-messaging-R41 | Spot manager의 Create/GetOrCreate는 User Spot만 명시적으로 생성 | 91 | |
| 16-spot-address-messaging-R42 | Create: Caller는 required stable type만 지정, Framework가 global Spot ID를 만듦 | 93-96 | |
| 16-spot-address-messaging-R43 | GetOrCreate: Caller가 global Spot ID와 stable type을 모두 지정 | 93-96 | |
| 16-spot-address-messaging-R44 | Instance Spot kind를 받는 manager overload와 Instance Spot 전용 create operation은 제공하지 않음 | 98-99 | |
| 16-spot-address-messaging-R45 | 두 operation은 target node나 endpoint를 받지 않으며 한 번만 제출할 수 있는 fluent call | 99-100 | |
| 16-spot-address-messaging-R46 | InMesh, encoded creation request, timeout은 선택 항목 | 102 | |
| 16-spot-address-messaging-R47 | Caller callback, target RID와 predicate를 받지 않음 | 102-103 | |
| 16-spot-address-messaging-R48 | 같은 option을 두 번 설정하거나 terminal submit을 두 번 실행하면 InvalidOperation | 104 | |
| 16-spot-address-messaging-R49 | Terminal submit 시작 시 resolve, reservation, factory, Ready barrier 전체에 적용할 end-to-end deadline 하나를 고정 | 104-106 | |
| 16-spot-address-messaging-R50 | InMesh 지정 시 해당 Mesh 사용; 생략 시 object Client/Server role Mesh가 하나면 자동 선택 | 139-140 | |
| 16-spot-address-messaging-R51 | 후보 0개면 NotConfigured, 둘 이상이면 InvalidOperation, 명시한 Mesh가 없으면 NotFound | 140-141 | |
| 16-spot-address-messaging-R52 | Framework는 role, stable type capability, active·pending capacity를 먼저 검사하고 남은 후보를 node-wide placement weight로 선택 | 141-142 | |
| 16-spot-address-messaging-R53 | Encoded creation request는 최대 1 MiB | 144 | |
| 16-spot-address-messaging-R54 | Framework는 reservation 전에 변경 불가 content reference와 hash를 creation intent에 기록, Spot이 Ready 되거나 실패한 생성을 정리할 때까지 유지 | 144-146 | |
| 16-spot-address-messaging-R55 | 생성 권한을 얻은 target만 request를 factory에 전달 | 146-147 | |
| 16-spot-address-messaging-R56 | Factory는 (SpotId, ObjectGeneration, creation attempt) 기준 한 번 이상 실행될 수 있으므로 같은 입력의 재실행을 안전하게 처리해야 함 | 147-149 | |
| 16-spot-address-messaging-R57 | Create는 lowercase canonical UUID v4 문자열을 automatic global Spot ID로 발급 | 151 | |
| 16-spot-address-messaging-R58 | Active authority와 충돌하면 새 UUID나 reservation을 만들지 않고 AlreadyExists로 terminal completion 반환 | 152-153 | |
| 16-spot-address-messaging-R59 | 같은 caller Spot ID의 kind 또는 stable type이 다르면 TypeMismatch | 154 | |
| 16-spot-address-messaging-R60 | GetOrCreate는 같은 User Spot type의 Ready object를 Existing으로 반환 | 155 | |
| 16-spot-address-messaging-R61 | 진행 중인 Creating attempt를 관찰한 서로 다른 operation은 새 reservation·factory를 시작하지 않고 authority 변경을 기다림 | 155-157 | |
| 16-spot-address-messaging-R62 | 앞선 attempt가 Ready로 끝나면 Existing과 그 incarnation의 SpotRef를 반환 | 157-158 | |
| 16-spot-address-messaging-R63 | Rejected·failure cleanup으로 Missing이 되면 남은 deadline 안에서 새 reservation을 경쟁, winner가 자신의 creation request로 factory와 callback 실행 | 158-160 | |
| 16-spot-address-messaging-R64 | 서로 다른 operation은 앞선 attempt의 Rejected state와 application reply를 공유하지 않음 | 160-161 | |
| 16-spot-address-messaging-R65 | 동일한 operation ID가 재전달된 경우에만 retained terminal result를 재전송 | 161-162 | |
| 16-spot-address-messaging-R66 | Deadline까지 authority가 Ready/Missing으로 안 바뀌면 DeadlineExceeded; 다음 call은 Store 현재 authority를 다시 확인 | 162-164 | |
| 16-spot-address-messaging-R67 | Terminal result는 해당 attempt의 SpotRef, Existing·Created·Rejected state, optional creation reply를 함께 반환 | 166-167 | |
| 16-spot-address-messaging-R68 | Existing: 같은 stable type의 Ready incarnation 사용, factory callback 실행 안 함 | 167-168 | |
| 16-spot-address-messaging-R69 | Created: 새 incarnation이 Ready로 commit됨 | 168 | |
| 16-spot-address-messaging-R70 | Rejected: application create callback이 거부해 reservation과 authority를 정리함 | 168-169 | |
| 16-spot-address-messaging-R71 | Rejected의 SpotRef는 거부된 attempt를 식별하며 current Ready location을 보장하지 않음 | 169-170 | |
| 16-spot-address-messaging-R72 | Reply는 create callback이 반환한 opaque framework message; Existing에서는 비어 있음 | 170-171 | |
| 16-spot-address-messaging-R73 | Owner가 다른 MeshNode이면 source는 Location Store에 generic reservation을 만든 후 command 47 userSpotCreate를 선택 target으로 보냄 | 173-175 | |
| 16-spot-address-messaging-R74 | command 47에 담기는 값: reply correlation, terminal result를 한 번만 만드는 operation ID, source node RID·lifecycle generation, global Spot ID와 stable type, provider 발급 reservation fence, create 전체에 적용하는 하나의 deadline | 176-182 | |
| 16-spot-address-messaging-R75 | Reservation fence에 포함: expected StoreVersion, ObjectGeneration, AuthorityOwnerGeneration, target node RID와 lifecycle generation, target owner lease, pending capacity delta | 184-186 | |
| 16-spot-address-messaging-R76 | Creation request bytes는 command payload로 다시 보내지 않음 | 186-187 | |
| 16-spot-address-messaging-R77 | Target은 Location Store의 Pending creation projection에서 reference, hash, encoded size를 exact read하고 변경되지 않은 creation content 확인 후에만 factory/initialize 실행 | 187-189 | |
| 16-spot-address-messaging-R78 | Target은 같은 reservation을 commit한 결과를 command 20 reply로 한 번만 반환 | 191 | |
| 16-spot-address-messaging-R79 | Reply의 correlation, terminalResult, failureCode, operation-specific tail 순서는 바꾸지 않음 | 192-193 | |
| 16-spot-address-messaging-R80 | 성공 tail: Existing·Created·Rejected와 exact SpotRef 포함; Existing에는 application reply 없음, Created/Rejected에는 callback이 만든 reply가 선택적으로 포함될 수 있음 | 193-195 | |
| 16-spot-address-messaging-R81 | Source는 Location row를 조회한 결과를 현재 call의 terminal reply로 사용하지 않으며, application packet으로 create control을 대신 만들 수 없음 | 195-197 | |
| 16-spot-address-messaging-R82 | Manager Find(SpotId)는 current Ready authority의 SpotRef를 반환, creation을 시작하지 않음 | 199 | |
| 16-spot-address-messaging-R83 | Manager가 제공하는 query: current Spot query와 page size 1..1000, encoded 4 MiB 이하 operational query 외에 unbounded list와 별도 resolver는 제공하지 않음 | 199-201 | |
| 16-spot-address-messaging-R84 | Spot direct call은 기본적으로 이미 존재하는 Spot만 호출; Instance Spot intent 없는 send·request는 RID Missing이어도 factory 실행하거나 creation intent를 만들지 않음 | 205-207 | |
| 16-spot-address-messaging-R85 | Location Store authority가 Missing인 Instance Spot을 새로 만들어 최초 message를 처리할 수 있게 준비하는 과정을 cold activation이라 함 | 245-246 | |
| 16-spot-address-messaging-R86 | Cold activation을 허용하려면 같은 Spot direct call builder에서 Instance intent를 명시 | 247-248 | |
| 16-spot-address-messaging-R87 | Builder는 stable type 생략 형식과 명시 형식을 모두 제공 | 250 | |
| 16-spot-address-messaging-R88 | InMesh는 Instance intent를 명시한 call에서만 사용 가능, Missing Spot을 처음 배치할 Mesh를 선택 | 250-252 | |
| 16-spot-address-messaging-R89 | Existing Ready owner를 다른 Mesh로 이동하거나 현재 placement를 제한하는 option이 아님 | 252-253 | |
| 16-spot-address-messaging-R90a | Terminal 순서(1) global Spot ID의 current authority를 조회 | 257 | |
| 16-spot-address-messaging-R90b | Terminal 순서(2) Ready authority 있으면 저장된 kind와 stable type을 사용해 current owner로 전송 | 258 | |
| 16-spot-address-messaging-R90c | Terminal 순서(3) authority Missing이고 Instance intent 없으면 NotFound로 끝냄 | 259 | |
| 16-spot-address-messaging-R90d | Terminal 순서(4) authority Missing이고 Instance intent 있으면 eligible Object Mesh 선택; InMesh 생략하고 후보 0개면 NotConfigured, 둘 이상이면 InvalidOperation | 260-261 | |
| 16-spot-address-messaging-R90e | Terminal 순서(5) stable type 명시하면 해당 capability serving node만 후보; 없는 node면 NotFound | 262-263 | |
| 16-spot-address-messaging-R90f | Terminal 순서(6) stable type 생략하면 Mesh serving descriptor의 distinct Instance type 계산; 하나면 자동 선택, 0개면 NotFound, 둘 이상이면 required type 생략한 InvalidOperation | 264-265 | |
| 16-spot-address-messaging-R90g | Terminal 순서(7) 선택 stable type을 제공하지만 capacity 남은 node가 없으면 CapacityExceeded | 266 | |
| 16-spot-address-messaging-R90h | Terminal 순서(8) Source는 여러 값을 하나의 activation envelope에 넣어 target으로 보냄(global Spot ID, 선택 Mesh·stable type과 target descriptor fence, source node RID·lifecycle generation·optional source Spot ID, operation identity·reply correlation·deadline, command 39 optional metadata 존재여부와 metadata frame, 최초 application message); 이 시점에 Source는 자신/target을 owner로 등록하지 않음 | 267-271 | |
| 16-spot-address-messaging-R91 | Command 39의 route kind 1은 이미 Ready인 authority의 exact generation fence를 사용 | 272-273 | |
| 16-spot-address-messaging-R92 | Missing cold activation은 route kind 2 사용, target Mesh·node RID·lifecycle, Spot ID, stable type, descriptor version, deadline만 전달; 아직 존재하지 않는 authority generation은 포함 안 함 | 273-276 | |
| 16-spot-address-messaging-R93 | Route kind 2의 deadline은 Relocation Store에 기록하는 instance-activation-recovery-v1의 deadline과 같아야 함 | 276-277 | |
| 16-spot-address-messaging-R94 | Cold activation send/request는 모두 중복 실행을 막는 non-zero operation identity를 사용하며 metadata flag와 ZLIA metadata presence도 같아야 함 | 277-279 | |
| 16-spot-address-messaging-R95 | (Target 처리 계속) Target은 Location Store의 현재 owner 기록과 자신의 Spot 목록을 함께 확인; 현재 owner가 자신이고 같은 generation Spot이 이미 있으면 최초 message를 그 Spot 기존 queue에 넣음 | 280-282 | |
| 16-spot-address-messaging-R96 | 자신의 목록에 Spot이 있더라도 Store가 다른 owner/generation을 가리키면 오래된 Spot으로 판단해 message를 실행하지 않음 | 282-283 | |
| 16-spot-address-messaging-R97 | Store에 owner 없고 target에도 사용할 Spot 없으면 target은 complete activation envelope를 Relocation Store에 변경 불가 recovery root로 저장 | 284-285 | |
| 16-spot-address-messaging-R98 | reference, SHA-256, encoded size, retention 확인 후 자신에게 생성 가능 여부를 Reserve로 요청 | 285-287 | |
| 16-spot-address-messaging-R99 | Location Store는 target의 lifecycle, owner lease, type, 남은 capacity를 다시 확인 | 287-288 | |
| 16-spot-address-messaging-R100 | 조건 만족 시 object 상태를 Missing에서 Creating으로 바꿈 (Missing → Creating transition) | 288-290 | |
| 16-spot-address-messaging-R101 | 같은 transaction에서 recovery receipt, provider 발급 reservation fence, 생성 중 capacity를 기록 | 290-291 | |
| 16-spot-address-messaging-R102 | 예약 성공한 target만 factory와 initialize를 실행 | 292 | |
| 16-spot-address-messaging-R103 | 최초 message를 durable activation inbox 첫 record로 확정할 때까지 handler 실행은 barrier로 차단 | 292-294 | |
| 16-spot-address-messaging-R104 | 같은 reservation의 Commit은 recovery root와 replay cursor를 유지하는 Ready authority를 게시하고 active capacity를 게시 | 294-296 | |
| 16-spot-address-messaging-R105 | Runtime은 첫 record를 local queue 선두에 복원 후 barrier를 엶 | 296-297 | |
| 16-spot-address-messaging-R106 | 후속 message는 이 record를 추월할 수 없으며 Source는 준비 완료 뒤 같은 message를 다시 보내지 않음 | 297-298 | |
| 16-spot-address-messaging-R107 | 최초 handler 완료를 durable 기록하고 replay cursor를 inbox sequence까지 갱신한 뒤에만 expected-version Preserve CAS로 recovery pointer를 제거 | 299-301 | |
| 16-spot-address-messaging-R108 | Queue에 넣었다는 사실만으로 pointer를 제거하지 않음 | 301-302 | |
| 16-spot-address-messaging-R109 | CAS 성공 후 Relocation Store의 root를 idempotent하게 삭제 | 302 | |
| 16-spot-address-messaging-R110 | 이미 Ready owner가 있으면 factory 실행하지 않고 기존 Spot queue에 request를 넣음 | 332-333 | |
| 16-spot-address-messaging-R111 | 다른 target이 먼저 생성 권한을 얻었다면 현재 target은 Spot을 만들지 않음 | 333-334 | |
| 16-spot-address-messaging-R112 | 권한 얻은 target의 Spot이 Ready가 되면 최초 request의 식별정보와 deadline을 유지해 현재 owner에 한 번만 전달 | 334-335 | |
| 16-spot-address-messaging-R113 | 여러 MeshNode가 같은 stable Instance type을 등록해도 type은 하나이고 배치 후보 node가 여러 개인 것으로 처리 | 337-338 | |
| 16-spot-address-messaging-R114 | 동시에 보낸 첫 message가 서로 다른 target에 도착해도 Store에서 생성 권한을 얻은 target 하나만 factory 실행; 나머지는 local Spot을 만들지 않음 | 338-340 | |
| 16-spot-address-messaging-R115 | 권한 얻은 Spot이 이미 Ready면 최초 operation의 identity, payload, reply correlation, deadline을 유지해 current owner로 한 번만 전달 | 342-343 | |
| 16-spot-address-messaging-R116 | 아직 Creating이면 같은 activation 완료를 기다림 | 343-344 | |
| 16-spot-address-messaging-R117 | 기존 authority가 User Spot이거나 builder 명시 stable type과 다르면 TypeMismatch | 344-345 | |
| 16-spot-address-messaging-R118 | 기존 Instance Spot에 type 명시 안 한 일반 direct call은 authority에 저장된 type을 사용, 등록 type 수와 관계없이 전송 가능 | 345-347 | |
| 16-spot-address-messaging-R119 | Spot direct send/request 시작 method는 global Spot ID와 typed payload만 받음 | 351 | |
| 16-spot-address-messaging-R120 | Framework는 positive route cache 또는 Location Store에서 current Ready Spot과 owner route를 resolve | 351-352 | |
| 16-spot-address-messaging-R121 | Resolve 시 확인한 ObjectGeneration은 route snapshot에 기록하지만 application message target 일치 조건으로 사용하지 않음 | 352-353 | |
| 16-spot-address-messaging-R122 | Local과 remote owner는 같은 handler, metadata, completion 의미를 가짐 | 354 | |
| 16-spot-address-messaging-R123 | Instance intent 없는 direct call은 existing-only operation, 이미 존재하는 Ready Spot만 대상 | 355 | |
| 16-spot-address-messaging-R124 | Missing, Creating, Store failure는 negative cache에 저장하지 않음 | 357 | |
| 16-spot-address-messaging-R125 | Positive Ready cache는 current owner lease의 local admission deadline과 RouteCacheMaxAge 안에서만 사용 | 358-359 | |
| 16-spot-address-messaging-R126 | Higher StoreVersion, stale result, Store recovery event 확인하면 즉시 invalidate | 360 | |
| 16-spot-address-messaging-R127 | Resolve 뒤 같은 owner에서 close와 recreate 발생 시 target queue가 수락하는 시점의 current Ready Spot이 message 처리 | 361-362 | |
| 16-spot-address-messaging-R128 | Resolve한 owner가 더 이상 해당 SpotId를 소유하지 않으면 현재 operation은 stale route 오류로 끝남; Framework는 fresh owner를 찾아 자동으로 다시 보내지 않음 | 363-364 | |
| 16-spot-address-messaging-R129 | Timeout, cancellation, disconnect와 실행 여부 불명확한 failure 뒤 다른 owner에게 자동 재제출하지 않음 | 365 | |
| 16-spot-address-messaging-R130 | One-way call은 local outbound admission까지만 기다림; cold activation 필요해도 application handler 실행은 기다리지 않음 | 367-368 | |
| 16-spot-address-messaging-R131 | Outbound admission은 activation envelope가 선택한 target transport에 수락된 시점, reservation이나 Ready commit 완료를 뜻하지 않음 | 368-369 | |
| 16-spot-address-messaging-R132 | Request는 resolve, cold activation, 최초 message dispatch, reply를 하나의 deadline 안에서 terminal-once로 완료 | 369-370 | |
| 16-spot-address-messaging-R133 | Target queue admission 뒤의 failure를 current owner를 다시 찾아 hidden retry하지 않음 | 370-371 | |
| 16-spot-address-messaging-R134 | RouteCacheMaxAge 기본값 15초, MessageFollowDuration 기본값 30초 | 375 | |
| 16-spot-address-messaging-R135 | 둘 다 0이면 각각 cache와 Message Follow를 끔 | 375-376 | |
| 16-spot-address-messaging-R136 | 두 값이 양수이면 cache max age가 Message Follow duration보다 최소 5초 작아야 함 | 376 | |
| 16-spot-address-messaging-R137 | Runtime 변경은 새 cache entry와 새 relocation에만 적용 | 377 | |
| 16-spot-address-messaging-R138 | Relocation commit 뒤 source는 commit된 source→target Message Follow route만 사용해 이전 physical route로 도착한 message를 current owner에 전달 | 379-380 | |
| 16-spot-address-messaging-R139 | Message Follow 중에는 Store를 읽거나 application handler를 실행하지 않음 | 380-381 | |
| 16-spot-address-messaging-R140 | Message Follow route는 Spot ID, ObjectGeneration, source·target AuthorityOwnerGeneration과 owner fence를 exact 검증 | 381-383 | |
| 16-spot-address-messaging-R141 | Target owner generation은 hop마다 증가하며 최대 8 hops | 383-384 | |
| 16-spot-address-messaging-R142 | Message Follow route 하나의 대기열에는 message 수와 저장 크기 어느 쪽에도 상한을 두지 않으며 negotiated message bound는 지킴 | 386-387 | |
| 16-spot-address-messaging-R143 | Message Follow는 original operation ID, generation, payload, reply route를 보존 | 387-388 | |
| 16-spot-address-messaging-R144 | Route 없음·만료와 loop는 Unavailable, generation mismatch는 InvalidOperation | 388-389 | |
| 16-spot-address-messaging-R145 | Failed application operation을 Store에서 찾은 owner에게 다시 제출하지 않으며 다음 call만 fresh resolve를 수행 | 389-390 | |
| 16-spot-address-messaging-R146 | Spot direct send/request의 target은 SpotId이며 ObjectGeneration mismatch로 current Ready Spot의 handler 실행을 거부하지 않음 | 392-394 | |
| 16-spot-address-messaging-R147 | SpotWide User Spot relocation은 Spot과 member Actor의 Message Follow route를 같은 aggregate commit에서 설치, 개별 participant route를 commit 전에 current route로 공개하지 않음 | 396-398 | |
| 16-spot-address-messaging-R148 | PerActor User Spot relocation은 Spot과 Actor의 Message Follow route를 분리 | 400 | |
| 16-spot-address-messaging-R149 | Spot authority commit 뒤 ToSpot, Actor Create와 Join은 target으로 보냄 | 401 | |
| 16-spot-address-messaging-R150 | 아직 source에 남은 Actor의 ToActor route는 해당 Actor의 current owner를 계속 가리킴 | 401-402 | |
| 16-spot-address-messaging-R151 | Actor가 이전될 때마다 Actor별 source→target Message Follow route를 설치 | 402-403 | |
| 16-spot-address-messaging-R152 | Relocation unit을 seal한 뒤 source route로 도착한 ingress는 relocation hold에 보관하며, application handler는 실행하지 않음 | 405-406 | |
| 16-spot-address-messaging-R153 | Relay-ready reply가 accepted 상태가 되기 전에 명시적으로 abort하면 보관한 ingress를 도착 순서대로 source queue에 되돌림 | 406-408 | |
| 16-spot-address-messaging-R154 | 그 뒤에는 cutover submit 결과와 관계없이 source를 복원하지 않고 operation ID, generation, reply route를 그대로 유지해 Message Follow route로 target에 relay | 408-409 | |
| 16-spot-address-messaging-R155 | Permit을 기다리는 Relocating unit은 아직 seal되지 않음; 따라서 기존 owner route에서 application message와 timer를 계속 수락 | 411-412 | |
| 16-spot-address-messaging-R156 | Spot manager의 public Close는 User Spot의 exact SpotRef를 받음 | 416-417 | |
| 16-spot-address-messaging-R157 | Instance Spot은 application handler나 timer가 자신의 lifecycle context에서 local Close를 요청함 | 417-418 | |
| 16-spot-address-messaging-R158 | Host shutdown과 Relocate는 별도 운영 lifecycle로 Instance Spot을 정리하거나 이동할 수 있음 | 418 | |
| 16-spot-address-messaging-R159 | Close 절차 4단계: (1)Expected owner·ObjectGeneration 검증해 authority를 Closing으로 전이 (2)local admission seal, seal 전 수락 turn·timer를 정해진 boundary까지 처리 (3)handler scope·timer·local activation resource를 한 번 정리 (4)같은 owner·generation fence로 authority release | 420-423 | |
| 16-spot-address-messaging-R160 | 같은 incarnation이 이미 없으면 idempotent false, 같은 Spot ID의 다른 generation이 있으면 InvalidOperation, 이동 seal 중이면 Unavailable | 425-426 | |
| 16-spot-address-messaging-R161 | Framework는 current ref를 다시 찾아 새 incarnation을 닫지 않음 | 426-427 | |
| 16-spot-address-messaging-R162 | Seal 전 accepted operation은 기존 generation에서 완료할 수 있지만 seal 뒤 operation은 closing 또는 stale 결과로 끝남 | 427-428 | |
| 16-spot-address-messaging-R163 | User Spot에 current Actor membership이 하나라도 있으면 Close는 false로 끝나며 admission과 authority를 유지 | 430-431 | |
| 16-spot-address-messaging-R164 | Framework는 member Actor를 숨겨서 이동하거나 destroy하지 않음 | 431 | |
| 16-spot-address-messaging-R165 | Remote owner를 닫을 때 source는 command 48 userSpotClose를 current owner로 보냄 | 433-434 | |
| 16-spot-address-messaging-R166 | Request에 포함되는 값: correlation, terminal result를 한 번만 만들 operation ID, source node RID·lifecycle generation, exact SpotRef, target node RID·lifecycle generation, expected AuthorityOwnerGeneration·StoreVersion과 하나의 deadline | 434-437 | |
| 16-spot-address-messaging-R167 | Target은 service admission에서 확인한 peer identity와 target lifecycle을 먼저 검증하고 current User Spot authority를 exact read | 439-441 | |
| 16-spot-address-messaging-R168 | 그다음 object generation, owner generation, StoreVersion, active Actor membership, Closing과 relocation 상태를 모두 확인한 뒤에만 Closing CAS와 local admission seal을 시작 | 441-442 | |
| 16-spot-address-messaging-R169 | Command 20의 close 성공 tail은 closed bool 하나 | 444 | |
| 16-spot-address-messaging-R170 | false는 같은 incarnation이 이미 없거나 active membership 때문에 authority를 유지한 경우에만 사용 | 444-446 | |
| 16-spot-address-messaging-R171 | Stale generation과 moving conflict는 typed failure | 446 | |
| 16-spot-address-messaging-R172 | Source는 current ref를 다시 찾아 다른 incarnation으로 target을 바꾸지 않으며 Location row 조회를 completion으로 사용하지 않음 | 446-448 | |
| 16-spot-address-messaging-R173 | 이미 존재하는 Spot owner의 이동은 명시적인 host Relocate transaction만 시작 | 452 | |
| 16-spot-address-messaging-R174 | Object Server factory는 DisableRelocation, RecreateOnRelocation, PreserveStateWith 중 하나를 반드시 선택; 생략 overload와 compatibility default는 제공하지 않음 | 452-454 | |
| 16-spot-address-messaging-R175 | PreserveStateWith는 Spot type에 맞는 SpotRelocationAdapter를 요구; Adapter는 application이 형식·version을 관리하는 opaque byte sequence를 capture·restore | 454-455 | |
| 16-spot-address-messaging-R176 | PerActor User Spot은 RecreateOnRelocation만 허용하고 Spot adapter를 사용하지 않음 | 457 | |
| 16-spot-address-messaging-R177 | Target Spot shell은 같은 public SpotId와 ObjectGeneration을 유지하지만 Location Store authority가 target으로 바뀌기 전까지 resolver와 application handler에 노출하지 않음 | 458-460 | |
| 16-spot-address-messaging-R178 | 임시 public SpotId를 만들거나 생성 뒤 SpotId를 바꾸지 않음 | 460 | |
| 16-spot-address-messaging-R179 | Relay-ready reply가 accepted 상태가 되기 전 명시적 failure만 source를 유지 | 463-464 | |
| 16-spot-address-messaging-R180 | 그 뒤에는 cutover submit 결과와 관계없이 source를 복원하지 않고 selection이 끝난 같은 target process에서만 절차를 계속 | 464-466 | |
| 16-spot-address-messaging-R181 | Target process가 종료되면 다른 target을 선택하거나 relocation을 자동으로 재개하지 않음 | 466 | |
| 16-spot-address-messaging-R182 | Seal 시점의 실행하지 않은 message, accepted journal과 timer logical registration·pending tick은 relocation payload에 포함하며 target Framework가 timer를 자동 복원 | 466-468 | |
| 16-spot-address-messaging-R183 | Application은 Restore에서 Framework timer를 다시 등록하지 않음 | 468 | |
| 16-spot-address-messaging-R184 | 이 queue·timer 규칙은 SpotWide와 Instance Spot에 적용 | 469 | |
| 16-spot-address-messaging-R185 | PerActor에서는 Actor queue와 Actor timer만 Actor와 함께 이전하고 Spot-level application timer는 이전하지 않음 | 469-470 | |
| 16-spot-address-messaging-R186 | Original send·request를 maintenance target에 새 operation으로 자동 재제출하지 않지만 seal 뒤 source ingress hold는 commit된 Message Follow route로 relay | 472-473 | |
| 16-spot-address-messaging-R187 | Object role Mesh 후보가 없거나 여러 개인 create와 cold activation은 §3·§4의 typed error로 끝남 | 477 | |
| 16-spot-address-messaging-R188 | Ready authority 없으면 NotFound, exact generation이 다르면 InvalidOperation, owner fence가 다르면 Unavailable | 478 | |
| 16-spot-address-messaging-R189 | Closing 또는 Draining owner는 신규 admission을 거부 | 479 | |
| 16-spot-address-messaging-R190 | Relocation seal 이후 source route로 도착한 ingress는 거부하지 않고 relocation hold에 보관 | 479-481 | |
| 16-spot-address-messaging-R191 | Relocating이지만 아직 seal하지 않은 unit은 기존 owner admission을 유지 | 481 | |
| 16-spot-address-messaging-R192 | Request failure를 다른 Spot ID, MeshName이나 owner로 우회하지 않음 | 482 | |
| 16-spot-address-messaging-R193 | Expired owner는 신규 message·timer admission과 location update를 수행할 수 없음 | 483 | |
| 16-spot-address-messaging-R194 | 관측 정보는 구분: global Spot ID, current MeshName, ObjectGeneration, resolve·cache 결과, creation attempt, cold activation·close·maintenance operation kind, Message Follow hop·drop과 stale 분류 | 485-486 | |
| 16-spot-address-messaging-R195 | Spot ID는 metric label로 사용하지 않음 | 487 | |
| 16-spot-address-messaging-R196 | 검증: Spot ID가 Store namespace 전체의 global key이고 MeshName별 중복을 허용하지 않음 | 491 | |
| 16-spot-address-messaging-R197 | 검증: Caller가 예약 형식으로 User·Instance Spot ID를 지정하면 Store reservation·factory 실행 전에 InvalidOperation으로 거부 | 492-493 | |
| 16-spot-address-messaging-R198 | 검증: User Spot Create가 lowercase canonical UUID v4 문자열을 발급하고 active conflict에서 두 번째 UUID를 만들지 않음 | 494 | |
| 16-spot-address-messaging-R199 | 검증: Entry Spot join과 placement가 descriptor의 exact lifecycle mapping을 사용하고 Spot ID 문자열을 parsing하지 않음 | 495-496 | |
| 16-spot-address-messaging-R200 | 검증: User Spot Create·GetOrCreate가 target RID와 endpoint를 application에 요구하지 않음 | 497 | |
| 16-spot-address-messaging-R201 | 검증: Spot manager가 Instance Spot create·get-or-create를 제공하지 않음 | 498 | |
| 16-spot-address-messaging-R202 | 검증: Concurrent create가 authority attempt와 factory execution 하나로 수렴 | 499 | |
| 16-spot-address-messaging-R203 | 검증: Remote User Spot create가 provider reservation과 target lifecycle을 command 47에 고정하고 Pending creation content를 exact read한 뒤 command 20으로 terminal result를 한 번만 반환 | 500-502 | |
| 16-spot-address-messaging-R204 | 검증: SpotRef가 public exact generation을 보존하되 messaging target으로 사용되지 않음 | 503 | |
| 16-spot-address-messaging-R205 | 검증: Spot direct 시작 method가 Spot ID만 받고 owner route를 요구하지 않음 | 504 | |
| 16-spot-address-messaging-R206 | 검증: Instance intent 없는 Missing Spot message가 creation intent를 만들지 않음 | 505 | |
| 16-spot-address-messaging-R207 | 검증: Instance intent가 Missing Spot에서만 optional initial Mesh와 stable type을 사용해 cold activation을 시작 | 506 | |
| 16-spot-address-messaging-R208 | 검증: 선택한 Mesh의 distinct Instance type이 하나면 자동 선택, 여러 개면 명시 요구 | 507 | |
| 16-spot-address-messaging-R209 | 검증: Cold activation source가 owner claim을 만들지 않고 최초 message 포함 activation envelope를 target에 제출 | 508-509 | |
| 16-spot-address-messaging-R210 | 검증: 생성 권한 얻은 target만 자신을 owner로 기록·factory 실행; Durable inbox 첫 record를 Ready 전에 확정, recovery pointer 유지한 채 queue 선두 복원 후 barrier 개방 | 510-512 | |
| 16-spot-address-messaging-R211 | 검증: Store 현재 authority와 불일치하는 local Instance에는 message를 전달하지 않음; 생성 권한 못 얻은 target은 별도 instance를 만들지 않음 | 513-514 | |
| 16-spot-address-messaging-R212 | 검증: Missing, Creating과 Store failure를 negative cache하지 않음 | 515 | |
| 16-spot-address-messaging-R213 | 검증: Message Follow는 commit된 route만 사용; Relay queue에는 relocation 전용 record 수·byte 상한을 두지 않으며 operation identity를 그대로 보존 | 516-517 | |
| 16-spot-address-messaging-R214 | 검증: Close가 exact generation을 검사하고 새 incarnation으로 retarget하지 않음 | 518 | |
| 16-spot-address-messaging-R215 | 검증: User Spot Close가 active membership을 숨겨서 정리하지 않음 | 519 | |
| 16-spot-address-messaging-R216 | 검증: Remote User Spot Close가 exact SpotRef, owner generation, StoreVersion, target lifecycle을 command 48에 고정하고 Location row 조회나 application control packet을 completion으로 사용하지 않음 | 520-522 | |
| 16-spot-address-messaging-R217 | 검증: C++, .NET, JVM, Node.js가 create 경쟁·logical messaging·cold activation·close·Message Follow에서 같은 terminal 결과를 제공 | 523-524 | |

### 17-stage-wrapper-on-spot

| # | 규칙 (수치·상태·오류) | 옛 위치 | 새 위치 |
|---|---|---|---|
| 17-stage-wrapper-on-spot-R1 | Framework는 별도 Stage runtime이나 공통 Stage base type을 제공하지 않는다 | 19-20 | |
| 17-stage-wrapper-on-spot-R2 | application의 domain wrapper가 Spot의 public 등록·메시징·timer·lifecycle 표면을 조합한다 | 20 | |
| 17-stage-wrapper-on-spot-R3 | Spot identity, 생성·종료와 application turn은 Framework Spot runtime이 소유 | 27 | |
| 17-stage-wrapper-on-spot-R4 | Spot direct와 Logical Multicast dispatch는 Framework Spot runtime이 소유 | 28 | |
| 17-stage-wrapper-on-spot-R5 | timer admission과 callback turn은 Framework Spot runtime이 소유 | 29 | |
| 17-stage-wrapper-on-spot-R6 | Actor queue와 Actor 업무 handler는 Framework Actor runtime이 소유 | 30 | |
| 17-stage-wrapper-on-spot-R7 | Actor join·leave와 lifecycle control은 Framework의 lifecycle 전용 queue가 소유 | 31 | |
| 17-stage-wrapper-on-spot-R8 | 입장 권한, stage state, membership 정책과 broadcast 내용은 Stage wrapper/application이 소유 | 32 | |
| 17-stage-wrapper-on-spot-R9 | domain key→global Spot ID 매핑 정책은 Stage wrapper와 application domain store가 소유 | 33 | |
| 17-stage-wrapper-on-spot-R10 | Stage wrapper는 transport RID, endpoint, internal queue, native timer handle, message storage reference를 public surface에 노출하지 않는다 | 35-36 | |
| 17-stage-wrapper-on-spot-R11 | Stage 상태를 읽거나 바꾸는 callback은 target Spot의 application turn에서 실행해야 한다 | 40 | |
| 17-stage-wrapper-on-spot-R12 | Spot turn 대상 callback 목록: Spot direct handler, Logical Multicast subscription handler, Spot timer callback, Actor join·leave·lifecycle control callback, Stage wrapper가 명시적으로 제출한 domain operation | 42-46 | |
| 17-stage-wrapper-on-spot-R13 | User Spot 실행 mode는 factory 등록 시점에 고정한다 | 48 | |
| 17-stage-wrapper-on-spot-R14 | SpotWide에서는 위 작업과 member Actor handler가 같은 Spot gate를 사용하여 callback 두 개가 동시에 Spot 상태를 변경하지 않는다 | 48-50 | |
| 17-stage-wrapper-on-spot-R15 | PerActor에서는 Spot direct·Logical Multicast·lifecycle control이 Spot lane에서 직렬화된다 | 52-53 | |
| 17-stage-wrapper-on-spot-R16 | PerActor에서 각 member Actor의 payload는 Actor별 lane에서 직렬화된다 | 53 | |
| 17-stage-wrapper-on-spot-R17 | PerActor에서 같은 timer의 callback은 timer별 lane에서 직렬화된다 | 53-54 | |
| 17-stage-wrapper-on-spot-R18 | PerActor에서 서로 다른 Actor lane, Spot lane, timer lane은 동시에 실행될 수 있어 application이 공유 Stage state 동기화를 책임진다 | 54-56 | |
| 17-stage-wrapper-on-spot-R19 | Callback의 비동기 대기 중 turn 유지/반납 의미는 05 비동기 실행 정책이 정하며 Wrapper가 별도 규칙으로 바꾸지 않는다 | 58-60 | |
| 17-stage-wrapper-on-spot-R20 | request reply continuation이 Spot 상태를 바꾸면 원래 Spot turn으로 다시 제출되어야 한다 | 62-63 | |
| 17-stage-wrapper-on-spot-R21 | transport 또는 completion thread에서 Spot 상태를 직접 변경하지 않는다 | 64 | |
| 17-stage-wrapper-on-spot-R22 | Actor가 Stage 역할 Spot에 join해도 Actor 업무 payload는 Actor queue로 직접 전달된다 | 68-69 | |
| 17-stage-wrapper-on-spot-R23 | Actor payload를 Spot callback으로 변환하거나 Spot application queue에 넣지 않는다 | 69 | |
| 17-stage-wrapper-on-spot-R24 | Actor handler는 Stage의 mutable state를 직접 참조하지 않는다 | 70 | |
| 17-stage-wrapper-on-spot-R25 | Actor가 Stage state를 바꾸려면 Stage Spot으로 명시적 send/request를 제출해야 한다 | 72 | |
| 17-stage-wrapper-on-spot-R26 | 해당 handler가 Spot turn에서 membership, score, world state, broadcast 결정을 수행한다 | 72-73 | |
| 17-stage-wrapper-on-spot-R27 | SpotWide에서는 Actor handler도 같은 Stage 공통 execution gate를 사용한다 | 74 | |
| 17-stage-wrapper-on-spot-R28 | PerActor에서는 Actor별 gate와 Spot lane이 독립 실행될 수 있으므로 Actor handler가 mutable Stage state를 직접 참조하지 않는 경계를 유지한다 | 75-76 | |
| 17-stage-wrapper-on-spot-R29 | Framework는 Actor의 join, leave, relocation, lifecycle notification을 업무 message와 분리된 전용 queue에서 처리한다 | 78-79 | |
| 17-stage-wrapper-on-spot-R30 | 이 lifecycle 전용 queue에서는 Actor의 일반 업무 handler를 실행하지 않고, 업무 payload를 lifecycle callback으로 바꾸지도 않는다 | 79-80 | |
| 17-stage-wrapper-on-spot-R31 | 자세한 Actor queue와 lifecycle 처리 계약은 22 Actor 모델(14-actor-model.ko.md)이 소유한다 | 81-82 | |
| 17-stage-wrapper-on-spot-R32 | Stage timer는 Spot lifecycle 안에서 등록하고 tick을 Spot application queue에 제출한다 | 86-87 | |
| 17-stage-wrapper-on-spot-R33 | SpotWide에서 timer callback은 Spot direct, member Actor, 다른 timer callback을 포함한 Stage 전체와 직렬화된다 | 87-88 | |
| 17-stage-wrapper-on-spot-R34 | PerActor에서는 같은 timer callback만 직렬화하고 다른 timer, Actor lane, Spot lane은 동시 실행 가능 | 88-89 | |
| 17-stage-wrapper-on-spot-R35 | Spot 종료가 신규 timer tick admission을 닫는다 | 91 | |
| 17-stage-wrapper-on-spot-R36 | 이미 수락한 tick과 종료 callback의 순서는 Spot lifecycle 규칙으로 정한다 | 92 | |
| 17-stage-wrapper-on-spot-R37 | fixed-rate, delay, catch-up, overrun option은 언어별 timer 공개 계약으로 표현한다 | 93 | |
| 17-stage-wrapper-on-spot-R38 | wrapper는 native handle이나 scheduler thread를 application에 노출하지 않는다 | 94 | |
| 17-stage-wrapper-on-spot-R39 | Yield는 SpotWide User Spot과 Instance Spot callback에서만 현재 Spot gate를 반납한다 | 96-97 | |
| 17-stage-wrapper-on-spot-R40 | SpotWide Stage에서는 Channel·Spot·Actor request 또는 CPU·I/O worker 결과를 기다릴 때 Yield 사용 가능 | 97-98 | |
| 17-stage-wrapper-on-spot-R41 | Continuation은 같은 execution gate에서 새 turn으로 재개한다 | 98-99 | |
| 17-stage-wrapper-on-spot-R42 | PerActor Stage와 Entry Spot callback에서는 Yield를 사용할 수 없다 | 99-100 | |
| 17-stage-wrapper-on-spot-R43 | Member Actor handler가 Yield할 때도 현재 Actor queue head를 실행할 권한은 유지한다 | 102-103 | |
| 17-stage-wrapper-on-spot-R44 | 다른 Actor, Spot handler, timer는 Stage 공통 gate를 사용할 수 있지만 같은 Actor의 다음 job은 continuation이 gate를 다시 얻어 현재 job을 완료할 때까지 실행하지 않는다 | 103-104 | |
| 17-stage-wrapper-on-spot-R45 | 같은 Actor 자신에게 보낸 request도 현재 queue head를 앞질러 실행하거나 inline으로 재진입하지 않는다 | 104-105 | |
| 17-stage-wrapper-on-spot-R46 | 같은 gate가 필요한 request를 Async로 기다리거나 자신에게 보낸 request를 기다리는 호출은 submit 전에 InvalidOperation으로 거부한다 | 107-108 | |
| 17-stage-wrapper-on-spot-R47 | Host Relocate가 시작되어도 실행 권한을 아직 얻지 못한 Stage Spot은 기존 message와 timer turn을 계속 처리한다 | 110-111 | |
| 17-stage-wrapper-on-spot-R48 | relocation 준비 상태를 알리는 내부 notification은 application event가 아니므로 Stage callback을 실행하지 않는다 | 111-113 | |
| 17-stage-wrapper-on-spot-R49 | 실행 권한을 얻어 새 turn 수락을 닫은 뒤에는 아직 실행하지 않은 timer tick과 timer 등록 정보를 relocation payload에 포함한다 | 115-116 | |
| 17-stage-wrapper-on-spot-R50 | Target Framework가 이를 자동 복원하므로 Stage wrapper의 Restore가 같은 timer를 다시 등록하지 않는다 | 116-117 | |
| 17-stage-wrapper-on-spot-R51 | Stage wrapper는 User Spot manager의 explicit Create·GetOrCreate에 stable type과 domain 생성 payload를 전달하고 생성 callback 안에서 초기 Stage state를 만든다 | 121-122 | |
| 17-stage-wrapper-on-spot-R52 | 여러 node가 같은 Spot을 동시에 만들려 해도 Framework는 생성 권한을 얻은 factory 하나만 실행한다 | 123-124 | |
| 17-stage-wrapper-on-spot-R53 | 새 작업을 허용하는 조건과 재활성 뒤 복원할 업무 상태는 domain 규칙으로 결정한다 | 124-125 | |
| 17-stage-wrapper-on-spot-R54 | Actor join은 Framework의 lifecycle 전용 queue에서 Stage membership 정책을 검사한다 | 127-128 | |
| 17-stage-wrapper-on-spot-R55 | Join이 성공하면 Actor의 현재 Spot 위치와 Stage가 소유한 member state를 함께 갱신한다 | 128-129 | |
| 17-stage-wrapper-on-spot-R56 | 동시 변경을 하나로 확정하는 방법과 relocation 중 message 수락 경계는 23 Spot Actor(15-spot-actor.ko.md)가 소유한다 | 129-130 | |
| 17-stage-wrapper-on-spot-R57 | 같은 ChannelName의 여러 Spot에 알릴 때는 Logical Multicast를 사용한다 | 134 | |
| 17-stage-wrapper-on-spot-R58 | Stage 하나의 member state 기준 알림은 Spot turn에서 대상 Actor/bound session을 골라 명시적 메시지로 제출한다 | 135-136 | |
| 17-stage-wrapper-on-spot-R59 | Logical Multicast를 Stage member 목록의 durable source로 사용하지 않는다 | 138 | |
| 17-stage-wrapper-on-spot-R60 | 외부 service는 domain key에서 global Spot ID를 얻어 Stage Spot에 메시지를 보낸다 | 142 | |
| 17-stage-wrapper-on-spot-R61 | Exact incarnation을 종료하거나 운영 정보로 표시할 때는 manager lookup이 반환한 SpotRef를 사용한다 | 142-143 | |
| 17-stage-wrapper-on-spot-R62 | Owner RID와 endpoint는 wrapper 상태에 저장하지 않는다 | 143-144 | |
| 17-stage-wrapper-on-spot-R63 | 위치 갱신과 stale route의 의미는 24 Spot 주소 메시징(16-spot-address-messaging.ko.md)이 정한다 | 144-145 | |
| 17-stage-wrapper-on-spot-R64 | Stage 종료는 신규 application admission과 신규 join을 닫고, 이미 수락한 Spot turn과 membership 정리를 drain deadline 안에서 완료한다 | 147-148 | |
| 17-stage-wrapper-on-spot-R65 | 종료 뒤의 timer, subscription과 direct 메시지는 Stage callback을 새로 만들지 않는다 | 148-149 | |
| 17-stage-wrapper-on-spot-R66 | Stage wrapper는 메시지 모델의 immutable metadata snapshot을 handler에 그대로 제공하고 transport frame이나 storage ownership을 해석하지 않는다 | 153-155 | |
| 17-stage-wrapper-on-spot-R67 | 관측 정보는 MeshName, Stage type, Spot turn backlog, timer 지연, membership control 결과, 종료 state를 구분해야 한다 | 157 | |
| 17-stage-wrapper-on-spot-R68 | Stage ID와 Actor ID는 metric label로 사용하지 않는다 | 158 | |
| 17-stage-wrapper-on-spot-R69 | (검증) Spot direct, Logical Multicast, timer, explicit Stage operation이 같은 Spot turn을 보존 | 162 | |
| 17-stage-wrapper-on-spot-R70 | (검증) SpotWide에서 member Actor handler도 같은 Spot gate 사용 | 163 | |
| 17-stage-wrapper-on-spot-R71 | (검증) PerActor에서 Spot lane, Actor별 lane, timer별 lane이 각자 FIFO 직렬성을 유지하며 서로 동시 실행 가능 | 164-165 | |
| 17-stage-wrapper-on-spot-R72 | (검증) SpotWide Actor의 Yield는 Spot gate만 반납하고 Actor queue head 실행 권한은 유지 | 166-167 | |
| 17-stage-wrapper-on-spot-R73 | (검증) 같은 gate 필요한 Async와 self-awaited request는 submit 전에 InvalidOperation 거부 | 168-169 | |
| 17-stage-wrapper-on-spot-R74 | (검증) Actor payload가 Stage Spot callback이나 Spot application queue를 거치지 않음 | 170 | |
| 17-stage-wrapper-on-spot-R75 | (검증) Actor handler가 Stage state를 바꿀 때 명시적 Spot 호출 사용 | 171 | |
| 17-stage-wrapper-on-spot-R76 | (검증) Spot lifecycle 전용 queue에는 join·leave와 lifecycle control만 포함, Actor 업무 payload 미포함 | 172-173 | |
| 17-stage-wrapper-on-spot-R77 | (검증) request continuation이 transport thread에서 Stage state를 직접 변경하지 않음 | 174 | |
| 17-stage-wrapper-on-spot-R78 | (검증) Stage wrapper가 Framework의 public Spot·Actor·timer·location 표면만 사용 | 175 | |
| 17-stage-wrapper-on-spot-R79 | (검증) Spot 종료 뒤 신규 timer와 message callback이 실행되지 않음 | 176 | |
| 17-stage-wrapper-on-spot-R80 | (검증) Relocation permit 전에는 Stage Spot을 seal하지 않고, seal 뒤 timer registration과 pending tick을 target에서 자동 복원 | 177-178 | |

### 18-object-routing

| # | 규칙 (수치·상태·오류) | 옛 위치 | 새 위치 |
|---|---|---|---|
| 18-object-routing-R1 | Spot·Actor direct: Ready cache 사용, cache miss일 때만 Store 조회 | 22, 32-34 | |
| 18-object-routing-R2 | Session→Actor: Stored binding route 사용, message마다 재조회하지 않음 | 23, 35-36 | |
| 18-object-routing-R3 | Request reply: Preserved reply route + correlation 사용, requester 위치 재조회 안 함 | 24, 37-38 | |
| 18-object-routing-R4 | 세 route source 중 direct resolution만 Location Store를 읽는다 | 26 | |
| 18-object-routing-R5 | 이 문서는 Node direct, Channel select-one, Logical Multicast의 target 선택을 다루지 않는다 | 41-42 | |
| 18-object-routing-R6 | Object create·get-or-create, exact ActorRef·SpotRef를 쓰는 close·destroy, membership transaction은 각 lifecycle 문서가 정의 | 42-43 | |
| 18-object-routing-R7 | Spot direct call은 global Spot ID, Actor direct call은 global Actor ID를 받는다 | 49-50 | |
| 18-object-routing-R8 | Source runtime 처리 순서: ①Ready route 조회 → ②없으면 Location Store 조회 → ③Ready면 MeshName/NodeRid/generation/owner fence를 route snapshot에 기록 → ④선택한 owner route로 제출 → ⑤target이 current owner/Ready object/local admission 확인 후 queue에 넣음 | 70-79 | |
| 18-object-routing-R9 | Object generation은 application handler의 target 일치 조건으로 검사하지 않는다 | 77-78 | |
| 18-object-routing-R10 | Location Store가 기록한 current owner, incarnation, owner generation, lease 정보를 authority라 한다 | 81-82 | |
| 18-object-routing-R11 | Framework는 current Ready authority만 application message의 route로 사용한다 | 82-83 | |
| 18-object-routing-R12 | Spot/Actor ID 문자열에는 owner 주소가 들어 있지 않고, Framework는 ID를 parse해 node를 추론하거나 Core routing ID로 변환하지 않는다 | 85-87 | |
| 18-object-routing-R13 | Caller는 MeshName, owner NodeRid, ActorRef/SpotRef, Actor의 current Spot ID를 message target으로 지정하지 않는다 | 87-93 | |
| 18-object-routing-R14 | Positive route cache는 Store의 current authority를 대체하는 별도 authority가 아니라 최근 조회 결과의 snapshot이다 | 97-98 | |
| 18-object-routing-R15 | Cache 보관 정보: global object ID, ObjectGeneration, AuthorityOwnerGeneration, StoreVersion, owner lease, node lifecycle, owner route | 102 | |
| 18-object-routing-R16 | 사용 기간: current owner lease의 local admission deadline과 RouteCacheMaxAge 중 먼저 끝나는 시점까지 | 103 | |
| 18-object-routing-R17 | RouteCacheMaxAge 기본값 15초, 0이면 route cache 미사용 | 104 | |
| 18-object-routing-R18 | Missing, Creating과 Store failure는 cache하지 않는다 | 105 | |
| 18-object-routing-R19 | 즉시 무효화 조건: 더 큰 StoreVersion, stale route 결과, Store recovery event, owner lease invalidation, relay 통지 | 106 | |
| 18-object-routing-R20 | Relay 통지: Message Follow relay가 새 owner로 넘기면 원 송신 runtime에 통지, 통지 받으면 entry 제거 후 다음 call에서 재조회 | 107 | |
| 18-object-routing-R21 | 실행 중 RouteCacheMaxAge 변경은 새 cache entry부터 적용, 기존 entry 수명은 연장 안 함 | 108 | |
| 18-object-routing-R22 | Relay 통지는 Framework가 소유하는 infrastructure record이며 application handler를 호출하지 않는다 | 110-111 | |
| 18-object-routing-R23 | 통지가 유실되어도 정확성은 바뀌지 않는다(cache 수명이 끝나면 같은 결과 도달) | 111-112 | |
| 18-object-routing-R24 | Local owner와 remote owner에는 같은 handler, metadata, completion 계약을 적용한다 | 119 | |
| 18-object-routing-R25 | Instance intent가 없는 Spot direct call과 Actor direct call은 이미 Ready인 object만 대상으로 한다 | 123-124 | |
| 18-object-routing-R26 | Missing Actor message는 Actor를 새로 만들지 않는다 | 126 | |
| 18-object-routing-R27 | Missing Spot message도 기본적으로 Spot을 새로 만들지 않는다 | 127 | |
| 18-object-routing-R28 | Spot 전용 fluent call에 Instance intent를 명시한 경우에만 Missing Instance Spot의 cold activation을 시작할 수 있다 | 128-129 | |
| 18-object-routing-R29 | Missing/Creating/Store failure를 cache 안 하므로 다음 call은 당시 current 상태를 다시 확인 | 131-132 | |
| 18-object-routing-R30 | 이전 owner는 commit된 source→target Message Follow route가 있을 때만 같은 operation을 current owner로 relay | 137-138 | |
| 18-object-routing-R31 | Relay 중에는 Location Store를 읽거나 application handler를 실행하지 않는다 | 138-139 | |
| 18-object-routing-R32 | Message Follow route는 global object ID, ObjectGeneration, source·target AuthorityOwnerGeneration, owner fence를 검증 | 141-142 | |
| 18-object-routing-R33 | Owner generation은 hop마다 증가해야 하며 chain은 최대 8 hops | 142-143 | |
| 18-object-routing-R34 | Route 하나의 queue에는 message 수·저장 크기 상한이 없으나 각 message의 negotiated message bound는 지킨다 | 143-144 | |
| 18-object-routing-R35 | MessageFollowDuration 기본값 30초, 0이면 Message Follow 미사용 | 146 | |
| 18-object-routing-R36 | RouteCacheMaxAge와 MessageFollowDuration이 모두 양수이면 cache max age가 MessageFollowDuration보다 최소 5초 짧아야 함 | 147-148 | |
| 18-object-routing-R37 | 실행 중 변경한 Message Follow duration은 새 relocation부터 적용 | 148-149 | |
| 18-object-routing-R38 | Relay는 original operation ID, ObjectGeneration, payload, reply route를 보존 | 151-152 | |
| 18-object-routing-R39 | Message Follow route가 없거나 만료되거나 loop 발생 시 Unavailable, generation mismatch는 InvalidOperation으로 끝난다 | 152-153 | |
| 18-object-routing-R40 | 이 generation 검사는 relocation이 설치한 Message Follow route가 같은 incarnation 이동에 속하는지 확인하는 것이지 일반 message target 제한 검사가 아니다 | 155-157 | |
| 18-object-routing-R41 | PerActor User Spot relocation 중 ToActor는 Spot authority가 아니라 Actor별 current owner route를 사용 | 159-160 | |
| 18-object-routing-R42 | Spot authority가 target으로 바뀌어도 아직 source에 남은 Actor는 source route를 유지 | 160-161 | |
| 18-object-routing-R43 | Actor owner CAS가 성공하면 이전 owner는 같은 Actor Message Follow route로 target에 relay | 161-162 | |
| 18-object-routing-R44 | Actor queue를 seal하기 전에 수락한 작업은 이전 queue와 accepted journal에 포함 | 164-165 | |
| 18-object-routing-R45 | Seal 뒤 source에 도착한 작업은 ingress hold에 보관 | 165-166 | |
| 18-object-routing-R46 | Target relocation temporary queue 사용 순서: ①Restore 전 temporary queue 등록(Join이면 OnActorJoin에서 이미 등록됨) ②ingress hold message를 identity·reply route 유지해 relay ③Restore 완료 후 owner CAS, source는 target dispatch 전환까지 relay 지속 ④이전 queue+journal을 먼저, temporary queue 작업을 그 뒤에 실제 Actor queue에 삽입 ⑤temporary queue 제거, 기존 Actor dispatch로 전환 | 168-177 | |
| 18-object-routing-R47 | 전환 전 target 도착 작업은 temporary queue에 보관, 전환 뒤 Message Follow와 target direct 작업은 기존 Actor queue가 실제 수락한 순서대로 실행 | 179-180 | |
| 18-object-routing-R48 | Actor 전송 도중 이전되어도 caller가 새 route 선택하거나 operation을 다시 만들 필요 없다 | 182-183 | |
| 18-object-routing-R49 | Request deadline, correlation, one-way operation identity, ActorId, ObjectGeneration을 relay 전후 유지 | 183-184 | |
| 18-object-routing-R50 | Framework는 실패한 현재 operation을 새 owner에게 자동으로 다시 제출하지 않는다 | 186-187 | |
| 18-object-routing-R51 | 다음 call만 cache 또는 Location Store에서 current owner를 다시 찾는다(중복 실행 방지) | 187-189 | |
| 18-object-routing-R52 | 일반 Actor·Spot message는 global logical ID만 target으로 사용한다 | 193 | |
| 18-object-routing-R53 | ActorRef·SpotRef와 그 안의 ObjectGeneration은 application message target이 아니다 | 195-196 | |
| 18-object-routing-R54 | ObjectGeneration은 같은 ID로 object를 제거 후 다시 만들었는지를 구분한다 | 198 | |
| 18-object-routing-R55 | Actor·Spot direct send/request: ObjectGeneration을 target 일치 조건에서 제외, 같은 owner에서 재생성됐다면 target queue 수락 시점의 current Ready object가 처리 | 203 | |
| 18-object-routing-R56 | Destroy·Close와 membership 변경: caller가 지정한 incarnation과 current authority가 같은지 확인, 이전 incarnation 작업은 새 object 상태를 바꾸지 않음 | 204 | |
| 18-object-routing-R57 | 생성 recovery: 같은 생성 attempt와 incarnation만 계속, 다른 generation의 factory/생성 결과 미사용 | 205 | |
| 18-object-routing-R58 | Relocation과 Message Follow: 같은 relocation에 속한 state·queue·relay route인지 확인, 이전 generation의 relocation control을 새 object에 미적용 | 206 | |
| 18-object-routing-R59 | Session bind와 relay: bind는 exact ActorRef로 시작, binding token 발급, Actor 제거 시 기존 binding 종료(새 incarnation엔 explicit bind 필요), 늦은 relay는 종료된 binding token으로 거부 | 207 | |
| 18-object-routing-R60 | 같은 owner에서 close·destroy 후 같은 ID로 새 incarnation이 만들어졌다면 target queue 수락 시점의 current Ready object가 처리(모든 Spot direct message에 동일 적용) | 213 | |
| 18-object-routing-R61 | Owner process 종료 또는 owner 변경으로 resolve한 route를 쓸 수 없으면 current operation을 Unavailable로 끝낸다 | 214 | |
| 18-object-routing-R62 | 두 경우 모두 Framework는 실패 operation을 새 owner에게 자동으로 다시 보내지 않는다 | 216 | |
| 18-object-routing-R63 | Application이 새 call을 시작하면 그때 logical ID의 current Ready owner를 다시 확인 | 217 | |
| 18-object-routing-R64 | Logical ID는 message 대상을, ObjectGeneration은 특정 incarnation 상태 변경 control을 제한한다 | 220-222 | |
| 18-object-routing-R65 | Session relay는 message마다 Actor ID를 resolve하지 않는다. Bind할 때 route를 한 번 검증하고 Session owner에 저장 | 228-229 | |
| 18-object-routing-R66 | Bind는 caller가 제출한 exact ActorRef의 위치를 최초 route로 사용 | 246-247 | |
| 18-object-routing-R67 | Source가 bind 전에 Location Store에서 current route를 미리 조회하거나 local Actor instance를 받는 overload는 제공하지 않는다 | 247-248 | |
| 18-object-routing-R68 | Actor owner는 ActorId·ObjectGeneration, target NodeGeneration, AuthorityOwnerGeneration, current owner lease, Session owner와 Session lifecycle identity를 확인하고 binding generation을 등록한 뒤 terminal reply 반환 | 250-257 | |
| 18-object-routing-R69 | Bind 성공 시 Session owner가 binding route에 저장하는 정보: ActorId/ObjectGeneration, MeshName/owner NodeRid, NodeGeneration/AuthorityOwnerGeneration/OwnerLeaseGeneration, Session owner RID·lifecycle generation·binding generation·token, Session sequence | 262-268 | |
| 18-object-routing-R70 | 다른 owner/Actor generation으로 rebind할 때 target Actor owner는 새 identity 등록 후 이전 exact owner에 tombstone 제출, 이전 owner ACK 받은 뒤에만 bind terminal reply 반환 | 270-272 | |
| 18-object-routing-R71 | Session owner는 terminal reply 전에는 기존 route 유지, reply 뒤에는 새 route로 atomic하게 교체 | 272-273 | |
| 18-object-routing-R72 | Session owner는 교체 뒤 별도 durable retry journal을 보관하거나 Location Store·Relocation Store에 binding route를 기록하지 않는다 | 273-275 | |
| 18-object-routing-R73 | 같은 owner의 atomic replacement에서는 이전 identity tombstone이 새 identity를 제거하지 않아야 한다 | 275-276 | |
| 18-object-routing-R74 | Bind 후 저장 route를 쓰는 작업: Session→Actor RelayAsync, physical/logical disconnect 통지, Actor→bound Session push | 282-284 | |
| 18-object-routing-R75 | 저장 route는 current owner lease와 local admission deadline 안에서만 유효, Store 일시 불가여도 lease/deadline을 연장하지 않는다 | 287-288 | |
| 18-object-routing-R76 | 저장 route가 무효화되면 active Message Follow route로 original operation을 정확히 한 번 전달하거나 Unavailable로 끝냄, 새 ActorRef를 찾아 자동 재전송하지 않음 | 290-292 | |
| 18-object-routing-R77 | Location Store와 Relocation Store는 binding route를 저장/갱신하지 않는다(Session owner runtime이 소유) | 294-295 | |
| 18-object-routing-R78 | Direct route cache 갱신이 Session binding route를 자동으로 바꾸지 않는다 | 295-296 | |
| 18-object-routing-R79 | Actor 이동해도 physical STREAM connection과 Session object는 Session owner process에 유지, socket/transport handle/Session callback state를 target process로 옮기거나 복제하지 않는다 | 300-302 | |
| 18-object-routing-R80 | Relocation 중 Session owner는 Location Store를 조회해 새 Actor route를 추측하지 않는다 | 304-305 | |
| 18-object-routing-R81 | Relocation 순서: ①source handler 종료+target preflight 성공 후 command42 sessionRelocationSeal request/command43 reply로 exact binding seal 설치, 새 dispatch 막고 queue·timer·state capture ②target이 lookup/factory 전에 temporary queue group 등록, Restore로 payload 복원 후 relay 수신 준비 reply ③capture 뒤 source 도착 message는 ingress hold, boundary 전 relay 구간 전달, source가 cutover를 one-way 전송 ④target이 cutover 수신 또는 relay 준비 reply 뒤 1,000ms 경과 시 target-only Location Store CAS로 commit ⑤CAS 뒤 saved work+relay+temporary work를 실제 Actor queue에 순서대로, dispatch는 닫아둠 ⑥lifecycle callback 완료(Join relocation이면 Join completion callback도), target dispatch 오픈 ⑦target이 command44 sessionRelocationRoute commit을 Session owner에 one-way 전송, Session owner가 exact Session·binding·Actor generation·relocation identity 확인 후 route/snapshot atomic 교체, held message를 target route로 제출, seal 해제, reply 없음 ⑧SessionRelocationSealTimeout 안에 command44 미도착 시 Session owner가 physical Session 종료, binding·held·seal state 정리 | 308-333 | |
| 18-object-routing-R82 | Route 갱신은 binding이 가리키는 ObjectGeneration과 같은 Actor relocation에만 허용, 새 incarnation 생성 시 기존 binding을 새 Actor로 바꾸지 않음(application이 새 ActorRef로 재bind 필요) | 335-337 | |
| 18-object-routing-R83 | 같은 Session에서 relocation 대상 아닌 다른 Actor의 route·snapshot·token·generation은 유지, physical STREAM connection도 유지 | 339 | |
| 18-object-routing-R84 | Command 44에는 적용 reply가 없고 request로 재전송하지 않음, Target Actor는 dispatch 오픈 뒤 처리, 이전 route 도착 message는 source Message Follow route가 전달, Application은 relocation을 알기 위해 rebind하지 않음 | 340-342 | |
| 18-object-routing-R85 | Relay-ready reply가 accepted 상태 되기 전 명시적 relocation failure에서는 Location Store 재확인 없이 target temporary queue 폐기, source Actor queue와 admission 복원 | 344-345 | |
| 18-object-routing-R86 | Bound Session seal 있으면 source coordinator가 command44 abort one-way로 held message를 source route에 제출, matching seal만 해제 | 346-348 | |
| 18-object-routing-R87 | Relay-ready 뒤에는 cutover submit 결과와 관계없이 source route/snapshot으로 rollback하지 않음, target process 종료 시 다른 runtime이 route 갱신을 자동으로 이어받지 않음 | 348-351 | |
| 18-object-routing-R88 | Source runtime은 request 제출 시 reply가 돌아올 내부 경로와 correlation을 함께 만든다 | 357-358 | |
| 18-object-routing-R89 | Target handler는 request가 가진 reply capability를 사용, reply를 위해 시작 Spot·Actor의 global ID를 cache나 Location Store에서 resolve하지 않는다 | 371-373 | |
| 18-object-routing-R90 | Reply correlation은 어떤 request를 완료할지 정하고, reply route는 원래 source runtime으로 돌아갈 경로를 정한다 | 375-377 | |
| 18-object-routing-R91 | Reply route와 correlation은 application metadata가 아니다. Request metadata를 reply에 자동 복사하지 않으며 일반 reply에는 metadata setter를 제공하지 않는다 | 379-381 | |
| 18-object-routing-R92 | Spot에서 request 시작 시 source runtime은 request correlation과 함께 request를 시작한 Spot 실행과 그 ObjectGeneration을 보존 | 385-389 | |
| 18-object-routing-R93 | Reply 도착 시 원래 request completion을 재개, 같은 Spot ID로 새 incarnation이 만들어져도 이전 reply를 새 Spot에 application message로 전달하지 않음 | 391-392 | |
| 18-object-routing-R94 | Spot·Actor operation이 Message Follow나 relocation payload를 거쳐도 original reply route와 correlation을 보존 | 394-395 | |
| 18-object-routing-R95 | Operation ID는 중복 작업 구분 값이며 reply route를 대신하지 않는다 | 395-396 | |
| 18-object-routing-R96 | Framework는 reply route를 복원할 수 있는 request의 handler·decode failure를 구조화된 error reply로 완료한다 | 400-401 | |
| 18-object-routing-R97 | reply route 복원 불가라고 해서 requester의 Spot·Actor ID나 새 owner를 Location Store에서 찾아 우회하지 않는다 | 401-403 | |
| 18-object-routing-R98 | 해당 failure는 03 상호작용 모델 §10 handler 실패가 정한 drop, log, metric 계약을 따른다 | 403-404 | |
| 18-object-routing-R99 | Route 오류, timeout, cancellation, 실행 여부 불명확한 failure 뒤에도 같은 request를 다른 owner에게 자동 재제출하지 않는다 | 406-407 | |
| 18-object-routing-R100 | Request는 reply, error, timeout, cancellation, shutdown 중 먼저 확정된 terminal 결과 하나로 완료 | 407-409 | |
| 18-object-routing-R101 | (검증) global ID만 받고 owner RID·generation·ActorRef/SpotRef 미요구 | 413-443 | |
| 18-object-routing-R102 | (검증) cache hit 시 Store 미조회, miss/invalidation 후 재조회 | 413-443 | |
| 18-object-routing-R103 | (검증) Missing/Creating/Store failure negative-cache 안 함 | 413-443 | |
| 18-object-routing-R104 | (검증) positive cache가 deadline/RouteCacheMaxAge 준수 및 즉시 제거 조건 | 413-443 | |
| 18-object-routing-R105 | (검증) target admission이 exact generation·lease fence 검증, retarget 안 함 | 413-443 | |
| 18-object-routing-R106 | (검증) Message Follow relay가 committed route만 사용, Store 미조회, ID/generation/payload/reply route 보존 | 413-443 | |
| 18-object-routing-R107 | (검증) PerActor ToSpot=Spot authority/ToActor=Actor별 owner, temporary queue 독립 등록·atomic 전환 | 413-443 | |
| 18-object-routing-R108 | (검증) failed operation 자동 재제출 안 함 | 413-443 | |
| 18-object-routing-R109 | (검증) bind가 exact ActorRef를 최초 route로, 검증된 route만 저장 | 413-443 | |
| 18-object-routing-R110 | (검증) session relay/disconnect/push가 stored binding route 사용, 매 message Store 미조회 | 413-443 | |
| 18-object-routing-R111 | (검증) Actor relocation이 같은 ObjectGeneration의 command44를 one-way로 적용, 해당 Actor binding route만 변경, 나머지 유지 | 413-443 | |
| 18-object-routing-R112 | (검증) command44 무응답·재전송 안 함, target 처리는 적용을 기다리지 않고 source Message Follow가 MessageFollowDuration 동안 전달; reply가 request의 reply route/correlation 사용, Location Store 미조회; application metadata가 owner/reply route 대체 안 함, 자동 복사 안 함 | 413-443 | |

### 45-internal-routing-and-cache

| # | 규칙 (수치·상태·오류) | 옛 위치 | 새 위치 |
|---|---|---|---|
| 45-internal-routing-and-cache-R1 | 모든 호출에서 message마다 Location Store를 왕복하면 비용이 크다(문제 정의) | 25-27 | |
| 45-internal-routing-and-cache-R2 | 최근에 확인한 owner 경로를 source runtime에 보관했다가 재사용 = Positive route cache | 31-32 | |
| 45-internal-routing-and-cache-R3 | 캐시에 보관하는 것은 owner 경로와 수락 판단에 필요한 fence 값이다. 경로만 캐시하고 fence를 빼면 낡은 owner로 보내고도 알지 못한다 | 34-35 | |
| 45-internal-routing-and-cache-R4 | 실패와 진행 중 상태(없음/만드는 중/저장소 실패)는 캐시하지 않는다 | 39-40 | |
| 45-internal-routing-and-cache-R5 | 이를 캐시하면 잠깐의 실패가 캐시 수명만큼 지속되는 장애가 된다 | 40-41 | |
| 45-internal-routing-and-cache-R6 | 공개 동작은 31 §4.4가 정의하며, Resolver는 ReadyRoute, Missing, Unavailable, StoreFailure의 닫힌 결과로 전달 | 45-47 | |
| 45-internal-routing-and-cache-R7 | ReadyRoute: route와 authority·owner lease fence 보존, route admission이 받음 | 51 | |
| 45-internal-routing-and-cache-R8 | Missing: authority record 없음 보존, creation coordinator가 받음 | 52 | |
| 45-internal-routing-and-cache-R9 | Unavailable: authority는 남아있지만 current owner 사용 불가, terminal completion mapper가 받음 | 53 | |
| 45-internal-routing-and-cache-R10 | StoreFailure: authority 유무 판정 불가, Store retry/reconciliation이 받음 | 54 | |
| 45-internal-routing-and-cache-R11 | 네 결과를 null이나 하나의 '없음'으로 축약하지 않는다 | 56 | |
| 45-internal-routing-and-cache-R12 | Positive route cache에는 ReadyRoute만 저장, activation coordinator에는 Missing만 전달 | 56-57 | |
| 45-internal-routing-and-cache-R13 | Authority release를 소유하는 lifecycle component가 완료한 뒤에만 resolver가 Missing을 반환 | 57-58 | |
| 45-internal-routing-and-cache-R14 | 캐시 수명 상한 = 세 값 중 가장 짧은 것: RouteCacheMaxAge(캐시 최대 보관 시간), owner의 수락 기한, Message Follow 기간보다 최소 5초 짧게 | 62-68 | |
| 45-internal-routing-and-cache-R15 | Message Follow 기간보다 5초 짧아야 하는 이유: 우회 경로가 닫히기 전에 캐시가 먼저 만료되어야 함 | 68 | |
| 45-internal-routing-and-cache-R16 | Location Store 반환 object peer descriptor에는 endpoint, RID, lifecycle generation, security identity가 들어 있다 | 72-73 | |
| 45-internal-routing-and-cache-R17 | 수동 endpoint 설정은 연결 의도만 제공, runtime이 descriptor와 연결해 object peer로 쓸 때 handshake 필요값을 transport에 모두 전달해야 함 | 73-75 | |
| 45-internal-routing-and-cache-R18 | JVM 경로: MeshNode 시작 시 manual endpoint-only intent 먼저 등록, 이후 connectManualObjectPeers/MeshNodeExecutor/ensureManualObjectPeer가 descriptor를 찾으면 replacePeerConnection(endpoint, rid, lifecycleGeneration, securityIdentity) 호출 | 78-82 | |
| 45-internal-routing-and-cache-R19 | 교체 경로는 이전 intent의 transport liveness close 확인 뒤에만 새 intent 설치 | 84-85 | |
| 45-internal-routing-and-cache-R20 | ZLinkJavaRawMeshNode는 intent, 실제 peer routing ID, close 상태를 함께 보관해 admission fence와 liveness event를 처리 | 85-86 | |
| 45-internal-routing-and-cache-R21 | descriptor를 찾지 못한 endpoint-only intent는 placement 근거로 사용하지 않는다 | 86-87 | |
| 45-internal-routing-and-cache-R22 | Caller가 generation이나 security identity를 직접 설정해 이 절차를 우회할 수 없다 | 87-88 | |
| 45-internal-routing-and-cache-R23 | 옛 owner로 간 message는 버려지지 않는다. Message Follow가 새 owner에게 넘긴다 | 95-96 | |
| 45-internal-routing-and-cache-R24 | 넘기는 동안 모든 message가 한 홉을 더 거친다 | 97 | |
| 45-internal-routing-and-cache-R25 | 캐시를 무효화하지 않으면 이동 후 Message Follow 기간(기본 30초) 내내 그 객체로 가는 모든 트래픽이 우회 경로로 흐른다 | 99-100 | |
| 45-internal-routing-and-cache-R26 | 최대 8홉까지 이어질 수 있으므로 이동이 잦은 환경에서는 홉이 쌓인다 | 100-101 | |
| 45-internal-routing-and-cache-R27 | 우회로 넘어간 사실을 보낸 쪽에 알려 캐시를 갱신한다. 정식 spec(18)이 relay 통지를 캐시 무효화 조건에 포함했다 | 114-116 | |
| 45-internal-routing-and-cache-R28 | 통지를 받은 runtime은 해당 캐시 항목을 지우고 다음 호출에서 owner를 다시 조회한다 | 116-117 | |
| 45-internal-routing-and-cache-R29 | 우회는 캐시가 갱신될 때까지의 과도기를 메우는 장치이지 정상 경로가 아니다 | 119 | |
| 45-internal-routing-and-cache-R30 | 알림이 없으면 캐시 수명이 끝날 때까지 우회가 계속된다 | 120 | |
| 45-internal-routing-and-cache-R31 | 통지 record의 공통 wire 형식은 schema가 정한다: service-wire-v1.schema.json의 command 50 messageFollow에 source/target route fence, hop count, relay 시점 queue 회계, 원래 operation ID, reply route가 들어간다 | 122-123 | |
| 45-internal-routing-and-cache-R32 | Flags와 application payload는 허용하지 않는다 | 124 | |
| 45-internal-routing-and-cache-R33 | 각 runtime은 record를 relay·수신한 뒤 source route의 object generation, authority generation, target node를 검증해야 한다 | 126-127 | |
| 45-internal-routing-and-cache-R34 | 현재 cache 항목이 이 값과 일치할 때만 무효화하여 이미 저장된 더 새로운 route를 지우지 않는다 | 127-128 | |
| 45-internal-routing-and-cache-R35 | 중복 억제는 전용 registry가 맡는다. Key는 source·target route fence의 모든 field(object kind, 논리 ID, object generation, target node RID·generation, authority owner generation, owner lease generation) 포함, 양쪽 값 비교 | 130-133 | |
| 45-internal-routing-and-cache-R36 | 일부 generation만 key로 쓰면 이전 route에서 남은 표식이 새 target으로 보내야 할 통지까지 막을 수 있다 | 133-134 | |
| 45-internal-routing-and-cache-R37 | Registry 상태 전이: idle→(통지 전송 권한 획득)→inFlight→(전송 성공)→sentUntilExpiry, inFlight→(전송 실패)→idle, sentUntilExpiry/idle→(route cache 만료/교체)→종료 | 137-144 | |
| 45-internal-routing-and-cache-R38 | inFlight에서는 같은 key의 추가 전송을 시작하지 않는다 | 146 | |
| 45-internal-routing-and-cache-R39 | 전송 성공하면 cache route 만료까지 sentUntilExpiry 유지, 실패하면 idle로 전이해 재시도 가능 | 147 | |
| 45-internal-routing-and-cache-R40 | Registry는 자체 expiry timer를 만들지 않고 route cache 만료·교체 시점에 같은 key를 제거 | 148-149 | |
| 45-internal-routing-and-cache-R41 | 이 registry는 통지 중복만 관리, 원래 operation의 payload·reply route·terminal completion은 각 기존 owner가 계속 관리 | 151-152 | |
| 45-internal-routing-and-cache-R42 | suppression 상태가 원래 operation의 terminal 결과를 만들거나 바꾸지 않는다 | 152-153 | |
| 45-internal-routing-and-cache-R43 | 후보 제외 조건: weight가 0인 대상, 종료 준비 중인 대상 | 157-158 | |
| 45-internal-routing-and-cache-R44 | 후보 목록은 변경 시점에 만들어 두고 호출은 읽기만 한다. peer 상태가 바뀔 때 새 목록을 만들어 바꿔 끼우고, 호출 경로에서는 필터링·정렬을 하지 않는다 | 160-161 | |
| 45-internal-routing-and-cache-R45 | 호출마다 전체 peer를 훑으면 peer 수에 비례하는 비용이 모든 호출에 발생하지만 peer 상태는 message 빈도보다 훨씬 드물게 바뀐다 | 163-164 | |
| 45-internal-routing-and-cache-R46 | MeshNode(RouteMesh) 채널: 논리 node를 고른 뒤 NodeRid로 직접 지정, framework가 고름 | 172 | |
| 45-internal-routing-and-cache-R47 | ClientServer 채널: 후보 server 중 하나를 골라 그 server 전용 연결로 제출, framework가 고름 | 173 | |
| 45-internal-routing-and-cache-R48 | 수동 연결 fallback: 후보 endpoint를 socket 하나에 모두 알리고 대상 지정 없이 제출, Core가 고름 | 174 | |
| 45-internal-routing-and-cache-R49 | 정식 경로(MeshNode, ClientServer) 둘은 모두 framework가 고른다 | 176-177 | |
| 45-internal-routing-and-cache-R50 | 세 번째(수동 연결 fallback)는 ClientServer transport가 등록되지 않은 채널에서만 쓰는 fallback, Core의 load balancer가 고름, framework는 선택에 관여하지 않는다 | 180-182 | |
| 45-internal-routing-and-cache-R51 | framework는 socket 하나에 속한 connection 집합을 Core 대신 관리하지 않는다. Framework는 후보 endpoint와 weight만 Core에 전달 | 186-188 | |
| 45-internal-routing-and-cache-R52 | Core는 언제 연결할지, 재연결할지, 어느 connection으로 보낼지를 결정한다 | 188-189 | |
| 45-internal-routing-and-cache-R53 | 경계를 넘으면 3가지 중복: 대상 선택 시 연결 수명·재연결 backoff·HWM·operation completion도 함께 떠안음; 후보마다 socket 하나씩이면 socket·fd·monitor 자원이 후보 수에 비례해 증가; 연결 순서로 선택을 유도하면 Core는 연결 순서를 보장하지 않음 | 193-197 | |
| 45-internal-routing-and-cache-R54 | 연결 순서로 Core의 선택을 유도하려 하면 안 된다 — winner를 앞에 오도록 회전해도 받는 쪽이 집합에 넣으면 순서가 사라지고 선택 결과가 적용되지 않는다 | 199-201 | |
| 45-internal-routing-and-cache-R55 | 후보마다 socket을 하나씩 만들어 framework가 고르는 구조는 겉보기엔 성립하지만 연결 수명과 재연결을 framework가 떠안는 대가가 있다 | 203-204 | |
| 45-internal-routing-and-cache-R56 | 판정 기준: 하위 계층이 선택 시점에 eligibility 조건·weight·안정적 식별자를 모두 알고 강제할 수 있는가 | 210-211 | |
| 45-internal-routing-and-cache-R57 | ClientServer가 그렇다(대체 가능하지만 선택 필요 조건이 framework에만 있음) | 213-214 | |
| 45-internal-routing-and-cache-R58 | 필요 조건들의 소재: ready·drain 상태(framework 연결 승인 기록), descriptor-연결 identity·세대 일치(framework 검증), 수동 연결 ChannelName·RID·세대·weight·drain·보안 검증(framework 검증), Server RID tiebreak(framework가 아는 값) | 216-221 | |
| 45-internal-routing-and-cache-R59 | 조건들을 하위 계층에 투영하는 경로가 없으면 socket 하나로 합쳤을 때 아직 승인 안 됐거나 drain 중인 연결이 선택될 수 있다 | 223-224 | |
| 45-internal-routing-and-cache-R60 | 지금은 per-server 연결과 framework 선택이 맞다 | 224-225 | |
| 45-internal-routing-and-cache-R61 | Connection을 socket 하나로 합치려면 framework 선택 정보를 하위 계층에 전달하는 projection API가 먼저 필요하다 | 227-230 | |
| 45-internal-routing-and-cache-R62 | 이 API가 없으면 per-server connection과 framework selector를 유지한다 | 230 | |
| 45-internal-routing-and-cache-R63 | Core가 고르는 경로에서 framework가 계약을 만족시키려면 Core가 그 순서를 내야 한다 — framework 안에서는 닫을 수 없다 | 234-235 | |
| 45-internal-routing-and-cache-R64 | Core의 load balancer가 §5 절차를 내지 않는 동안 이 경로의 선택 순서는 계약을 만족하지 않는다 | 235-236 | |
| 45-internal-routing-and-cache-R65 | 가중치를 매끄럽게 분산하는 순환(smooth weighted round-robin)을 쓴다 — 정식 spec(08)이 계약으로 고정 | 240-243 | |
| 45-internal-routing-and-cache-R66 | 정식 spec 요구 2가지: 장기 선택 비율이 weight 비율에 수렴, ClientServer 경로에서 같은 weight 대상끼리 순환 | 246-249 | |
| 45-internal-routing-and-cache-R67 | 이 둘을 만족하는 알고리즘은 여럿이고 서로 다른 순서를 낼 수 있어 알고리즘 자체를 고정한다(재현성/언어 간 비교 위해) | 251-253 | |
| 45-internal-routing-and-cache-R68 | 절차: 후보마다 고정 weight와 가변 current(초기 0)를 둠. 매 선택마다: ①모든 후보 current에 weight를 더함 ②current 최대인 후보를 고름(동점이면 후보 식별자가 작은 쪽) ③고른 후보 current에서 전체 weight 합을 뺌 | 257-262 | |
| 45-internal-routing-and-cache-R69 | current 값은 channel의 선택기가 계속 들고 있고, 후보 목록 변경 시 새 목록의 후보만 남기고 나머지는 버림 | 264-265 | |
| 45-internal-routing-and-cache-R70 | 예시: weight 100·300인 A·B에 네 번 연속 요청하면 B, A, B, B (A의 node RID가 B보다 작을 때, 1회차 동점) | 269-271 | |
| 45-internal-routing-and-cache-R71 | 같은 weight 두 후보는 A, B, A, B로 번갈아 나온다 | 274-275 | |
| 45-internal-routing-and-cache-R72 | 가중 무작위는 장기 비율은 맞지만 순환을 보장하지 않는다(같은 weight 두 대상 연속 10회에서 한쪽이 8회 가능) — spec 요구 미충족, 재현 안 됨 | 279-281 | |
| 45-internal-routing-and-cache-R73 | 후보 순서는 topology별 식별자로 정렬: RouteMesh는 node RID, ClientServer는 Server RID | 283-289 | |
| 45-internal-routing-and-cache-R74 | 연결 경로, 등록 출처, 연결 map key를 후보 식별자로 쓰면 연결 순서에 따라 tiebreak 결과가 달라진다(쓰면 안 됨) | 291-292 | |
| 45-internal-routing-and-cache-R75 | 절차를 호출마다 수행하면 후보 수 N에 비례하는 비용이 모든 send에 발생 | 296-297 | |
| 45-internal-routing-and-cache-R76 | 후보 목록이 바뀔 때 순서를 미리 계산해 두고 호출은 cursor만 옮긴다 | 299-300 | |
| 45-internal-routing-and-cache-R77 | 절차가 결정적이므로 같은 누적값 상태가 다시 나타나면 그 사이가 주기. 도입부와 주기를 나눠 저장, 호출은 배열 읽고 cursor 증가만 | 302-306 | |
| 45-internal-routing-and-cache-R78 | 결과 순서는 절차를 매번 수행한 것과 완전히 같다 | 306 | |
| 45-internal-routing-and-cache-R79 | 단정 금지 1: weight 합÷최대공약수 길이는 누적값이 전부 0인 상태에서만 주기, 후보가 바뀌면 남은 후보 누적값이 보존되므로 0에서 시작한 주기라는 보장이 없다 | 310-312 | |
| 45-internal-routing-and-cache-R80 | 단정 금지 2: 시작 상태로 돌아오기를 기다리면 안 된다 — 도입부를 지나 주기에 들어가면 시작 상태는 다시 나타나지 않는다 | 314-315 | |
| 45-internal-routing-and-cache-R81 | 예시(A=-2,B=1,C=1에서 B 제외): 주기는 (-1,0)→(0,-1) 두 걸음, (-2,1)은 도입부. 시작 상태 복귀를 기다리면 영영 못 찾음 | 317-328 | |
| 45-internal-routing-and-cache-R82 | 주기 탐색에는 걸음 수와 시간 두 상한을 둔다. 상한 안에 못 찾으면 호출마다 수행하는 방식으로 되돌린다. 탐색은 후보 변경 경로에서 하며 send 경로에서 하지 않는다 | 330-332 | |
| 45-internal-routing-and-cache-R83 | 누적값 상태와 cursor 증가는 하나의 순서로 정렬한다. 후보 교체와 선택이 동시 발생하면 기준이 불명확 | 334-336 | |
| 45-internal-routing-and-cache-R84 | 단일 cursor를 여러 스레드가 증가시키면 동기화 비용이 send 경로에 남으므로 channel별 선택 경로를 하나로 두거나 shard별 독립 상태를 쓴다 | 336-338 | |
| 45-internal-routing-and-cache-R85 | shard별 독립 상태를 고르면 결과 순서가 shard마다 달라져 계약 미충족 — 그래서 channel별 단일 경로를 택한다 | 338 | |
| 45-internal-routing-and-cache-R86 | 후보 배열·정렬·집합 생성은 호출 경로에 두지 않는다. §3 후보 목록과 주기를 함께 준비해 두고 호출은 읽기만 한다 | 340-341 | |
| 45-internal-routing-and-cache-R87 | 이름만 지정: 후보를 만들고 하나를 고름 / node RID나 객체 ID 직접 지정: 다른 대상을 대신 고르지 않음 | 348-349 | |
| 45-internal-routing-and-cache-R88 | "대상을 바꾸지 않는다"와 "호출이 성공한다"는 다른 보장. 직접 지정한 대상이 준비 안 됐으면 호출은 실패로 끝나며 runtime이 다른 후보로 옮기지 않는다 | 351-352 | |
| 45-internal-routing-and-cache-R89 | 발행을 시작할 때 대상 목록을 고정한다. 보내는 도중 구독자가 늘거나 줄어도 이번 발행의 대상은 바뀌지 않는다 | 359-361 | |
| 45-internal-routing-and-cache-R90 | 원격 node에는 message를 하나만 보내고 그 node가 자기 구독자에게 나눠 준다 | 363 | |
| 45-internal-routing-and-cache-R91 | 구독자마다 따로 보내면 같은 payload가 네트워크를 여러 번 건넌다(node당 구독자 100개면 100배); 나눠 주면 전송량이 구독자 수가 아닌 node 수에 비례 | 385-387 | |
| 45-internal-routing-and-cache-R92 | 같은 node 안의 구독자에게는 각자의 대기열에 직접 넣는다 | 389 | |
| 45-internal-routing-and-cache-R93 | 일부 대상이 실패해도 이미 수락한 대상을 되돌리지 않는다 — 되돌리려면 이미 실행됐을 handler를 취소해야 하는데 방법이 없다 | 391-393 | |
| 45-internal-routing-and-cache-R94 | 발행은 결과값 없이 완료하며 대상별 결과를 돌려주지 않는다. 수락하지 못한 대상을 public 결과나 monitoring으로 집계하지 않는다 | 395-397 | |
| 45-internal-routing-and-cache-R95 | 완료 시점은 보내는 쪽이 자기 자리를 확보한 때다 | 397 | |
| 45-internal-routing-and-cache-R96 | 발행은 "보냈다"까지만 보장하는 호출 — 대상별 도달 확인이 필요하면 발행이 아니라 응답을 기다리는 호출을 쓴다 | 400-402 | |
| 45-internal-routing-and-cache-R97 | (§8확인) Location Store 조회가 호출마다 발생 안 함 | 406-422 | |
| 45-internal-routing-and-cache-R98 | (§8확인) 없음/만드는 중/저장소 실패가 캐시에 안 남음 | 406-422 | |
| 45-internal-routing-and-cache-R99 | (§8확인) Resolver Missing/Unavailable이 서로 다른 tag | 406-422 | |
| 45-internal-routing-and-cache-R100 | (§8확인) activation coordinator엔 Missing만, Unavailable은 terminal mapper로 | 406-422 | |
| 45-internal-routing-and-cache-R101 | (§8확인) 캐시 수명이 Message Follow 기간을 안 넘음 | 406-422 | |
| 45-internal-routing-and-cache-R102 | (§8확인) 유효한 messageFollow 수신 시 즉시 캐시 무효화(유실 시 기존 lifetime 종료 후 재조회) | 406-422 | |
| 45-internal-routing-and-cache-R103 | (§8확인) peer 상태 불변 동안 후보 필터링 미실행 | 406-422 | |
| 45-internal-routing-and-cache-R104 | (§8확인) 같은 weight 대상이 번갈아 선택 | 406-422 | |
| 45-internal-routing-and-cache-R105 | (§8확인) weight 100/300 4연속 호출 시 B,A,B,B | 406-422 | |
| 45-internal-routing-and-cache-R106 | (§8확인) 같은 후보/선택기 상태에서 항상 같은 순서 | 406-422 | |
| 45-internal-routing-and-cache-R107 | (§8확인) weight 100/300 장기 비율 약 1:3 | 406-422 | |
| 45-internal-routing-and-cache-R108 | (§8확인) 직접 지정 호출에서 runtime이 다른 대상 미선택 | 406-422 | |
| 45-internal-routing-and-cache-R109 | (§8확인) 발행 도중 구독자 변경돼도 대상 목록 불변 | 406-422 | |
| 45-internal-routing-and-cache-R110 | (§8확인) 한 원격 node에 구독자 여럿이어도 wire record 하나 | 406-422 | |
| 45-internal-routing-and-cache-R111 | (§8확인) 일부 대상 실패가 이미 수락한 대상을 되돌리지 않음 | 406-422 | |
| 45-internal-routing-and-cache-R112 | (§8확인) 발행 결과에 target별 수락·실패 미노출(관측 지표로도) | 406-422 | |

### 47-internal-object-lifecycle

| # | 규칙 (수치·상태·오류) | 옛 위치 | 새 위치 |
|---|---|---|---|
| 47-internal-object-lifecycle-R1 | Spot 종류는 닫힌 값 집합: Invalid=0, Entry=1, User=2, Instance=3 | 24 | |
| 47-internal-object-lifecycle-R2 | Entry Spot: Object Server 시작 시 그 node에 하나 생성, 이동하지 않는다, 반납 대기 쓸 수 없다 | 29 | |
| 47-internal-object-lifecycle-R3 | User Spot: 만들기를 명시적으로 요청할 때 생성, 이동한다, SpotWide에서만 반납 대기 가능 | 30 | |
| 47-internal-object-lifecycle-R4 | Instance Spot: 만들겠다는 의사를 명시한 호출이 처음 도착할 때 생성, 이동한다, 반납 대기 쓸 수 있다 | 31 | |
| 47-internal-object-lifecycle-R5 | 세 종류를 서로 다른 타입으로 표현한다 — 한 타입에 표시를 붙이면 두 조합이 동시 참일 수 있고 규칙이 조건문으로 흩어진다 | 33-36 | |
| 47-internal-object-lifecycle-R6 | 언어별 재량: 상속·합성·태그 유니온 중 자유, 관찰 기준은 "불가능한 조합을 만들 수 있는가" | 40-41 | |
| 47-internal-object-lifecycle-R7 | Entry Spot 인스턴스는 이동 대상 목록에 들어가지 않는다(Object Server lifecycle에 속함) | 45-46 | |
| 47-internal-object-lifecycle-R8 | Entry Spot에 있던 Actor는 이동한다(이동하지 않는 것은 Entry Spot 자신뿐) | 48-49 | |
| 47-internal-object-lifecycle-R9 | 이동 대상을 고를 때 "Entry Spot에 속한 Actor"를 통째로 제외하면 그 Actor들은 node가 내려갈 때 사라진다 | 49-50 | |
| 47-internal-object-lifecycle-R10 | 일반 message는 없는 객체를 만들지 않는다. 만들겠다는 의사를 명시한 Spot 전용 호출만 새로 만들 수 있다 | 54-55 | |
| 47-internal-object-lifecycle-R11 | 내부 상태 4종: Missing(authority 없다는 조회 version, creation coordinator), Creating(attempt·reservation fence, 같은 attempt의 waiter), Ready(route와 authority·owner lease fence, route admission), Unavailable(authority와 무효 owner evidence, terminal completion adapter) | 65-70 | |
| 47-internal-object-lifecycle-R12 | Unavailable은 authority가 남아있지만 current owner 사용 불가라는 뜻, Missing과 같은 상태로 취급하지 않는다 | 72-73 | |
| 47-internal-object-lifecycle-R13 | Explicit Close, IdleEvicted cleanup, 다른 정식 lifecycle operation이 authority release를 완료한 뒤에만 resolver가 새 Missing 입력을 만들 수 있다 | 73-75 | |
| 47-internal-object-lifecycle-R14 | Stored creation intent는 같은 target node·lifecycle에서 끝나지 않은 최초 cold activation operation만 재개, steady Ready owner 장애의 takeover나 queue recovery엔 미사용 | 77-79 | |
| 47-internal-object-lifecycle-R15 | 이 구분이 없으면 오타 하나가 객체를 만들고 아무도 정리하지 않는다 | 80-81 | |
| 47-internal-object-lifecycle-R16 | 여러 caller가 동시에 만들려 하면 만들 권한을 먼저 확보한 쪽만 owner로 기록하고 만든다. 나머지는 만들어진 객체를 대상으로 삼는다. factory는 한 번만 실행 | 85-87 | |
| 47-internal-object-lifecycle-R17 | "만드는 중" 상태를 캐시하지 않는다 — 넣으면 만들기가 끝난 뒤에도 캐시 수명만큼 "만드는 중"으로 보인다(45의 캐시에 미포함) | 89-91 | |
| 47-internal-object-lifecycle-R18 | 생성 시퀀스: A·B가 동시에 만들 권한 시도 → Store가 A에 확보, B에 "이미 잡음" 통보 → A가 factory 실행(B는 캐시 없이 대기) → factory가 객체 반환 → A가 자신을 owner로 기록·Ready → B는 Ready 객체를 받음. factory는 한 번만 실행, 진 쪽은 만들어진 객체를 대상으로 삼음 | 93-110 | |
| 47-internal-object-lifecycle-R19 | Instance Spot은 Ready를 기록한 뒤에도 target runtime의 local view(instance intent projection)에 즉시 반영해야 한다 | 114-117 | |
| 47-internal-object-lifecycle-R20 | Projection은 Store를 대신하는 authority가 아니라 이미 검증된 Ready route를 process 안에서 조회하기 위한 값 | 117 | |
| 47-internal-object-lifecycle-R21 | 공개 순서: ①target node가 Ready authority를 Location Store에 commit ②commit 성공한 같은 동기 continuation에서 target runtime이 instance intent 등록 ③그 뒤 activation continuation이 첫 application message를 대기열에 넣음 | 121-124 | |
| 47-internal-object-lifecycle-R22 | 2번이 늦어지면 Store엔 Ready가 있지만 target runtime엔 route가 없어 첫 message가 NotFound 또는 stale route 오류로 끝날 수 있다 | 126-127 | |
| 47-internal-object-lifecycle-R23 | 다음 continuation에서 같은 route를 다시 등록해 누락을 복구할 수 있으나 이 등록도 첫 admission 전에 끝나야 한다. 같은 route를 여러 번 등록해도 중복 실행이 생기지 않도록 멱등적으로 처리 | 128-130 | |
| 47-internal-object-lifecycle-R24 | 진 쪽이 "만드는 중"을 캐시하면 마지막 두 단계가 캐시 수명만큼 늦어진다 | 132 | |
| 47-internal-object-lifecycle-R25 | 만들기가 중간에 실패하면 activation state machine이 남은 기록의 정리 주체와 시점을 정해야 한다 — 없으면 실패한 만들기가 그 ID를 영구히 점유 | 136-137 | |
| 47-internal-object-lifecycle-R26 | owner 정보는 캐시되므로 보내는 쪽이 아는 owner가 이미 바뀌었을 수 있어 받는 쪽이 걸러내야 한다 | 141-142 | |
| 47-internal-object-lifecycle-R27 | 걸러내는 기준은 owner 신원과 유효 기간이다. 객체 세대가 아니다 | 144 | |
| 47-internal-object-lifecycle-R28 | ObjectGeneration은 일반 message의 대상 조건이 아니다 — 검사하면 재생성 직후 정상 message가 전부 거절된다. 객체 세대는 lifecycle 변경과 이동 중계를 걸러낼 때만 사용 | 146-149 | |
| 47-internal-object-lifecycle-R29 | 검사표: owner 신원(더 이상 owner 아닌 경우), owner 유효 기간(기한 지난 경우), 객체 세대(lifecycle 변경·이동 중계만) | 151-155 | |
| 47-internal-object-lifecycle-R30 | 걸러낸 message는 낡은 경로 오류로 끝낸다. runtime이 자동으로 다시 시도하지 않는다 — 다시 시도하면 보내는 쪽은 성공으로 보지만 실제로는 두 번 실행됐을 수 있다 | 172-174 | |
| 47-internal-object-lifecycle-R31 | Application이 새 호출을 시작할 수는 있으며 그때 중복 실행 위험은 application이 판단한다 | 174-175 | |
| 47-internal-object-lifecycle-R32 | 유휴 정리 상태는 runtime 내부 object catalog가 소유. .NET mapping에서 이름은 ZLinkSpotNodeCatalog | 181-182 | |
| 47-internal-object-lifecycle-R33 | InstanceSpotIdleTimeout이 양수이면 catalog가 주기적으로 후보를 검사, 한 번에 최대 64개만 확인, 마지막 검사 위치를 다음 주기에 이어서 사용(Spot 수 많아도 maintenance가 dispatch를 독점하지 않음) | 182-184 | |
| 47-internal-object-lifecycle-R34 | 후보는 Instance Spot으로 한정: Actor membership 없음, relocation·Message Follow 미참여, application 작업 미대기 중인 activation만 후보 | 186-188 | |
| 47-internal-object-lifecycle-R35 | 마지막 application 작업이 끝난 시각부터 timeout이 지나야 후보로 인정 | 188 | |
| 47-internal-object-lifecycle-R36 | 정리 transaction 시작 시 catalog는 같은 Spot ID의 다른 close 요청과 합침, serial quiescence 재확인 후 IdleEvicted 사유로 closing callback 호출, activation dispose, Location Store의 Spot location release | 190-193 | |
| 47-internal-object-lifecycle-R37 | callback 실행 중 새 작업을 받지 않으며 callback이 끝나기 전에 location을 지우지 않는다 | 193 | |
| 47-internal-object-lifecycle-R38 | location row가 release 중일 때 instance intent request가 이전 route를 사용하면 runtime은 route를 무효화하고 close transaction이 Missing 되거나 current Ready route가 확인될 때까지 재조회 가능 | 195-197 | |
| 47-internal-object-lifecycle-R39 | 이 동작은 이미 수락된 application request를 재전송하는 retry가 아니라 explicit idle cleanup 결과를 확인하는 owner route 갱신이다 | 197-198 | |
| 47-internal-object-lifecycle-R40 | Resolver는 idle cleanup 완료 결과와 owner availability evidence만 바뀐 결과를 서로 다른 tag로 activation state machine에 전달, creation coordinator는 전자만, terminal completion adapter는 후자 | 200-202 | |
| 47-internal-object-lifecycle-R41 | 이미 수락된 request의 재제출 금지는 31 §2가 정의 | 202-203 | |
| 47-internal-object-lifecycle-R42 | 언어별 catalog 이름이 달라도 같은 종료 조건을 구현하고 독립된 process evidence로 검증한다. 한 language mapping의 구조 설명은 다른 mapping의 검증 증거를 대신하지 않는다 | 205-206 | |
| 47-internal-object-lifecycle-R43 | 활성 객체 수 상한은 배치 선택과 로컬 활성화 양쪽에서 적용해야 한다 | 210-211 | |
| 47-internal-object-lifecycle-R44 | 상한 두 지점: 배치 선택(상한 가까운 node를 새 객체 후보에서 뺌), 로컬 활성화(상한 넘으면 그 node에서 활성화를 거절) | 215-220 | |
| 47-internal-object-lifecycle-R45 | 배치 단계에서만 막으면 이미 그 node를 가리키는 요청이나 이동으로 들어오는 객체는 상한을 그냥 통과한다 | 222 | |
| 47-internal-object-lifecycle-R46 | 정리 대상은 Instance Spot뿐이다 — IdleEvicted 종료 사유는 Instance Spot 한정 | 227-228 | |
| 47-internal-object-lifecycle-R47 | User Spot을 정리하지 않는 이유: 정리된 User Spot을 일반 message가 다시 만들지 않기 때문 | 228-229 | |
| 47-internal-object-lifecycle-R48 | 없는 객체를 만들 수 있는 것은 Instance intent를 명시한 호출뿐(§3) | 230 | |
| 47-internal-object-lifecycle-R49 | Entry Spot은 그 Object Server의 lifecycle에 속하므로 애초에 정리 대상이 아니다(§2) | 231 | |
| 47-internal-object-lifecycle-R50 | 정리 기준은 "마지막 활동 이후 경과 시간"과 "지금 진행 중인 작업이 없음"을 함께 만족해야 한다 — 시간만 보면 오래 기다리는 작업이 있는 객체를 지운다 | 233-234 | |
| 47-internal-object-lifecycle-R51 | Framework는 정리할 때 application 상태를 보존하지 않는다 — 유지해야 하는 상태는 application이 종료 callback에서 직접 저장한다 | 236-239 | |
| 47-internal-object-lifecycle-R52 | Framework가 상태를 대신 저장하려면 무엇을 저장할지 알아야 하고 그것은 application의 몫이다 | 239 | |
| 47-internal-object-lifecycle-R53 | process 단위 byte 회계와 Spot 단위 byte 회계는 서로 다른 회계 단위 — 한쪽 숫자를 다른 쪽 상한으로 재사용하지 않는다 | 243-246 | |
| 47-internal-object-lifecycle-R54 | 실행 queue는 application과 lifecycle을 별도 FIFO lane으로 두고 각 lane에 count·byte reservation을 둔다 | 248-249 | |
| 47-internal-object-lifecycle-R55 | Application lane 기본값: 1,024건·64 MiB. lifecycle lane 기본값: 128건·4 MiB | 249-250 | |
| 47-internal-object-lifecycle-R56 | Accepted application work는 payload 크기와 work당 고정 retained cost 256 byte를 함께 예약. Reservation은 handler terminal completion에서 반납 | 250-251 | |
| 47-internal-object-lifecycle-R57 | Relocation hold에는 relocation 전용 건수·byte 상한을 두지 않는다 | 252 | |
| 47-internal-object-lifecycle-R58 | process HWM이 남아 있어도 Spot queue가 먼저 포화될 수 있고, 반대로 Spot queue에 여유가 있어도 process inbound admission이 먼저 멈출 수 있다 | 254-255 | |
| 47-internal-object-lifecycle-R59 | 두 결과를 같은 CapacityExceeded 상황으로 합치지 않고 실제 admission 실패 queue에 따라 구분 | 255-256 | |
| 47-internal-object-lifecycle-R60 | 실행 대기열의 한도는 건수와 byte 두 축을 모두 강제하고 먼저 걸리는 쪽을 적용 | 260-261 | |
| 47-internal-object-lifecycle-R61 | 한 축만으로는 다른 축으로 우회할 수 있다: 건수만 두면 큰 payload 몇 건이 memory를 채우고, byte만 두면 빈 payload를 무한히 쌓아도 걸리지 않는다 | 264-265 | |
| 47-internal-object-lifecycle-R62 | byte 회계는 payload 크기만 세지 않는다 — envelope·metadata·queue node 포함, 정확히 계산 불가한 언어는 작업당 고정 비용을 더한 값을 씀, payload 비어도 작업 하나는 0 byte가 아니다 | 267-269 | |
| 47-internal-object-lifecycle-R63 | 대기열 한도가 존재하는 이유: 메모리를 묶어 두는 양을 정하는 것, 밀린 일이 얼마나 되는지 판단하는 것 — 건수는 둘 중 어느 것도 알려 주지 못한다 | 271-272 | |
| 47-internal-object-lifecycle-R64 | 같은 1,024건이라도 100byte짜리면 약 100KB, 1MiB짜리면 1GiB — 메모리가 1만 배 차이 나도 한도는 똑같다 | 274-275 | |
| 47-internal-object-lifecycle-R65 | 처리량은 초당 몇 건이 아니라 초당 몇 byte에 가깝게 움직이므로 밀린 양을 재려면 byte로 재야 한다 | 275-276 | |
| 47-internal-object-lifecycle-R66 | 건수 한도는 두 방향으로 틀림: 작은 message 몰리면 메모리 여유 있는데 거절, 큰 message 몰리면 한도 안 걸리는데 메모리 고갈 | 280-283 | |
| 47-internal-object-lifecycle-R67 | process 단위 회계가 이미 byte로 되어 있고, 같은 기준을 Spot 단위로 내리면 두 층이 같은 단위를 써서 어느 층에서 걸렸는지도 구분된다 | 285-286 | |
| 47-internal-object-lifecycle-R68 | 상한이 없는 실행 대기열을 두지 않는다 — 각 lane은 건수와 byte reservation을 모두 가져야 한다 | 288-290 | |
| 47-internal-object-lifecycle-R69 | 초과했을 때의 결과는 하나가 아니다 — 제출 계열과 대기열 위치에 따라 갈림. 표는 41 §2에 있음 | 292-293 | |
| 47-internal-object-lifecycle-R70 | 대기열이 아닌 두 자리도 각각 CapacityExceeded: worker scheduler 대기열, 배치 수용량(admission 판정이지 대기열 포화가 아님) | 295-296 | |
| 47-internal-object-lifecycle-R71 | 이동 중 보류에는 relocation 전용 건수·byte 상한이 없다. 실행 lane의 reservation과 transport·deadline·cancellation 제한을 relocation hold의 별도 상한으로 재사용하지 않는다(30 §9가 정한 규칙) | 298-300 | |
| 47-internal-object-lifecycle-R72 | (§7확인) 세 Spot 종류가 서로 다른 타입, 불가능 조합 없음 | 304-324 | |
| 47-internal-object-lifecycle-R73 | (§7확인) Entry Spot 이동 목록에 없고 Entry Spot의 Actor는 이동 | 304-324 | |
| 47-internal-object-lifecycle-R74 | (§7확인) 없는 ID 일반 message는 생성 안 함 | 304-324 | |
| 47-internal-object-lifecycle-R75 | (§7확인) Instance intent 명시 호출은 생성함 | 304-324 | |
| 47-internal-object-lifecycle-R76 | (§7확인) 동시 생성 요청해도 factory 1회 실행 | 304-324 | |
| 47-internal-object-lifecycle-R77 | (§7확인) "만드는 중" 상태가 위치 캐시에 안 남음 | 304-324 | |
| 47-internal-object-lifecycle-R78 | (§7확인) 만들다 실패한 기록이 시작 시점 정리 | 304-324 | |
| 47-internal-object-lifecycle-R79 | (§7확인) owner availability evidence 변경이 authority release transition 호출 안 함 | 304-324 | |
| 47-internal-object-lifecycle-R80 | (§7확인) Unavailable tag가 terminal completion adapter로만 | 304-324 | |
| 47-internal-object-lifecycle-R81 | (§7확인) activation recovery root·scan key가 target node·lifecycle 포함 | 304-324 | |
| 47-internal-object-lifecycle-R82 | (§7확인) 재생성 직후 세대 불일치로 안 거절 | 304-324 | |
| 47-internal-object-lifecycle-R83 | (§7확인) 낡은 owner 호출이 자동 재시도 없이 오류 | 304-324 | |
| 47-internal-object-lifecycle-R84 | (§7확인) 상한 도달 시 그 node 활성화 거절 | 304-324 | |
| 47-internal-object-lifecycle-R85 | (§7확인) 유휴 정리는 Instance Spot만(Entry/User는 안 됨) | 304-324 | |
| 47-internal-object-lifecycle-R86 | (§7확인) 진행 중 작업 있는 Instance Spot은 유휴 시간 지나도 정리 안 됨 | 304-324 | |
| 47-internal-object-lifecycle-R87 | (§7확인) 정리 시 IdleEvicted로 closing callback | 304-324 | |
| 47-internal-object-lifecycle-R88 | (§7확인) 대기열이 건수·byte 두 축 제한 | 304-324 | |
| 47-internal-object-lifecycle-R89 | (§7확인) 큰 message는 건수 전 byte 한도 걸림 | 304-324 | |
| 47-internal-object-lifecycle-R90 | (§7확인) 빈 payload는 byte 전 건수 한도 걸림 | 304-324 | |
| 47-internal-object-lifecycle-R91 | (§7확인) byte 회계에 고정 비용 포함되어 빈 payload도 소진 | 304-324 | |
| 47-internal-object-lifecycle-R92 | (§7확인) 상한 없는 실행 대기열 없음 | 304-324 | |
| 47-internal-object-lifecycle-R93 | Object별 bounded queue는 ordering/owner isolation이고 host shared queue를 대체하지 않는다 | 328 | |
| 47-internal-object-lifecycle-R94 | Permit·fairness는 46(수신과 dispatch loop), pre-start terminal lease cleanup은 50(Payload 소유권)을 따른다 | 328 | |

## 6. 링크·코드·site 영향

이 주제는 session 파일럿보다 훨씬 넓게 참조된다 — `15-spot-actor`가 문서 전체에서 anchor
링크가 가장 많은 축에 속하고(14개), 10개 문서를 markdown에서 참조하는 파일이 150개를 넘는다
(§1). 여기서는 문장·anchor가 실제로 깨지는 지점을 정리한다.

| 대상 | 처리 |
|---|---|
| 스펙 내부 링크(10개 문서 상호 참조 + 다른 장에서 들어오는 링크) | 새 경로·새 절 anchor로 치환. §3의 새 절 제목이 확정되는 대로 옛 anchor → 새 anchor 치환표를 만든다 |
| `scripts/verify-framework-instance-spot-contracts.sh` | `06-framework-api`, `12-spot-messaging`, `16-spot-address-messaging`, `15-spot-actor`, `21-location-runtime`(다른 주제) 문장 needle을 검사한다. `12`·`15`·`16`의 재작성에서 §1.1(2번째 표)의 needle 문장은 **그대로 유지**해야 CI가 깨지지 않는다 — 문장을 자연스럽게 다듬더라도 이 needle과 정확히 일치하는 부분 문자열은 남긴다 |
| `scripts/verify-framework-submit-api.sh` | `12-spot-messaging`에서 문장 2개(§1.1 표)를 needle로 검사 |
| `test_cpp_framework_layout_contract.cpp` (`actor_model_documents_actor_destroy_lifecycle`) | `14-actor-model.ko.md`에서 문장 12개를 needle로 검사(§1.1 전문). 재작성 시 이 12개 문자열을 줄바꿈 없이 유지 |
| `framework/runtime/conformance/relocation-behavior-v1.json`, `validate-runtime-conformance-fixtures.mjs` | `15-spot-actor.ko.md`의 **절 번호**(`4.2`, `5`, `6`, `7`, `8`)를 `contractSources`에 고정 참조한다. 재작성 뒤 절 번호가 바뀌면(예: 새 문서의 §4 대신 §3) 이 JSON의 `sections` 배열도 함께 갱신해야 한다 — 이번 ko 재작성 단계에서는 옛 문서 경로가 아직 유효하므로 미룬다. 마지막 이동 단계(§5)의 체크리스트에 추가 |
| cpp/dotnet/java cross-language 주석 4곳 | `15-spot-actor.ko.md:489`, `:938` 같은 **줄 번호**를 주석에 박아 둔다. ko 재작성이 끝나 옛 문서를 대체할 때 이 줄 번호도 갱신 대상 — 마지막 이동 단계로 인계 |
| `sample/zoneworld/README.{ko,en}.md`의 **기존 실패 4건** | `15-spot-actor` 옛 anchor(`#3-membership`, `#4-actor의-spot-join`) 2개가 실제 절 제목(`3. Entry Spot과 User Spot의 Actor membership`, `4. Actor join과 commit 순서`)과 슬러그가 어긋나 깨져 있다(ko 2 + en 2 = 4, `check_doc_links.py` 기준선에 이미 명시됨). 이 주제를 재작성하면 **함께 고쳐야 한다** — 새 절 제목에 맞는 정확한 anchor로 zoneworld README도 갱신(zoneworld README 자체는 이 주제 산출물이 아니므로 건드릴 파일 목록에는 없지만, 마지막 이동 단계에서 이 4건 해소를 명시적으로 확인) |
| 레거시 `<a id>` | 이번 grep에서는 발견되지 않음(session 파일럿과 달리 이 열 개 문서에는 수동 anchor가 없어 보인다 — 재작성 중 실제로 없는지 다시 확인) |
| mkdocs nav | "Spot, Actor와 Session" 그룹을 `03-spot-actor/README`, 01~09 문서로 교체(마지막 단계) |
| redirect | `11-spot-model`→`03-spot-actor/01-spot-model` 등 10개 옛 경로 → 새 경로(마지막 단계, 45는 08-routing으로 부분 병합이므로 리다이렉트 대상이 07-channel-topology 계열과 겹칠 수 있음에 주의) |
| 검증 | `check_doc_links.py`(기준선 4건은 이 주제가 해소), `check_doc_tabs.py`, `mkdocs build --strict`, `git diff --check`, cpp layout contract test(문서 이동 뒤) |

## 7. 구현 대조 — 에이전트 입력

재작성이 끝나면 언어별 에이전트 4개(dotnet·jvm·cpp·node)를 병렬로 띄운다. 프롬프트 양식은
session 파일럿([topics/04-session/gap-dotnet.md](../04-session/gap-dotnet.md) 등)을 그대로
따른다.

- **입력**: 새로 쓴 9개 문서(README 포함 10개) 전체와 이 매핑표 §5 대장(새 위치 열을 채운 것),
  그리고 §4 S20에서 등록한 spec-gap 후보
- **과제**: 대장의 각 행(1,882개, 방대하므로 문서별로 나눠 배정하거나 — dotnet은 11·12·13·17,
  jvm은 14·15, cpp는 16·18, node는 45·47처럼 — 4명이 224건 안팎씩 맡는 방식도 가능하되 코디네이터가
  4언어 모두가 전체 대장을 훑도록 최종 조정한다) 대해 **일치 / 불일치 / 스펙 미정 / 판단 불가**와
  근거(파일:줄)
- **특히 확인할 것**: S20의 Failed/Aborted leaf 이름이 4언어 구현에서 실제로 어떻게 노출되는지
  (public error kind 이름, enum 값)
- **금지**: 스펙 수정, 코드 수정, 판정. 하위 에이전트를 띄우지 말고 직접 grep
- **출력 양식**: `| R# | 판정 | 근거 | 비고 |`

판정(Claude): 옛 문서·4개 구현이 일치하는데 새 문서만 다르면 **문서 오류** → 수정. 옛 문서
때부터 달랐거나 언어끼리 다르면 **spec gap** → [spec-gap 대장](../../spec-gap.ko.md) 등록.

## 8. 작업 순서

이 주제는 문서 수가 가장 많고(9개) 서로 의존이 깊다. 완전 병렬은 불가능하지만 아래처럼
단계를 나누면 상당 부분 동시에 진행할 수 있다.

1. **1차 병렬 — 서로 참조가 적은 기초 문서 4개를 동시에 쓴다.**
   `01-spot-model`(11), `03-mesh-node`(13), `04-actor-model`(14), `07-stage-wrapper-on-spot`(17).
   이 넷은 서로 본문을 크게 참조하지 않으므로 동시에 시작해도 안전하다(단, `04-actor-model`은
   §6.4-6.5를 `05-spot-actor-membership`로 넘기는 전제가 있으므로 §3.1의 S18 판정을 먼저
   에이전트에게 고정 지시로 준다).
2. **1차가 끝나는 대로 2차 병렬 — 1차 결과를 참조하는 문서 3개.**
   `02-spot-messaging`(12, cold activation을 06으로 링크), `05-spot-actor-membership`(15, 생성
   reservation을 단독 소유하고 04로부터 받은 §6.4-6.5 내용을 흡수), `06-spot-address-messaging`(16,
   cold activation·route cache를 단독 소유). 이 셋은 서로 겹치지 않으므로 동시에 가능하다 — 단
   `12`와 `16`이 같은 cold activation 절차를 다루므로(S1) **`16`을 소유 문서로 먼저 확정**한 뒤
   `12`를 쓰는 순서가 안전하다(완전 동시가 불안하면 `16` 먼저 → `12`를 10분 뒤 시작).
3. **3차 — `08-routing`(18+45 §1·§1.1·§2 병합)과 `09-object-lifecycle`(47).**
   `18`은 `16`(route cache 세부)과 `15`(Actor relocation route 갱신)를 링크하므로 2차 완료 후
   시작. `47`은 `18`(§2.5 ObjectGeneration 사용처)을 링크하므로 마찬가지. 이 둘은 서로 독립이므로
   동시에 가능하다.
4. **README.ko.md**는 8개 본문 절 제목이 확정된 뒤 마지막에 쓴다(§2 질문표 기준, 다른 8개
   문서의 실제 절 제목을 정확히 링크해야 하므로).
5. 등가성 대조 — 대장(§5) 빈 행 0, 추가 보장 0을 grep으로 확인.
6. en 짝 작성(마지막 단계, §5 캠페인 README와 동일 원칙).
7. 구현 대조(§7) → 판정·기록.
8. 이동·nav·cpp test·conformance JSON 절 번호 갱신·zoneworld anchor 4건 해소(§6) → 검증 4종
   그린 → 한 커밋.

병렬 폭에 대한 특이사항: `02-channel-transport` 주제가 이미 진행 중이거나 예정이라면, S7(45
§3-§7)과 S12(발행 완료 계약)를 그 주제 담당자에게 미리 알려 중복 작업을 막는다 — 이 주제와
02-channel-transport가 같은 시점에 `45`를 손대면 충돌한다.

## spec-gap 후보

이번 재작성이 아니라 [spec-gap 대장](../../spec-gap.ko.md)으로 넘길 실제 스펙 결함 후보다.
스펙 자체를 고치지 않고 위치만 기록한다.

- **G1 (S20)** — `14-actor-model.ko.md` 539~549행의 생성 상태 다이어그램과
  `15-spot-actor.ko.md` 146~152행의 같은 다이어그램이 세 번째 leaf 이름을 각각 `Failed`,
  `Aborted`로 다르게 쓴다. `15` 155~156행 산문은 "callback exception은 Failed, recovery
  cleanup은 (terminal record 없는) Abort"라고 **둘을 구분되는 별개 결과로** 설명하므로, `15`의
  다이어그램이 그 자리에 `Aborted`만 적어 둔 것 자체가 `15` 산문과도 어긋날 수 있다. 4언어
  구현이 이 갈림을 어떻게 다루는지(별도 error kind 2개인지, 하나로 합쳐 있는지) 대조가 필요하다.
  출처: `14-actor-model.ko.md:539-549,714-716`, `15-spot-actor.ko.md:146-156`.
- **G2** — `18-object-routing.ko.md` §2.2(94-119행)의 route cache 필드·수명 규칙과
  `45-internal-routing-and-cache.ko.md` §1(21-69행)의 같은 규칙은 수치(`RouteCacheMaxAge`
  기본 15초, Message Follow보다 최소 5초 짧게)까지 일치하는 것으로 확인했으나(§3.1에서 대조
  완료), **4언어 구현이 실제로 같은 기본값을 쓰는지는 확인하지 않았다** — 구현 대조(§7) 단계로
  넘긴다. 실제 스펙 문서 사이 모순은 아니므로 G1보다 우선순위가 낮다.
- **G3** — `12-spot-messaging.ko.md`와 `16-spot-address-messaging.ko.md`가 cold activation을
  각각 8단계·11단계로 서로 다른 단계 수로 서술한다(§4 S1). 두 목록이 같은 절차의 다른 세분화
  수준이라 내용 모순은 없어 보이지만, 재작성 후 `16`을 단독 소유로 확정하면서 이 단계 수 자체가
  스펙이 명시적으로 요구하는 값인지(즉 "정확히 8단계/11단계"가 계약인지, 서술 편의인지) 재작성
  에이전트가 판단하기 어려울 수 있다 — 재작성 중 애매하면 이 항목으로 에스컬레이션.

---

[스펙 목차](../../../../../framework/doc/framework/common/spec/server/README.ko.md) ·
[캠페인 지침](../../README.ko.md) · [spec-gap 대장](../../spec-gap.ko.md)
