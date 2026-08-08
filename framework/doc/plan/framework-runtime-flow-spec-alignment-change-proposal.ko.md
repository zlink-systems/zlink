# Framework runtime 흐름 단순화 — 보호 문서 변경 제안서

> 상태: `framework/doc/framework/common/internals/`와
> `framework/doc/framework/common/spec/server/`에 적용할 변경을 `file:line` 단위로 고정한 제안서다.
> 이 문서는 공개 계약이 아니며 아직 어떤 보호 문서도 수정하지 않았다.
>
> 대상 독자와 질문: 보호 문서 수정을 승인하는 사용자가 "어느 문장을 어떤 근거로 어떻게 바꾸며,
> 지금 바꿀 수 있는 것과 결정이 남은 것이 무엇인가"를 판단할 수 있게 한다.
>
> 검토 기준일: 2026-08-08, `main` branch의 working tree
>
> 이 문서는 원래 별도의 개선 계획서와 WP-0 baseline inventory를 근거로 삼았으나, 두 문서는 스펙 정합화
> 완료 후 정리되었다. 이후 구현 적용에 필요한 결정 결과와 결함 목록은 §10과 §11에 남긴다.

## 1. 왜 편집 대신 제안서인가

`AGENTS.md`의 보호 문서 규칙은 대상 경로를 지정받았더라도 변경 **내용**에 대한 승인을 따로 요구한다.
같은 규칙이 "계획서"와 "스펙에 맞춰"라는 지시를 명시적 수정 승인으로 인정하지 않으며, 수정 필요성을
발견하면 파일을 먼저 고치지 말고 정확한 `file:line`, 변경 이유와 제안 내용을 보고한 뒤 대기하도록
정한다. 사용자가 지정한 것은 세 경로이고 변경 내용은 개선 계획서가 근거이므로, 이 단계에서 필요한
산출물은 편집이 아니라 이 제안서다.

두 번째 이유는 개선 계획서 자신의 gate다. 계획서는 D-DATA부터 D-DRAIN까지 각 decision의 근거가
채워지기 전에는 관련 schema나 public interface를 수정하지 않는다고 정한다. 여덟 개 결정 가운데
D-READY의 Ready 분리와 D-FAILOVER의 failover 범위만 승인 상태이고 나머지는 미결이며, 진입 조건인
WP-0 inventory는 아직 수행하지 않았다. 따라서 "누락 없이 전부 반영"은 지금 시점에 달성할 수 없다.
미결 항목까지 문장을 쓰면 보호 계약 안에서 결정을 임의로 만들게 되고, 이는 계획서와 `AGENTS.md`가
함께 막으려는 상황이다.

## 2. 이번 조사로 새로 확정한 사실 — FRS-07

FRS-07은 bound-session request를 `Captured` 전에 terminal까지 drain하라는 규칙과 journal·replay 계약의
충돌이다. 이 충돌이 실재하는지는 "bound-session request의 source가 durable frozen record를 만들 수 있는
lifetime인가"에 달려 있었고, schema 원문에서 결론이 나왔다.

Schema의 durable frozen record 규칙은 `leaseBacked` source만 허용하며, 모든 frozen source가 정확한
owner id와 lease generation을 가져야 한다고 정한다(`service-wire-v1.schema.json:850`). 그런데 같은
schema의 bound-session barrier는 participant identity를
`session-owner-node-rid-generation-owner-id-lease-generation-session-rid-binding-generation`으로 정의해
owner id와 lease generation을 **포함**하고(`:747`), record sequence를
`bound-session-actorSend-or-actorRequest-sourceSessionSequence`로 정의해 bound-session request를 replay
가능한 participant record로 취급한다(`:748`).

여기서 주의할 점이 있다. 이 participant identity는 session owner **node**를 식별하는 값이고, schema가
bound-session record의 source identity로 정의한 값은
`actor-session-rid-sequence-and-nonzero-binding-generation-all-required`로 owner id와 lease generation을
포함하지 않는다(`:847`). 따라서 "bound-session request의 lifetime이 `leaseBacked`다"라고 단정할 수는
없으며, 그 분류는 D-DRAIN 조사가 확정해야 한다.

