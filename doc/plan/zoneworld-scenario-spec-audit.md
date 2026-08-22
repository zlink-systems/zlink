# ZoneWorld SCENARIO/DESIGN 규격 적합성 감사

## 결론

**NOT-COMPLIANT / NOT-CONVERGED**

ZoneWorld 공통 시나리오의 큰 구조는 Stage-wrapper, global Spot/Actor routing, Actor Join relocation,
target-only CAS, Message Follow, bound-session route update, crash 비-failover라는 규격 방향과 대체로
맞는다. 그러나 stage-7 구현의 공통 기준으로 쓰기에는 다음 경계가 닫히지 않았다.

- 명시적 규격 위반 또는 시나리오 내부 모순: **6개**
- Framework 규격이 소유하지 않는 동작 또는 시나리오가 닫지 않은 동작: **12개**
- 규격과 일치하는 주장: **28개**

가장 큰 문제는 공통 README가 금지한 physical owner/relocation 정보를 shared browser 계약이 다시
노출하고, 언어별 step intent가 zone→NodeId 배치를 고정하며, 초기 입장 diagram이 Actor Join의
`Defer()` 및 completion ordering을 표현하지 않는 점이다. Maintenance도 공통 문서의 "same-zone만
허용"과 .NET step intent의 "same-node cross-zone도 허용"이 서로 다르다.

이 감사는 문서·계약·step intent의 정적 비교다. 구현 완성도, 실행 성공 여부, 언어별 coverage 수는
판정하지 않았다. build/test/E2E는 실행하지 않았고 저장소 파일은 변경하지 않았다.

## 범위와 판정 기준

검토한 시나리오 자료:

- `framework/doc/framework/common/sample/zoneworld/README.ko.md`
- `framework/doc/framework/common/sample/zoneworld/README.en.md`
- `framework/languages/shared_sample/zoneworld/client/**`
- 언어별 `samples/ZoneWorld`의 scenario registry/comment와 그 의미를 고정하는 sample regression test

공통 `shared_sample/zoneworld` 아래에는 별도 scenario/design Markdown이 없다. 따라서 공통 README
ko/en 쌍이 시나리오의 유일한 공통 설계 문서이고, shared browser declaration/test 및 언어별
`ZW-*` registry/test는 그 설계를 구체화하는 contract-test intent로 취급했다.

판정 의미:

- `compliant`: 주장 자체가 인용한 규격 경계를 지킨다.
- `violates(clause)`: 주장 또는 서로 함께 지켜야 하는 scenario/step intent가 규격 조항과 충돌한다.
- `spec-silent`: Framework 규격이 그 application/harness 의미를 정하지 않는다. 시나리오가 충분히
  닫아 둔 단순 domain rule은 문제로 보지 않았고, 언어별 해석이 갈릴 수 있는 항목만 뒤의 (b)에 올렸다.

## Normative claims matrix

| ID | 시나리오의 normative claim과 근거 | governing clause | verdict | 판정 |
|---|---|---|---|---|
| ZW-01 | Gateway 1, Ops 1, 동등 capability의 ZoneNode 2, ZoneId별 Zone Spot 1, PlayerId별 Actor 1을 둔다 (`README.ko.md:82-140`, EN `:85-146`). | Spec 17 §1은 wrapper 형태를 application에 맡긴다 (`17-stage-wrapper-on-spot.ko.md:13-21`). | `spec-silent` | 역할 수와 process topology는 sample domain 선택이다. 자체적으로는 닫혀 있다. |
| ZW-02 | Game client는 Gateway, Ops client는 Ops endpoint만 사용하고 ZoneNode endpoint는 노출하지 않는다 (`README.ko.md:99-116`). | Spec 20 §2 (`20-session-actor-dispatch.ko.md:34-49`), Spec 17 §2 (`17-stage-wrapper-on-spot.ko.md:35-36`). | `compliant` | client가 raw owner/transport endpoint를 직접 다루지 않는다. |
| ZW-03 | 각 ZoneId는 global User Spot 하나이며 Location Store authority로 current owner를 찾는다 (`README.ko.md:120,136,146`). | Spec 17 §6-7 (`17-stage-wrapper-on-spot.ko.md:119-145`), Spec 18 §2.1 (`18-object-routing.ko.md:70-93`). | `compliant` | global logical ID와 authority routing의 경계를 따른다. |
| ZW-04 | 두 ZoneNode는 같은 executable capability와 Actor factory를 제공하며 owner 후보가 된다 (`README.ko.md:111-112,134`). | Spec 14 §6.3 (`14-actor-model.ko.md:472-490`). | `compliant` | role/type/capacity/weight로 Framework가 후보를 고른다는 모델과 맞는다. 단, "네 zone type" 용어는 ZW-14/C-2 참조. |
| ZW-05 | NodeId는 application label이고 owner 계산이나 transport RID로 사용하지 않는다 (`README.ko.md:115,139-140,638-643`; EN `:119,145-146,654-660`). | Spec 18 §2.1 (`18-object-routing.ko.md:81-93`), Spec 13 §3.1 (`13-mesh-node.ko.md:54-78`). | `compliant` | 공통 README의 주장 자체는 정확하다. |
| ZW-06 | shared/browser와 언어별 step intent는 west/east NodeId에 zone을 고정하고 physical owner를 예측한다 (`framework/languages/shared_sample/zoneworld/client/src/shared/config/world.ts:18-19`, `.../src/entities/zone/model.ts:23-25`, `framework/languages/dotnet/samples/ZoneWorld/Server/Configuration/ZoneTopology.cs:17-39`, `framework/languages/dotnet/tests/Zlink.Framework.SampleRegressionTests/ZoneWorldTopologyRegressionTests.cs:329-370`, `framework/languages/node/samples/ZoneWorld/Shared/spec.ts:85-94`). | Spec 18 §2.1: ID를 parse해 node를 추론하지 않고 caller가 owner route를 지정하지 않음 (`18-object-routing.ko.md:81-93`); Spec 14 §6.2-6.3 (`14-actor-model.ko.md:389-404,472-490`). | `violates(Spec 18 §2.1; Spec 14 §6.2-6.3)` | common claim ZW-05 및 완료 기준의 “no custom owner selection” (`README.ko.md:679-680`)과도 직접 모순이다. |
| ZW-07 | release gate는 서로 다른 owner의 인접 zone pair가 반드시 있어야 하고 없으면 실패한다 (`README.ko.md:613-615`; EN `:626-628`). | Spec 14 §6.3은 type/capacity/weight만 정하며 spread/anti-affinity를 보장하지 않는다 (`14-actor-model.ko.md:483-490`). | `spec-silent` | 규격상 두 인접 zone이 다른 owner에 놓인다는 보장이 없다. 이를 보장하는 합법적 fixture 계약도 문서에 없다. |
| ZW-08 | Player Actor가 X/Y/ZoneId 권위 상태를, Zone Spot wrapper가 사본/tick/border state를 소유한다 (`README.ko.md:55,123-124,136-137,583-586`). | Spec 17 §2 (`17-stage-wrapper-on-spot.ko.md:23-36`). | `compliant` | domain state와 runtime 책임 분리가 맞다. |
| ZW-09 | Actor가 Zone state를 바꿀 때 `UpdatePositionMsg` 같은 명시적 Spot message를 사용하고 ActorRef를 cache하지 않는다 (`README.ko.md:385-410,577-585`). | Spec 17 §4 (`17-stage-wrapper-on-spot.ko.md:66-80`), Spec 3 §7 (`03-interaction-model.ko.md:240-263`). | `compliant` | Actor payload를 Spot callback으로 우회하지 않는다. |
| ZW-10 | 별도 실행 모드를 선언하지 않은 Zone User Spot은 기본 `SpotWide` gate를 사용한다. | Spec 17 §3 (`17-stage-wrapper-on-spot.ko.md:38-60`), Spec 15 §3 (`15-spot-actor.ko.md:177-195`). | `compliant` | default가 규격에 닫혀 있으므로 언어별 선택 사항이 아니다. |
| ZW-11 | Zone tick과 bot tick은 Spot/Actor lifecycle 안의 timer이고 Stage state 갱신은 해당 turn에서 한다 (`README.ko.md:52,448-466,583-586`). | Spec 17 §3, §5 (`17-stage-wrapper-on-spot.ko.md:38-64,84-117`). | `compliant` | wrapper가 별도 scheduler나 native handle을 계약으로 노출하지 않는다. |
| ZW-12 | 인접 zone snapshot은 Logical Multicast, 전체 공지·maintenance는 classic fanout이다 (`README.ko.md:66,79,113-114,148-151,448-451`). | Spec 3 §5-6 (`03-interaction-model.ko.md:193-238`), Spec 17 §6 (`17-stage-wrapper-on-spot.ko.md:132-138`). | `compliant` | durable membership source로 multicast를 쓰지 않고 의미별 publish surface를 분리한다. |
| ZW-13 | 좌표/zone/인접/10-cell band/100ms tick/축별 5/시작점/거부 순서/정렬/3-tick expiry와 bot 궤적을 고정한다 (`README.ko.md:46-63,448-466`). | Spec 17 §2는 stage state와 membership policy를 application에 맡김 (`17-stage-wrapper-on-spot.ko.md:25-33`). | `spec-silent` | Framework 규격 밖의 domain rule이지만 common ko/en에 값과 순서가 닫혀 있어 그 자체는 stage-7 차단 항목이 아니다. |
| ZW-14 | 모든 언어가 동일 factory를 찾을 stable type을 사용해야 하지만 common 문서는 이름을 정하지 않고 “네 zone type”이라고만 쓴다 (`README.ko.md:111-112`; EN `:115-116`). | Spec 14 §2.1 (`14-actor-model.ko.md:34-48`); actorJoin receiver는 canonical authority의 stable type을 사용 (`51-internal-service-wire-protocol.ko.md:489-516`). | `spec-silent` | stable type의 exact 문자열과 “type 4개인지/Spot ID 4개인지”가 공통 계약에 닫히지 않았다. 현재 언어 값이 같아도 시나리오 authority가 아니다. |
| ZW-15 | PlayerId가 global ActorId이고 relocation 중 같은 ActorId/ObjectGeneration을 유지하며 owner generation만 전진한다 (`README.ko.md:64-65,137,442-444`). | Spec 14 §2.1-2.2, §6.1 (`14-actor-model.ko.md:34-65,325-345`); Spec 28 §1, §3 (`28-relocation-flow.ko.md:22-26,70-78`). | `compliant` | 동일 incarnation relocation 의미와 맞는다. |
| ZW-16 | bot은 사람과 같은 Player Actor type이지만 bound session이 없다 (`README.ko.md:56,453-466,678`). | Spec 14 §2.3 (`14-actor-model.ko.md:67-80`); Spec 51은 actorJoin 수용에 bound session을 요구하지 않음 (`51-internal-service-wire-protocol.ko.md:513-516`). | `compliant` | membership과 binding의 독립성을 올바르게 사용한다. |
| ZW-17 | JoinWorld는 global PlayerId로 Actor를 create-or-get하고 application은 target RID를 고르지 않는다 (`README.ko.md:398-403,569-580`). | Spec 14 §6.2-6.4 (`14-actor-model.ko.md:389-404,472-519`). | `compliant` | claim 수준에서는 Framework factory/placement authority를 지킨다. ZW-06의 fixture는 별도 위반이다. |
| ZW-18 | 초기 입장은 `A -> Z EnterZoneReq`, `Z -> A EnterZoneRes`, 그 뒤 `A -> G JoinWorldRes`로 그려지고, 일반 request/reply처럼 즉시 결과가 이어진다 (`README.ko.md:385-411`, EN `:394-421`). | Spec 15 §3은 `Defer()` 등록 후 현재 handler 정상 종료 뒤 Join 실행을 요구 (`15-spot-actor.ko.md:191-205`); cross-node 순서도 동일 (`:427-474`). | `violates(Spec 15 §3, §4.2)` | Join을 await하는 동기 request/reply처럼 표현한다. 실제 .NET intent도 `JoinWorldRes`를 먼저 만들고 deferred completion에서 나중 실패를 push한다 (`ZoneEntrySpot.cs:66-99,102-138`, `PlayerActor.cs:89-119`). 따라서 “JoinWorldRes 성공 = zone admission 완료”라는 observable meaning이 닫히지 않는다. |
| ZW-19 | 같은 owner면 membership만, 다른 owner면 동일 Join operation 안에서 Actor relocation을 수행하며 app은 NodeId로 분기하지 않는다 (`README.ko.md:413-417`). | Spec 15 §4 (`15-spot-actor.ko.md:220-227`), §4.2 (`:420-478`). | `compliant` | public JoinSpot의 target selection/relocation 결합과 맞는다. |
| ZW-20 | target Zone Spot의 `OnActorJoin`이 maintenance admission의 최종 권위이고 admission 뒤 capture/restore를 수행한다 (`README.ko.md:67-68,428-439,498-501`). | Spec 15 §4.2 steps 2-4 (`15-spot-actor.ko.md:433-459`), Spec 17 §6 (`17-stage-wrapper-on-spot.ko.md:127-130`). | `compliant` | common prose의 target-owned admission ordering은 정확하다. |
| ZW-21 | .NET step intent는 Entry Spot의 cached precheck와 target Zone Spot admission을 모두 업무 거부 지점으로 둔다 (`framework/languages/dotnet/samples/ZoneWorld/Server/ZoneNode/Infrastructure/ZLink/Spots/ZoneEntrySpot.cs:73-85`, `.../Spots/ZoneSpot.cs:90-118`), while common says target admission alone is final (`README.ko.md:67-68`). | Spec 15 §4.2가 target `OnActorJoin`에 actor-specific admission을 소유시킴 (`15-spot-actor.ko.md:422-446`); Spec 28 §2는 주체 간 결정을 반복 검증하지 않음 (`28-relocation-flow.ko.md:33-46`). | `violates(Spec 15 §4.2; Spec 28 §2)` | “deterministic reply”를 위해 source/Entry cache가 먼저 `ZoneMaintenance`를 반환하면 stale cache가 실제 terminal을 만든다. 이는 target-only final ruling이라는 scenario rule과 single-responsibility validation을 깨뜨린다. |
| ZW-22 | common은 maintenance 중 “같은 zone 내부 이동”만 허용한다 (`README.ko.md:498-501,624-630`; EN `:510-513,637-645`), .NET intent는 같은 NodeId의 서로 다른 zone 이동도 허용한다 (`framework/languages/dotnet/samples/ZoneWorld/Server/ZoneNode/Application/Node/NodeMaintenancePolicy.cs:26-33`, `framework/languages/dotnet/tests/Zlink.Framework.SampleRegressionTests/ZoneWorldMaintenancePolicyTests.cs:9-17`). | Spec 15는 target admission 결과를 존중하되 이 domain policy 값 자체는 정하지 않는다 (`15-spot-actor.ko.md:433-446`). | `violates(common ZoneWorld normative contract; admission semantics consumed by Spec 15 §4.2)` | ko/en 공통 문서와 언어별 contract-test intent가 서로 다른 업무 결과를 요구한다. |
| ZW-23 | Player factory는 `PreserveStateWith`; app payload에는 좌표/ZoneId/bot direction/idempotency state만 넣고 queue/timer/membership/fence는 Framework에 맡긴다. Zone Spot은 `DisableRelocation` (`README.ko.md:156-159`). | Spec 14 §6.1 (`14-actor-model.ko.md:325-359`). | `compliant` | application state와 Framework-owned state 경계가 맞다. |
| ZW-24 | relocation state/queue는 source memory에서 target으로 직접 전달하고 Relocation Store에는 post-relocation pending request terminal만 둔다 (`README.ko.md:120-124,156-159`). | Spec 28 §2, §4.2 (`28-relocation-flow.ko.md:35-43,92-100`); Spec 30 §1 (`30-host-relocation-flow.ko.md:62-76`). | `compliant` | Store를 handoff payload 경로로 사용하지 않는다. |
| ZW-25 | relocation exact identity/fence는 Framework가 소유하고 OwnerNodeRid는 runner probe에만 보인다 (`README.ko.md:380-381,616-618,642-643`). | Spec 28 §3 (`28-relocation-flow.ko.md:70-78`), Spec 24 §4 (`24-runtime-monitoring.ko.md:297-302`). | `compliant` | 관측값을 messaging/placement input으로 쓰지 않는다. |
| ZW-26 | owner commit은 target-only Location Store CAS이고 crash failover와 별개다 (`README.ko.md:505-508`). | Spec 28 §2, §6 (`28-relocation-flow.ko.md:35-46,466-482`). | `compliant` | source/Session owner가 authority를 쓰지 않는 경계와 맞는다. |
| ZW-27 | old route의 send/request는 committed target으로 Follow하며 operation id/generation/payload/reply route를 보존하고 Store 재조회/hidden resubmit을 하지 않는다 (`README.ko.md:442-444,616-618`). | Spec 18 §2.4 (`18-object-routing.ko.md:134-189`); Spec 28 §5.2 (`28-relocation-flow.ko.md:415-427`). | `compliant` | send에는 app ACK를 추가하지 않고 request correlation/deadline을 보존한다. |
| ZW-28 | `EnterZoneReq/Res`는 typed application request/reply이고 raw/private framing을 scenario API로 노출하지 않는다 (`README.ko.md:415-417,573-579,679-680`). | Spec 51 actorJoin envelope/reply (`51-internal-service-wire-protocol.ko.md:465-487`). | `compliant` | c56714a52c 이후 규칙인 command 28 request + command 20 reply의 sole multipart part와 충돌하는 prose는 없다. 다만 raw cross-language contract test intent는 B-8 참조. |
| ZW-29 | A→B→A 반복 relocation도 같은 ActorId/ObjectGeneration과 binding을 유지한다 (`README.ko.md:671-673,692`). | Spec 14 §2.2, §6.1 (`14-actor-model.ko.md:50-65,340-359`), Spec 28 §3 (`28-relocation-flow.ko.md:70-78`). | `compliant` | 각 leg가 성공한 planned relocation이라는 조건에서는 맞다. |
| ZW-30 | bound session은 message마다 Store를 조회하지 않고 commit 뒤 같은 incarnation의 route snapshot만 target으로 갱신하며 app rebind를 요구하지 않는다 (`README.ko.md:80,133,153`). | Spec 20 §2 (`20-session-actor-dispatch.ko.md:34-57`), §5 (`:309-396`); Spec 31 §6 (`31-failure-failover-policy.ko.md:191-207`). | `compliant` | binding identity와 physical connection owner를 Actor owner와 분리한다. |
| ZW-31 | cross-node move 및 완료 기준은 WebSocket connection/session binding이 항상 유지된다고 단정한다 (`README.ko.md:64-65,153,613-615,671-673`; EN `:66-67,159,626-628,693-695`). | Spec 20 §5 step 8-9 (`20-session-actor-dispatch.ko.md:362-368,394-396`)와 Spec 28 §7 (`28-relocation-flow.ko.md:500-505`)는 exact route update가 timeout 안에 올 때만 유지하고, timeout이면 physical connection을 종료한다. | `violates(Spec 20 §5; Spec 28 §7)` | success-path criterion으로는 가능하지만 failure/recovery expectation이 없는 무조건 보장 문구는 규격보다 강하다. |
| ZW-32 | common wire는 `JoinWorldRes`/`ZoneChangedNotify`에서 physical placement를 숨기고 특히 relocation 여부/owner RID 노출을 금지한다 (`README.ko.md:181-218`; EN `:185-226`). shared browser는 `nodeId`, `transferred`를 wire field/UI/assertion으로 요구한다 (`framework/languages/shared_sample/zoneworld/client/src/shared/api/contracts.ts:29-54`, `.../src/widgets/game-hud/game-hud.tsx:18-23`, `.../tests/live/server.spec.ts:16-19`). | Spec 20 §2 (`20-session-actor-dispatch.ko.md:34-38`), Spec 18 §2.1 (`18-object-routing.ko.md:81-93`), Spec 28 §1 (`28-relocation-flow.ko.md:22-26`). | `violates(Spec 20 §2; Spec 18 §2.1; Spec 28 §1)` | ko/en 공통 wire declaration과 shared browser contract가 직접 갈라졌고 physical topology를 player-facing contract로 승격했다. |
| ZW-33 | 특정 player push는 current bound session을 통해 전달하고 bot에는 push하지 않는다 (`README.ko.md:80,453-454,619-620,678`). | Spec 20 §2 (`20-session-actor-dispatch.ko.md:40-57`), Spec 14 §2.3 (`14-actor-model.ko.md:67-80`). | `compliant` | session 유무에 따른 push surface 사용이 맞다. |
| ZW-34 | Ready owner process가 죽으면 current operation은 `Unavailable`, 자동 replacement/new incarnation은 없다 (`README.ko.md:70,503-508,633-634,681`). | Spec 31 §4.2 (`31-failure-failover-policy.ko.md:106-123`), §5 (`:168-189`). | `compliant` | planned relocation과 crash failover를 구분한다. |
| ZW-35 | `SetMaintenanceReq`는 app desired-state store + fanout + join admission만 바꾸며 Host `Relocate`는 호출하지 않는다 (`README.ko.md:67-68,78,468-501`). | Spec 30 §1은 Framework maintenance relocation trigger를 `Relocate(PlannedMaintenance)`로 정의 (`30-host-relocation-flow.ko.md:14-34`), 그러나 app admission-only maintenance는 정의하지 않는다. | `spec-silent` | ZW-E는 Spec 30 planned-maintenance relocation 시나리오가 아니다. stage-7이 host relocation까지 검증하려는지 명시적 adjudication이 필요하다. |
| ZW-36 | maintenance desired state는 같은 NodeId의 ZoneNode restart 뒤 복원된다 (`README.ko.md:500-501,628-632`). | Specs 30/31은 application NodeId별 desired-state store의 schema/TTL/reconcile를 정하지 않는다. | `spec-silent` | store truth, write/read failure, restart 시 reconcile ordering이 common contract에 없다. |
| ZW-37 | Connected는 complete runtime topology status/event에서, Registered는 ZoneNode explicit report에서 얻으며 polling하지 않는다 (`README.ko.md:69,470-475`). | Spec 24 §3 (`24-runtime-monitoring.ko.md:211-295`). | `compliant` | runtime event의 complete-status/sequence 경계를 올바르게 설명한다. Registered는 application report라는 점도 분리했다. |
| ZW-38 | shared live test는 crashed node의 `registered=false`가 “documented 30-second owner lease” 뒤 push된다고 요구한다 (`framework/languages/shared_sample/zoneworld/client/tests/live/server.spec.ts:51-66`). | Spec 24는 topology Connected 상태만 소유하고 application `Registered` report expiry를 정의하지 않음 (`24-runtime-monitoring.ko.md:211-295`). | `spec-silent` | crash process는 explicit false report를 보낼 수 없다. app report TTL/lease source와 `Registered` terminal semantics가 common README에 없다. |
| ZW-39 | process 시작 순서, 정상 replacement, crash replacement 모두에서 global ZoneId routing이 NodeId와 독립적으로 “동작”한다 (`README.ko.md:638-643`). | Spec 13은 replacement RID가 새 값임을 정함 (`13-mesh-node.ko.md:71-78`); Spec 31은 old Ready owner crash 후 object 자동 복원을 금지 (`31-failure-failover-policy.ko.md:106-123`). | `spec-silent` | “동작”이 새 node status/새 object 생성인지 old authority recovery인지 불명확하다. 후자라면 ZW-34와 충돌한다. |
| ZW-40 | move/join/diagnostics reply의 `error?: string` 및 `MoveRejectedNotify.reason`으로 failure를 관찰한다 (`README.ko.md:181-214` 및 Ops contract). | Spec 15 §4 failure kinds와 Spec 20 §3 terminal-once/no-retry는 Framework failure를 구분 (`15-spot-actor.ko.md:323-338`, `20-session-actor-dispatch.ko.md:75-82`). | `spec-silent` | `ZoneMaintenance` 외 Join 실패, session seal timeout, route failure를 어떤 typed app result/push/connection close로 관측할지 closed mapping이 없다. |
| ZW-41 | completion은 relocation/border/Ops/maintenance evidence와 `zoneworld=completed` marker로 판단한다 (`README.ko.md:645-682`). | Framework specs는 sample runner marker와 evidence aggregation을 정의하지 않는다. | `spec-silent` | application harness rule이다. 개별 observable failure mapping이 닫힌 뒤에는 유효한 criterion이 될 수 있다. |
| ZW-42 | `ZW-A..G` family가 공통 의도를 정하고 언어별 runner가 individual ID를 출력한다 (`README.ko.md:684-697`; EN `:707-720`). | Framework specs는 sample ID taxonomy를 정의하지 않는다. | `spec-silent` | common 문서는 individual ID의 precondition/action/assertion을 정의하지 않는다. .NET registry는 `ZW-G2`, Node intent는 `ZW-D2`처럼 의미 집합이 이미 갈릴 수 있다. 구현 coverage 수와 별개로 canonical step definition 부재가 문제다. |
| ZW-43 | RID는 `zn-<lowercase-canonical-uuid-v4>`, 고정 RID/`SetRoutingId` 없음, replacement마다 새 RID다 (`README.ko.md:638-640`). | Spec 13 §3.1 (`13-mesh-node.ko.md:54-78`). | `compliant` | exact format과 lifecycle rule이 맞다. |
| ZW-44 | global object routing은 ActorRef/OwnerNodeRid가 아니라 ActorId/SpotId를 사용하고 probes만 exact owner를 관찰한다 (`README.ko.md:146-149,616-618,642-643`). | Spec 18 §2.1, §2.4 (`18-object-routing.ko.md:70-93,134-189`). | `compliant` | probe와 application routing input을 분리한 설계는 맞다. ZW-06은 이를 깨는 별도 intent다. |
| ZW-45 | Logical Multicast/fanout publish terminal이나 subscriber handler completion을 client 성공으로 간주하지 않는다 (`README.ko.md:66,624-627`). | Spec 3 §5-6 (`03-interaction-model.ko.md:203-216,220-238`). | `compliant` | publish admission과 downstream processing 완료를 혼동하지 않는다. |
| ZW-46 | Ops→ZoneNodes→Gateway readiness, browser/headless, replacement probes, evidence, cleanup 순서로 smoke를 수행한다 (`README.ko.md:647-655`). | Framework specs는 sample orchestration/cleanup ordering을 정의하지 않는다. | `spec-silent` | harness 정책이며 자체 순서는 닫혀 있다. 단, “replacement”의 의미는 ZW-39를 먼저 결정해야 한다. |