단정할 수 없는 부분을 빼도 충돌 자체는 남는다. 같은 schema 안에서 barrier는 bound-session
`actorRequest`를 replay sequence에 포함하는데(`:748`), `preCapturedDrain`(`:851`)과
`requestDrainBeforeCaptured`(`:1189`)는 같은 request를 terminal까지 drain하고 journal에 넣지 말라고
정한다. 정식 spec은 barrier 쪽과 같다. Session–Actor spec은 "Session에서
Actor로 전달한 요청도 다른 Actor 요청과 같은 규칙을 따른다. Seal 전에 Actor queue가 수락한 요청은
저장한 기존 작업에 포함하고, seal 뒤 owner commit 전에 source로 들어온 요청은 ingress hold에서 target
temporary queue로 relay한다"고 정한다(`20-session-actor-dispatch.ko.md:453-456`).

FRS-07은 실재하는 충돌이다. 다만 아래 §4의 B-1에서 설명하듯 실제 문장 교체는 D-DRAIN의 남은 조사를
필요로 한다.

## 3. 지금 적용할 수 있는 변경 — 정식 spec이 결과를 정한 항목

아래 세 건은 정식 spec의 문장이 명확하고 개선 계획서의 decision 상태도 승인이다. 승인만 있으면 바로
적용할 수 있다. 각 항목은 한국어와 영어 mirror를 같은 commit에서 함께 바꾼다.

### A-1. Target Ready를 route ACK와 cleanup에서 분리한다

| 항목 | 내용 |
|---|---|
| 대상 | `internals/12-service-wire-protocol.ko.md:508-512`, `internals/12-service-wire-protocol.en.md:553-559` |
| 현재 문장 | "`Activated`는 Ready가 아니다. Target application admission은 durable source cleanup, `Completed` CAS, bound-session route ACK와 steady authority normalization이 모두 끝날 때까지 닫혀 있다." |
| 제안 문장 | Target application admission은 정식 spec이 정한 네 조건, 즉 owner와 membership 변경, lifecycle callback·미완료 작업·timer 복원, 저장된 기존 작업과 temporary queue 작업의 execution queue 반영, temporary queue 등록 제거와 기존 dispatch 경로로의 atomic 전환이 끝나면 연다. Source ingress hold 원본 제거, 위치 record의 `Completed` 변경과 Session Actor 위치 갱신 응답은 target application message 처리를 막지 않으며, source와 target runtime이 이 후속 작업을 각자 계속한다. |
| 근거 | `spec/21-location-runtime.ko.md:942-947`이 Ready 조건 네 가지를 나열한다. 특히 `:945`는 "Lifecycle callback, 미완료 작업과 timer 복원을 완료했다"이므로 timer 복원을 빠뜨리면 안 된다. `:949-951`이 admission을 막지 않는 후속 작업 세 가지를 이름으로 지정한다. `spec/28-graceful-drain-handoff.ko.md:487-491`, `spec/20-session-actor-dispatch.ko.md:339-340,358-359,396`도 같은 내용이다. |
| 범위 주의 | 현재 internals 문장이 나열한 네 gate 가운데 durable source cleanup, `Completed` CAS, bound-session route ACK는 spec `:949`가 이름으로 풀어 주지만 **steady authority normalization은 spec에 대응 문장이 없다**. 이 항목까지 admission을 막지 않는다고 쓰면 spec에 없는 보장을 추가하는 것이므로, 그 처리는 D-READY에 남긴다. |
| Decision 상태 | D-READY 성공 경로 — 승인 |

### A-2. `TargetAttemptGeneration`을 같은 target의 재시도 구분자로 되돌린다

| 항목 | 내용 |
|---|---|
| 대상 | `internals/12-service-wire-protocol.ko.md:465-466`, `internals/12-service-wire-protocol.en.md:509-511` |
| 현재 문장 | "같은 relocation에서 target을 바꿀 때는 stable `RelocationId`와 relocation root를 유지하고 `TargetAttemptGeneration`만 증가시킨다." |
| 제안 문장 | 같은 target process에서 준비를 다시 할 때는 stable `RelocationId`와 relocation root를 유지하고 target 시도 번호와 준비 정보만 교체한다. `TargetAttemptGeneration`은 같은 target에 보낸 중복 또는 이전 Restore 요청을 구분하며 다른 target을 선택하는 데 사용하지 않는다. |
| 근거 | `spec/21-location-runtime.ko.md:776`이 "같은 target에 보낸 중복 또는 이전 Restore 요청을 구분하는 0이 아닌 값이다. 다른 target 선택에 사용하지 않는다"고 정하고, `:845-847`이 "같은 target process에서 준비를 다시 하면 target 시도 번호와 준비 정보만 교체한다. 다른 target으로 교체하지 않는다"고 정한다. 시도 번호만 바뀐다고 쓰면 준비 정보 교체를 부정하게 되므로 두 가지를 함께 쓴다. |
| Decision 상태 | D-FAILOVER failover 범위 — 승인 |

### A-3. Authority phase의 target 교체 전이를 같은 target 재시도로 한정한다

| 항목 | 내용 |
|---|---|
| 대상 | `internals/12-service-wire-protocol.ko.md:491`, `internals/12-service-wire-protocol.en.md:536` |
| 현재 문장 | "Target replacement는 target attempt, target owner lease와 reservation만 바꾸며 stable identity와 relocation root를 바꾸지 않는다." |
| 제안 문장 | 같은 target의 재시도는 정식 spec이 정한 target 시도 번호와 준비 정보만 바꾸며 stable identity와 relocation root를 바꾸지 않는다. 현재 version에는 다른 target process로 교체하는 전이가 없다. |
| 근거 | `spec/21-location-runtime.ko.md:845-847,918-926`, `spec/28-graceful-drain-handoff.ko.md:37-42,792-798`이 다른 target 자동 선택과 process 종료 뒤 takeover를 현재 계약에서 제외하고 차기 version의 object failover 계약으로 미룬다. |
| 표현 주의 | 현재 internals 문장의 "target attempt, target owner lease와 reservation"은 spec `:841`이 정의한 준비 정보(target 시도 번호, target owner lease, target node, 확보한 공간, 복원 데이터 위치)의 일부만 나열한 것이다. 목록을 그대로 옮기지 말고 spec의 표현을 사용해 exhaustive 목록을 새로 만들지 않는다. |
| Decision 상태 | D-FAILOVER failover 범위 — 승인 |

## 4. 사용자 판단이 필요한 변경

### A-4. Abort에서 source admission 복원을 route ACK에서 분리한다

정식 spec의 문장은 명확하지만 개선 계획서는 이 항목을 D-READY의 abort 하위 결정으로 미결 처리했다.
spec의 명확성이 gate보다 우선하는지 사용자가 판단해야 한다.

| 항목 | 내용 |
|---|---|
| 대상 | `internals/12-service-wire-protocol.ko.md:511-512`, `internals/12-service-wire-protocol.en.md:557-559` |
| 현재 문장 | "Abort도 source route ACK와 steady source normalization이 끝난 뒤 admission을 복원한다." |
| 제안 문장 | Owner를 바꾸기 전 abort에서는 Session route를 바꾸지 않았으므로 route 취소 message나 그 응답을 기다리지 않는다. Admission 복원은 정식 spec이 정한 순서, 즉 `Aborted` 기록, temporary queue 작업 폐기와 원래 순서로의 source queue 복원, 확보한 target 공간과 어디에서도 가리키지 않는 payload 정리, 진행 정보 제거를 끝낸 뒤에 수행한다. |
| 근거 | `spec/21-location-runtime.ko.md:1006-1007`이 "Owner를 바꾸기 전에는 Session Actor 위치 갱신 message를 보내지 않았으므로 취소 message나 응답 대기도 필요하지 않다"고 정한다. 다만 이 문장은 route message 대기만 제거하며, `:996-1005`의 여섯 단계 순서는 그대로 유지해야 한다. |
| 제외 항목 | 늦게 도착한 sealed·route record를 어떤 fence로 거부할지는 D-READY의 seal·abort 하위 결정이 정하므로 이번 문장에 넣지 않는다. |
| 충돌 대상 | schema `service-wire-v1.schema.json:756`의 abort 규칙이 abort route와 routed ACK 뒤에 reopen하도록 정한다. Schema는 이번 지정 경로 밖이므로 문서만 바꾸면 두 층이 어긋난 상태가 된다. |
| Decision 상태 | D-READY abort 하위 결정 — 미결 |

## 5. Decision gate 때문에 지금 쓸 수 없는 변경