## (a) 규격 조항 또는 scenario contract와 충돌하는 단계

### P0 — coordinator adjudication 전 stage-7 기준으로 사용할 수 없음

1. **Player-facing shared wire가 공통 wire와 직접 다르다 (ZW-32).**
   - Common ko/en: `JoinWorldRes`에 `nodeId`가 없고 `ZoneChangedNotify`는 `playerId/zoneId`만 가지며,
     physical relocation/owner를 노출하지 않는다 (`README.ko.md:181-218`, EN `:185-226`).
   - Shared browser: 두 message에 `nodeId`, 후자에 `transferred`를 추가하고 UI/E2E가 정확한
     `zone-node-2`와 “Actor transferred”를 요구한다 (`contracts.ts:29-54`, `server.spec.ts:16-19`).
   - 결정: common logical-only contract를 authority로 유지하고 shared fields/assertions를 제거할지,
     아니면 spec-compatible operational probe와 player contract를 분리해 common contract를 공식 변경할지
     먼저 정해야 한다. 현 상태에서 언어 서버가 어느 wire를 구현해도 다른 쪽을 위반한다.

2. **Zone→NodeId 고정 배치가 owner abstraction과 공통 금지 경계를 깬다 (ZW-06).**
   - Shared browser `nodeOf`, .NET `ZoneTopology.ZonesOf/NodeOf`, Node `nodeOf/zonesOf`, .NET regression
     test가 west/east owner를 고정한다.
   - Spec 18은 object ID를 parse해 node를 추론하지 않으며, Spec 14 placement는 Framework가 현재
     candidate에서 고른다. 공통 README도 NodeId owner 계산/custom owner selection을 금지한다.
   - 결정: cross-owner fixture는 NodeId hardcode가 아닌 규격상 허용된 deterministic fixture 방식으로
     별도 정의해야 한다. B-1과 함께 adjudicate해야 한다.