아래 항목은 무엇을 바꿔야 하는지는 정해졌지만, 어떤 문장으로 바꿀지는 미결 decision이 정한다.
각 항목의 "해제 조건"은 개선 계획서가 정한 진입 조건이다.

| ID | 대상 `file:line` | 무엇이 바뀌는가 | 막고 있는 gate와 해제 조건 |
|---|---|---|---|
| B-1 | `internals/12-...ko.md:431` / `.en.md:471`, `spec/server/languages/cpp/interfaces/02-configuration-host.ko.md:126-128` / `.en.md:150-153` | Bound-session request를 `Captured` 전 terminal drain 대상에서 빼고 journal·replay 대상으로 되돌린다. C++ interface 문서의 `blocked/deadline_exceeded` 설명도 같은 의미로 좁힌다. | D-DRAIN. 네 언어의 frozen record가 request source fence, operation identity와 reply route를 보존하는지, `replyRelay` 경로로 terminal 결과를 한 번만 전달하는지 확인해야 문장을 확정할 수 있다. |
| B-2 | `internals/12-...ko.md:163` (command table), `:440-446` / `.en.md:180,484-488` | Relocation Store를 seal한 journal의 단일 data plane으로 고정하고, `relocationData(31)`의 역할 설명을 선택한 의미로 바꾼다. | D-DATA. C++·.NET·Node는 payload stream으로, Java는 control-only record로 같은 command를 해석하므로 어느 쪽을 공통 계약으로 삼을지 먼저 정해야 한다. |
| B-3 | `internals/12-...ko.md:520` / `.en.md:567` | "Command 44·45는 `Completed` 이후 route switch·ACK에만 사용" 조건과 pre-`Captured` seal 왕복(command 42·43)의 필요 범위를 다시 정한다. | D-READY의 seal 하위 결정. Seal high-water 없이 replay 경계를 고정할 수 있는지, Session owner 무응답 시 어떤 bounded failure로 끝낼지 확정해야 한다. |
| B-4 | `internals/12-...ko.md:497` | Target offer가 고정하는 값과 `Prepared` CAS 수행 주체를 하나로 정한다. | D-RESERVE. 정식 spec의 sequence diagram(`21:222,905`)은 Target 기록으로, schema(`:657`)는 source CAS로 정하고, 같은 spec의 phase 표(`21:841`)는 authority owner만 설명한다. 실제 CAS 호출 runtime을 네 언어에서 확인해야 한다. |
| B-5 | `internals/12-...ko.md:222-223` / `.en.md:249`, `internals/09-session-binding.ko.md:82,88-90,128` / `.en.md:104,111,163` | 고정 100 ms를 유지할지, queue drain과 100 ms 가운데 먼저 도달한 조건으로 바꿀지 정한다. | D-GRACE. Transport가 outbound queue drain 신호를 제공하는지와 그 확인 비용을 먼저 측정해야 한다. |
| B-6 | `spec/server/languages/{cpp/interfaces/05-actors,cpp/interfaces/06-stream-session,dotnet/interfaces/07-stream-session,java/interfaces/stream-session,kotlin/interfaces/stream-session,node/interfaces/02-channel-messaging}.{ko,en}.md` | 각 언어 disconnect notification의 완료 시점을 local admission과 remote callback terminal 가운데 하나로 통일한다. | D-NOTIFY. 언어별 local·remote 경로의 실제 완료 시점을 확인해야 한다. .NET remote 경로는 이미 local send admission에서 완료하므로(`ZLinkActorBoundSessionCoordinator.cs:1075-1154`, `SendFlags.DontWait` 전송은 `:1132`, 즉시 완료 반환은 `:1154`) 어느 쪽을 택해도 한 언어 이상이 바뀐다. |

## 6. 지정한 세 경로 밖에 있어 이번에 닫히지 않는 부분

FRS-01과 FRS-07은 internals와 언어 interface 문서만 바꿔서는 닫히지 않는다. 규범 문장이 지정 경로 밖에
있기 때문이다. 세 경로만 승인하면 구조상 부분 정합화로 끝나므로, 범위를 넓힐지 여부를 함께 판단해야
한다.