3. **초기 입장/zone join sequence가 `Defer()` ordering을 위반한다 (ZW-18).**
   - Diagram은 `EnterZoneRes`가 현재 JoinWorld request 안에서 반환된 뒤 `JoinWorldRes`가 성공하는 것처럼
     보인다. Spec 15는 handler가 끝난 뒤 Join이 시작되고 결과는 completion callback으로 온다고 규정한다.
   - 실제 .NET intent의 early cached precheck는 바로 이 ordering gap을 우회하려고 stale source state로
     `JoinWorldRes`를 결정한다. target admission이 나중에 실패할 수 있으므로 JoinWorld 성공의 의미가
     공통 문서와 일치하지 않는다.
   - 결정: (i) JoinWorld가 Actor Join completion 뒤 별도 client event로 확정되는 2단계 protocol인지,
     (ii) actor creation/first join과 client request의 책임을 다른 public flow로 분리할지 정해야 한다.

### P1 — 언어별 업무 결과가 이미 갈림

4. **Maintenance admission 책임이 두 지점에 있고 policy 값도 다르다 (ZW-21, ZW-22).**
   - Common: target `OnActorJoin`이 최종 판정, stale cache는 최종 판정자가 아니며 same-zone만 허용.
   - .NET: Entry Spot cache가 신규 entry를 먼저 terminal로 만들고, target은 payload의 source NodeId를
     다시 판정하며 same-node cross-zone까지 허용한다.
   - 결정: target-only admission과 정확한 예외를 하나의 canonical rule로 고정하고 source cache는
     terminal을 만들지 않는 관측/optimization으로 제한해야 한다.

5. **Session/WebSocket continuity를 무조건 보장한다 (ZW-31).**
   - Spec 20/28의 exact route update가 `SessionRelocationSealTimeout` 안에 오지 않으면 physical stream을
     닫는 실패 경로가 scenario와 success criteria에 없다.
   - 결정: 정상 path criterion은 “timeout 전에 exact update가 적용된 경우 connection 유지”로 한정하고,
     timeout case의 observable disconnect/rebind expectation을 별도 step으로 닫아야 한다.

## (b) spec-silent 또는 scenario가 닫지 않아 언어 divergence가 예상되는 항목

우선순위 순서다. 단순 geometry/bot rule처럼 common 문서가 값까지 닫은 application rule은 제외했다.

1. **B-1 / P0: 합법적인 deterministic cross-owner fixture가 없다 (ZW-07).** Spec placement는
   adjacent-owner spread를 보장하지 않는데 release gate는 그것을 필수로 요구한다. NodeId hardcode는
   (a)-2를 위반한다. capability/fixture/사전 생성 중 어떤 public 계약으로 pair를 보장할지 Claude
   adjudication과 필요 시 spec 구체화가 필요하다.
2. **B-2 / P1: stable type exact names와 zone type cardinality가 없다 (ZW-14).** actorJoin(28)은 stable
   type을 wire에 싣지 않고 canonical authority row로 해석하므로 모든 candidate language가 동일 stable
   name을 등록해야 한다. 공통 문서에 `zoneworld.zone`, `zoneworld.player` 같은 canonical names 또는
   language-neutral mapping을 고정해야 한다.