| 파일 | 왜 필요한가 |
|---|---|
| `framework/runtime/protocol/service-wire-v1.schema.json` | Ready barrier(`:729-760`), abort 규칙(`:756`), drain 규칙(`:851,1189`), replacement round(`:674-712`)의 실제 입력이다. 문서만 바꾸면 schema가 반대 규칙을 유지한다. |
| `spec/20-session-actor-dispatch.{ko,en}.md` | 고정 100 ms(`:166-171`)와 `NotifyDisconnected`의 remote callback terminal 대기(`:274-276`)가 여기에서 규범이다. internals만 바꾸면 두 문서가 어긋난다. |
| `spec/18-object-routing.{ko,en}.md` | `:269-275`가 rebind terminal을 이전 owner의 ACK 뒤에 반환한다고 설명해 command 51의 one-way 계약과 충돌한다. 이 문장은 "Session owner가 별도 durable retry journal을 보관하지 않는다"는 파생 보장의 근거이기도 하므로, 교체할 때 그 보장을 함께 다시 도출해야 한다. |
| `spec/19-stream-session.{ko,en}.md` | `:255-259,289`가 같은 replacement·grace 규칙을 반복 서술한다. |

## 7. 승인 요청 형식

다음 가운데 무엇을 승인할지 알려주면 그 범위 안에서만 수정한다.

1. §3의 A-1, A-2, A-3만 적용한다. 세 건 모두 정식 spec이 결과를 정했고 decision 상태도 승인이다.
2. 여기에 §4의 A-4를 더한다. spec 문장은 명확하지만 계획서 gate는 미결이므로 사용자가 gate보다 spec을
   우선한다고 판단한 경우에만 포함한다.
3. §6의 파일까지 범위를 넓힌다. FRS-01과 FRS-07을 실제로 닫으려면 schema와 최상위 spec이 필요하다.
4. §5의 B 항목은 WP-0 inventory를 먼저 수행한 뒤 별도 승인으로 진행한다.

## 8. 독립 검증 이력

이 제안서의 초안을 Codex로 독립 검증했고, 지적받은 여섯 건을 모두 원문과 대조한 뒤 반영했다.
검증 범위는 인용 정확성, 충돌 실재성, 제안 문구가 정식 spec을 넘어서는지, 확정 항목 누락 여부였다.

| 지적 | 확인 결과 | 반영 |
|---|---|---|
| A-1 제안 문구가 Ready 조건에서 "미완료 작업과 timer 복원"을 빠뜨렸다 | `spec/21-location-runtime.ko.md:945` 원문으로 확인 | 네 조건을 spec 문장대로 다시 썼다 |
| A-1이 spec에 없는 `steady authority normalization`까지 non-blocking으로 확장했다 | `:949`는 세 항목만 이름으로 지정 | 해당 항목을 D-READY로 넘기는 범위 주의를 추가했다 |
| A-2의 "`TargetAttemptGeneration`만 증가"가 spec 및 A-3과 충돌한다 | `:846-847`은 "target 시도 번호와 준비 정보만 교체" | 두 가지를 함께 쓰도록 고쳤다 |
| A-3의 필드 목록이 spec보다 구체적이다 | `:841`의 준비 정보가 더 넓다 | spec 표현을 쓰도록 바꾸고 표현 주의를 추가했다 |
| A-4가 abort의 여섯 단계 순서를 생략하고 미결 fence를 보장으로 넣었다 | `:996-1005`의 순서 확인 | 순서를 유지하도록 다시 쓰고 fence 문구를 제외 항목으로 옮겼다 |
| §2의 lifetime 단정이 인용 line에서 확정되지 않는다 | `schema:847`에 owner id·lease generation 없음 | 단정을 제거하고 충돌만 남겼다 |
| B-6의 .NET 인용이 현재 working tree와 어긋난다 | 실제 method는 `:1075`에서 시작 | `1075-1154`로 고쳤다 |

확정 항목 가운데 제안서에서 빠진 것은 없다는 점도 함께 확인했다.

## 9. 적용 결과

사용자가 §7의 1번을 승인하여 A-1, A-2, A-3을 `internals/12-service-wire-protocol.ko.md`와 `.en.md`에
적용했다. A-4와 §5의 B 항목, §6의 경로 확대는 적용하지 않았다.

적용 뒤 Codex 재검증은 spec 충실성, 범위 준수, Korean/English mirror 대칭, 잔존 문구 네 축에서 finding
없음으로 판정했다. 그러나 자체 검토가 Codex가 놓친 내부 모순 한 건을 찾아 함께 고쳤다.