3. **B-3 / P1: crashed node의 `Registered=false` 규칙이 없다 (ZW-38).** explicit report owner가 죽은 뒤
   누가 언제 false를 합성하는지, 30초가 어떤 lease인지, 마지막 report를 유지하는지/expire하는지 정해야
   한다. Spec 24 Connected status로 Registered를 대신할 수 없다.
4. **B-4 / P1: ZW-E maintenance는 Spec 30 Host PlannedMaintenance가 아니다 (ZW-35).** 현재는 app
   admission desired state일 뿐 workload relocation이나 `SafeToShutdown`을 유발하지 않는다. stage-7이
   Spec 30/31 maintenance relocation을 검증하려는 경우 별도 Host `Relocate(PlannedMaintenance)` step과
   success/failure criteria가 필요하다. 그렇지 않다면 명시적으로 “Spec 30 비대상”이라 고정해야 한다.
5. **B-5 / P1: failure-to-wire mapping이 닫히지 않았다 (ZW-40).** `ZoneMaintenance` 외 Join failure,
   `Unavailable`, deadline, session seal timeout, malformed actorJoin reply가 각각 `JoinWorldRes.error`,
   `MoveRejectedNotify`, stream close 중 어디로 보이는지 정의해야 한다. generic string은 언어별 exception
   text divergence를 허용한다.
6. **B-6 / P1: individual `ZW-*` step 정의가 없다 (ZW-42).** family만으로는 precondition/action/assertion,
   terminal condition, probe timing을 동일하게 만들 수 없다. 공통 canonical table을 만든 뒤 언어별 subset
   coverage는 별도 implementation review에서 판단해야 한다.
7. **B-7 / P2: “crash replacement에서도 routing 동작” 의미가 모호하다 (ZW-39).** 새 lifecycle report/RID
   관찰인지, 새 object creation인지, old authority 복구인지 닫아야 한다. old Ready object 자동 복구라면
   Spec 31 및 scenario §7.5를 위반한다.
8. **B-8 / P2: c56714a52c actorJoin reply framing의 contract-test intent가 없다 (ZW-28).** 현재 prose는
   raw framing과 충돌하지 않으므로 violation은 아니다. 다만 cross-language test가 `EnterZoneRes`를 직접
   관찰한다면 command 20 reply의 정확히 한 multipart application part를 Framework가 unwrap한 typed reply로
   보며, nested envelope/inner bytes 노출을 금지한다는 intent를 canonical step에 넣어야 한다.
9. **B-9 / P2: maintenance restart store contract가 없다 (ZW-36).** key/version/TTL, write failure,
   fanout-before/after-store ordering, restart read failure의 observable result가 없어 언어마다 persistence 의미가
   달라질 수 있다.