`Activated`는 schema에서 target activation이 성공한 상태이고(`service-wire-v1.schema.json:767`의
`target-activation-succeeded`, `:773`의 `cleanupGate`), activation은 replay, timer 복원, queue 병합과
dispatch 전환을 포함한다. 따라서 정식 spec의 Ready 조건 네 가지는 `Activated`에서 모두 끝난다. A-1이
admission을 그 네 조건에 연결한 뒤에는 다음 두 문장이 새 규칙과 어긋난다.

| 대상 | 이전 문구 | 바꾼 문구 |
|---|---|---|
| `12-...ko.md:511` / `.en.md:557` | "`Activated`는 Ready가 아니다" | "`Committed`와 `Activating`은 Ready가 아니다" |
| `12-...ko.md:565` / `.en.md:618` | "`Activated`에서 Ready를 publish하지 않는다" | "Owner commit, restore·replay와 timer 복원, queue 병합과 dispatch 전환을 마치기 전에는 Ready를 publish하지 않는다" |

첫 문장은 A-1이 바꾼 문단 안에 있으므로 A-1의 일부다. 두 번째는 §12 구현 검증 절에 있어 A-1이 지정한
위치 밖이지만, 같은 규칙을 다른 절에서 반복한 문장이라 한쪽만 고치면 문서가 스스로 모순된다. 이 판단으로
함께 고쳤으며 되돌릴 수 있다.

새 문구는 `Activated`가 곧 Ready라고 단정하지 않는다. Durable phase CAS와 local Ready publication의
정확한 대응은 schema 정합화(WP-2A)가 정한다.

적용 뒤 `scripts/verify-framework-doc-contracts.sh`가 통과했다.

이후 WP-0 조사가 §4의 A-4와 §5의 B-1 진입 조건을 실측으로 채워, 사용자 승인 아래 두 건을 추가로
적용했다. A-4는 `internals/12`의 abort 문장을 spec `21:996-1007`의 여섯 단계 순서로 교체했고(§4의 수정
문안 그대로, late-record fence 제외), B-1은 `internals/12`의 drain 조건에서 bound-session request를
분리해 spec `20:453-456`의 journal·ingress hold 규칙으로 되돌리고
`spec/server/languages/cpp/interfaces/02-configuration-host`의 drain 조항을 같은 의미로 좁혔다. 모두
Korean/English mirror 동시 수정이다.

## 10. 구현 적용 시 사용할 결정 결과

WP-0 조사(2026-08-08, 네 언어 runtime 전수 대조)가 닫은 결정이다. §5의 B 항목을 구현에 적용할 때 이
표를 진입 조건 충족의 근거로 사용한다.

| Decision | 결과 | 핵심 근거 |
|---|---|---|
| D-NOTIFY | **awaited lifecycle operation으로 확정. spec은 유지하고 .NET remote 경로를 고친다.** | C++·Java·Node는 이미 remote callback terminal에서 완료한다. .NET remote 경로만 `SendFlags.DontWait`로 즉시 반환한다(`ZLinkActorBoundSessionCoordinator.cs:1075-1154`). one-way로 바꾸면 세 언어와 계약 test 4건이 깨진다. |
| D-GRACE | **고정 100 ms timer 유지.** | Core에 drain·flush·queue-empty 신호가 없다. `snd_pending_msgs`는 poll 전용이고 socket 전체 합계라 session 하나를 분리할 수 없다. 대신 Java의 실효 125 ms(`ZLinkStreamRuntime.java:529-531,550`)와 언어별 deadline 불일치(5초/`DefaultRequestTimeout`/30초)를 정리한다. |
| D-FAILOVER | **coordinator role 제거 가능. `sourceLeaseExpired` cleanup의 수행 주체는 target이다.** | 네 언어 모두 role 3을 발신하지 않고 C++·Java는 수신도 거부한다. 단 .NET만 commit 뒤 same-process target 교체를 실제 구현한다(`ZLinkStandaloneActorRelocationTakeoverCoordinator.cs:103-107,225-260`) — 제거 시 이 경로의 처리를 함께 결정해야 한다. |
| D-DATA | **Relocation Store 단일 data plane. Java의 control record 형태가 공통 계약 후보다.** | C++·.NET·Node는 Store에 seal한 journal을 command 31로 재전송하고, Java는 Store만 사용한다. C++ target은 store payload를 버리고 wire를 기다리기까지 한다(`public_host_runtime.cpp:3266-3269`). |
| D-DRAIN | **journal 보존으로 확정(문서 반영 완료). C++에 producer 추가가 남는다.** | Java는 이미 source fence·operation identity·reply route를 보존해 journal한다(`ZLinkServiceFrozenRecordCodec.java:163,214-222,243-249`). C++는 journal하되 session 신원을 벗긴다(`raw_stateful_dispatch.cpp:338-340`). |
| D-RESERVE | **미결. "phase를 durable authority 상태로 둘 것인가"를 먼저 정해야 한다.** | phase 3을 durable로 쓰는 언어가 없다. C++는 phase byte 자체가 없고, Java는 4·8 두 값, .NET은 전체 ladder(target 기록), Node는 store 미도달. store write 주체도 source 2 대 target 2로 갈린다. |

구현 적용 시 주의할 사실 두 가지. Schema의 command 42~45 wire 형식을 쓰는 runtime이 없다 — C++는
receive-only, .NET은 `$zlink.session.route-*.v1` named packet, Node는 Entry-Spot packet, Java는 44·45만
wire다. 그리고 spec의 route retry ladder(`1,1,2,4,5`→5초)와 네 결과 값(`Applied` 등)은 어떤 runtime에도
없고 command 45 body에 result field 자체가 없다.

## 11. 조사 중 확인된 결함 목록

단순화 계획과 무관하게 실재하는 결함이다. 각각 독립적으로 수정할 수 있다.

| # | 결함 | 위치 | 영향 |
|---|---|---|---|
| 1 | .NET `NotifyDisconnectedAsync` remote 경로가 spec과 달리 local 전송 직후 완료 | `ZLinkActorBoundSessionCoordinator.cs:1075-1154` | 공개 계약 위반 |
| 2 | C++ command 35 유실 시 target이 `recovering`에 영구히 머묾. durable 대체 경로 없음 | `public_host_runtime.cpp:2820-2827` | 복구 공백 |
| 3 | C++ source가 commit 뒤 종료하면 `Completed` 도달 불가. `sourceLeaseExpired` 확인 함수가 test 전용 | `raw_stateful_dispatch.cpp:1961-1997` | 복구 공백 |
| 4 | Java target 복구가 `sourceCleanupCompleted`에서 무한 대기. source cleanup을 boolean으로만 모델링 | `ZLinkCanonicalRelocationStateMachine.java:117-119` | 복구 공백 |
| 5 | Java production encoder가 command 30 sentinel 규칙을 강제하지 않아 .NET이 거부 가능 | `ZLinkCanonicalRelocationProtocol.java:69-78` vs .NET `MatchesAcceptance:2914-2943` | cross-language 상호운용 |
| 6 | Node가 규격 준수 peer의 command 42~45 frame을 transport allowlist 통과 뒤 조용히 폐기 | `service-stateful-runtime.ts:1801-1813` | 무응답 |
| 7 | Java `isKnownCommand` 허용 목록에 command 51 누락. generic decode 경로로 오면 실패 | `ZLinkServiceWireCodec.java:108-153` | 잠재 결함 |
| 8 | C++ command 34 수신 처리 분기 없음. `unsupported infrastructure mailbox command`로 종료 | `public_host_runtime.cpp:3789` | dead wire surface |
| 9 | .NET `entry ? ActorLifecycleKind.Joined : ActorLifecycleKind.Joined` — 양쪽 분기 동일 | `ZLinkManagedMeshNode.cs:3215` | 명백한 오타 버그 |
| 10 | Node relocation seal API 3종이 test에서만 호출됨 | `service-mailbox.ts:193-240` | dead API |
| 11 | Java cross-node bound-session reply relay의 landing site 부재. reply route가 accepting node에만 등록되는데 relay는 `sourceNodeRid`로 주소 지정 | `ZLinkJavaRawMeshNode.java:5398-5406`, `ZLinkUserSpotRetireTargetEndpoint.java:448-450` | cross-node 결함 의심 |
| 12 | Java에서 `lastAcceptedSessionSequence`가 wire 40·30·41을 건너며 유실되어 target이 0으로 재구성 | `ZLinkCanonicalRelocationStateMachine.java:1106` | fence 약화 |