## (c) stale clause references와 drifted terms

1. **Actor Join 근거가 Spec 15 §4.2 하나로만 연결된다.** `README.ko.md:148` / EN `:154`는 relocation
   unit의 owner 전환·relay·target queue·CAS 전체 근거처럼 Spec 15만 가리킨다. 현재 Spec 15
   `:422-425`는 이 전체 순서의 단일 authority가 Spec 28이고 Spec 15는 target admission/membership/
   lifecycle만 소유한다고 명시한다. “canonical 28-only relocation flow; 15 actor-specific join layer”로
   참조 역할을 분리해야 한다.
2. **“ZoneNode의 owner”와 “네 zone type”은 drifted term이다.** `README.ko.md:111-115` / EN
   `:115-120`. Owner는 Zone Spot/Actor 같은 object authority에 적용되고 ZoneNode 자체의 owner를 Location
   Store가 고르는 것이 아니다. 네 개는 ZoneId/Spot instance이며 stable type 수는 별도 계약이다.
3. **Shared contract 주석의 절 번호가 틀렸다.** `shared.../api/contracts.ts:1-4`는 browser wire를 scenario
   §7/§7.3이라고 하지만 현재 wire declaration은 §6/§6.3이고 §7은 flow다.
4. **.NET `Scenarios.cs` 주석의 다수 절 번호가 현재 문서와 맞지 않는다.** 예: `:204` §4.1,
   `:651` §2.6, `:690` §8.1, `:737` §8.2, `:777,1301` §8.4, `:1346` §2.7. 현재 README의
   해당 의미는 주로 §2, §7, §9에 있다. 주석은 normative link로 사용할 수 없다.
5. **“documented 30-second owner lease”는 공통 scenario에 없다.** `framework/languages/shared_sample/zoneworld/client/tests/live/server.spec.ts:63-66`.
   Spec 18의 30초 default는 Message Follow duration (`18-object-routing.ko.md:146-149`)이지 application
   Registered report lease가 아니다.
6. **“crash replacement” 용어는 failover가 아니라는 경계와 함께 제한해야 한다.** `README.ko.md:640,653`
   / EN `:657-670`. 새 process/new RID 관찰을 뜻하는지 old object replacement를 뜻하는지 용어만으로는
   구분되지 않는다.

## ko/en 문서 상호 비교

- `README.ko.md`와 `README.en.md`의 section 구성, topology, resource/role tables, message field 집합,
  sequence 단계, self-check 순서, completion criteria, ZW family 의미를 대조했다.
- **ko/en 사이의 독립적인 의미 divergence는 발견하지 못했다.** 위의 공통 문서 문제는 두 언어에
  대칭으로 존재한다: 무조건적인 connection continuity, `ZoneNode owner`/“four zone types” 용어,
  Actor Join ordering 누락, family-only scenario IDs, Spec 15만 가리키는 Actor Join link가 모두 같다.
- 실제 divergence는 ko↔en이 아니라 **common ko/en ↔ shared browser / language step intent** 사이에 있다.

## Coordinator 결정 요청 요약

Stage-7 구현을 시작하기 전에 최소 다음 결정을 하나의 canonical scenario에 반영해야 한다.

1. Player wire는 logical-only인가, physical node/transfer를 공개하는가.
2. NodeId/owner를 application input으로 쓰지 않으면서 cross-owner adjacent pair를 어떻게 확정하는가.
3. JoinWorld 성공은 deferred Actor Join의 어느 terminal 이후인가.
4. Maintenance의 유일한 admission owner와 허용 범위는 same-zone인가 same-node인가.
5. Session route-update timeout 시 client가 관찰할 exact result는 무엇인가.
6. `Registered` explicit report는 crash 뒤 어떤 규칙으로 expire되는가.
7. ZW-E가 app admission-only인지 Spec 30 planned host relocation까지 포함하는가.
8. Canonical stable types, individual `ZW-*` steps, typed failure mapping을 무엇으로 고정하는가.

이 결정 전에는 모든 언어가 같은 stage-7 시나리오를 구현했다는 단일 판정이 불가능하다.
