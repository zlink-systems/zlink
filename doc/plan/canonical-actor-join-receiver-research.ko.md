# canonical actorJoin(28) 수신자 type-resolution 리서치 (H-12/H-15 스펙 결정 근거)

작성 2026-08-20 (codex terra high, read-only). Claude 검토. **판정: 옵션 A(Authority Store lookup) 채택 방향.**

---

결론: canonical `actorJoin(28)` body만으로는 target이 Actor factory를 고를 stable type을 결정할 수 없습니다. 하지만 canonical Authority Store record에는 `allocation.stableType`이 있으므로, actorId와 body의 exact fence를 함께 검증하는 Store lookup은 가능합니다. 현재 스펙은 그 lookup을 수신 의무로 규정하지 않고, 현 구현들은 대부분 사설 상위 패킷의 `actorType`에 의존합니다. 따라서 H-12/H-15의 본질은 “Store에 type이 없는 문제”가 아니라 “28 수신자가 type을 어디서, 어떤 fence로 얻어야 하는지 계약이 비어 있는 문제”입니다.

## 1. 현재 4언어 수신자의 stable type 출처

| 언어 | 실제 상위 actor-join 수신 경로 | type 출처 | factory/materialize 사용 |
|---|---|---|---|
| C++ | 사설 typed-JSON `__zlink.spot.actor.join.admission` | 송신 사설 packet의 `actor_type`; canonical 28 수신은 별도로 target-local `actor_types_by_id`만 조회 | 사설 경로는 request의 type으로 `actor_ref`/factory를 구성. canonical 28은 target이 그 actorId를 전에 알고 있지 않으면 즉시 reject |
| .NET | 사설 envelope `__zlink.actor.join_spot.{request,admission,commit}` | `ZLinkRemoteActorJoinRequest.ActorType` | target의 relocation registry 및 `RelocateAndBindActorAsync`에 `request.ActorType` 전달 |
| Java | 사설 `__zlink.actor.joinSpot` transfer packet | newline-delimited `TransferRequest.actorType` 필드 | admission prewarm이 `actorType`으로 factory type을 resolve |
| Node | 사설 JSON `__zlink.actor.join_spot.request` | JSON `actorType` | receiver가 `getOrCreateActor(actorId, actorType)` 호출; routed commit도 `actorType`을 materializer에 전달 |

중요한 정정: 네 상위 경로가 모두 사설인 것은 맞지만, 모두 JSON은 아닙니다. C++는 typed JSON serializer, .NET/Node는 JSON 계열 envelope, Java는 newline-delimited private codec입니다.

C++ canonical 수신은 특히 명확합니다. [`admit_wire_actor_join`]( /home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp:410 )이 `spot.resolve_actor_type(actorId)`를 호출하고, 없으면 reject합니다. 그 resolver는 Store가 아니라 [`actor_types_by_id` 로컬 map]( /home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:5388 )만 읽습니다. 주석도 “wire body에는 stable type이 없고, 이 노드가 prior create/join으로 identity를 이미 알아야 한다”고 명시합니다.

반면 C++ 기존 사설 admission request는 [`transfer_id`, `actor_type`, `actor_id` 등을 직접 전송]( /home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/spot_route_packets.hpp:28 )하고, source도 그 값을 채웁니다([`mesh_node_runtime.cpp`]( /home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp:2349 )).

.NET은 [`ZLinkRemoteActorJoinRequest.ActorType`]( /home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Actors/ZLinkRemoteActorJoinPackets.cs:639 )를 decode한 뒤, target에서 registry lookup과 materialization에 그대로 씁니다([`ZLinkFrameworkRuntimeActors.cs`]( /home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Host/ZLinkFrameworkRuntimeActors.cs:551 ), [`:593`]( /home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Host/ZLinkFrameworkRuntimeActors.cs:593 )).

Java는 packet encoder가 `actorType`을 field 4에 넣고([`ZLinkActorSpotRoutePackets.java`]( /home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkActorSpotRoutePackets.java:191 )), decoder가 이를 `TransferRequest.actorType`으로 복원합니다([`:238`]( /home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkActorSpotRoutePackets.java:238 )). 수신 admission은 이 값을 prewarm/factory resolution에 사용합니다([`ZLinkActorSpotAdmission.java`]( /home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/spots/ZLinkActorSpotAdmission.java:385 )).

Node는 JSON payload의 `actorType`을 필수로 decode하고([`actor-remote-wire.ts`]( /home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/actors/actor-remote-wire.ts:240 )), receiver가 곧바로 `getOrCreateActor(join.actorId, join.actorType)`를 호출합니다([`remote-actor-join-receiver.ts`]( /home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/host/remote-actor-join-receiver.ts:33 )).

## 2. Store descriptor/authority에는 stable type이 있는가?

있습니다. 다만 “MeshNode descriptor의 capability 목록”과 “특정 Actor authority row”를 구별해야 합니다.

- MeshNode descriptor의 `objectCapabilities`는 node가 지원하는 `(objectKind, stableType)` 집합입니다. 특정 actor의 type을 말하지 않습니다. [§2.4]( /home/hep7/project/zlink/framework/doc/framework/common/spec/server/21-location-runtime.en.md:448 )
- 특정 actor의 Authority row는 `allocation.objectKind = actor`와 `allocation.stableType`을 가집니다. [§2.4]( /home/hep7/project/zlink/framework/doc/framework/common/spec/server/21-location-runtime.en.md:506 )
- canonical key는 `authority\0actor\0{ActorId}`입니다. 그러므로 actorId로 해당 Actor authority row를 읽는 것은 가능합니다. [§2.4]( /home/hep7/project/zlink/framework/doc/framework/common/spec/server/21-location-runtime.en.md:401 )
- golden fixture도 actor authority에 `stableType: "chat"`를 고정합니다. [store-record-v1.json]( /home/hep7/project/zlink/framework/runtime/protocol/golden/store-record-v1.json:243 )

따라서 “actorId만으로 lookup key를 만들 수 있는가?”에는 예입니다. 그러나 안전한 type resolution은 actorId만으로 끝나면 안 됩니다. 수신자는 body의 actor fence와 Store row의 다음을 함께 대조해야 합니다.

- `allocation.state == active`, `allocation.objectKind == actor`
- `objectGeneration`
- owner node RID / descriptor lifecycle generation
- `authorityOwnerGeneration`
- `ownerLeaseGeneration`
- 그 뒤 `allocation.stableType`으로 local factory registry를 찾기

C++에도 Authority에서 type을 얻는 helper가 이미 있습니다. [`actor_type_from_authority`]( /home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:648 )는 actor authority를 읽어 type을 복원합니다. 하지만 canonical 28 admission의 `resolve_actor_type`에는 연결되어 있지 않습니다.

## 3. Session-owner lease resolver는 canonical 28 수신에 필수인가?

“bound Session이 있어야만 canonical actorJoin을 받을 수 있다”는 뜻으로는 아닙니다. 이는 C++의 현재 wiring 제약입니다.

schema상 actor route fence에는 항상 non-zero `expectedOwnerLeaseGeneration`이 있습니다. [schema]( /home/hep7/project/zlink/framework/runtime/protocol/service-wire-v1.schema.json:3872 ) 따라서 source는 unbound Actor라도 자기 현재 owner lease를 넣어야 합니다. C++의 canonical source prototype은 이 값을 `_session_route_owner_resolver()`에서 가져오며, 없으면 `Unavailable`로 끝냅니다. [C++ source prototype]( /home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp:2480 )

하지만 그 resolver는 실제로 app startup에서 `location_runtime.current_owner_token()`으로 배선됩니다. 즉 이름은 session-route owner resolver지만, 여기서 쓴 값은 “bound Session 존재 여부”가 아니라 source host의 Location owner lease입니다. [wiring]( /home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/host/app.cpp:1405 )

더 결정적으로 C++의 실제 canonical 28 target ingress는 source peer RID/generation과 target spot RID/generation만 검사합니다. session-owner resolver를 요구하지 않습니다. [`raw_mesh_node_owner.cpp`]( /home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:3081 ) 이후 `admit_wire_actor_join`도 local type/spot admission만 수행합니다. 따라서 H-12 주석의 “unbound에도 resolver 필요”는 현재 C++ source prototype의 configuration dependency이지, 28 receiver의 보편적 Session requirement는 아닙니다.

Java/Node의 현 사설 unbound join은 Session 값을 optional로 취급하며, Node receiver도 owner lease resolver를 읽지 않습니다. .NET도 request의 bound-session coordinates를 nullable로 둡니다. 즉 네 언어 공통 설계 규칙으로 확인되지는 않습니다. bound Session일 때만 seal/route update가 추가되는 것은 스펙과 일치합니다. [§15]( /home/hep7/project/zlink/framework/doc/framework/common/spec/server/15-spot-actor.en.md:528 )

## 4. relocation(40/52)이 이 문제를 피하는 방식

relocation이 green인 사실은 canonical 28의 type-resolution 해법을 증명하지 않습니다. 둘은 type 전달/복원 지점이 다릅니다.

`relocationPrepare(40)`와 `relocationState(52)`의 canonical object identity에서 Actor case는 `actor-ref + expectedAuthorityOwnerGeneration`만 담고 stable type을 담지 않습니다. [schema]( /home/hep7/project/zlink/framework/runtime/protocol/service-wire-v1.schema.json:4058 ) `relocationPrepare(40)`도 그 object identity를 참조할 뿐입니다. [schema]( /home/hep7/project/zlink/framework/runtime/protocol/service-wire-v1.schema.json:7605 )

현재 relocation은 다음의 조합으로 type을 확보합니다.

- 상위 relocation/join coordinator는 source local Actor state 또는 사설 handoff packet의 `actorType`을 이미 갖고 있습니다.
- target factory/adapter suitability는 target descriptor capability와 authority identity로 검증합니다.
- canonical 40/52 target은 Actor stable type이 wire object에 없음을 전제로 Authority-derived participant identity를 사용합니다. C++ 구현 주석도 Actor/User Spot의 type은 wire object에서 의도적으로 생략되며 Authority row에서 보충한다고 명시합니다. [`public_host_runtime.cpp`]( /home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp:3776 )
- .NET은 사설 `ZLinkRemoteActorJoinRequest.ActorType`을 materializer에 전달하면서 authority `Allocation.StableType`과 대조합니다. [authority check]( /home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Host/ZLinkFrameworkRuntimeActors.cs:2415 )
- Java source builder는 Authority payload의 stable type과 local registry type이 일치하는지 확인합니다. [`ZLinkStandaloneActorRelocationSourceBuilder.java`]( /home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/spots/ZLinkStandaloneActorRelocationSourceBuilder.java:381 )
- Node도 relocation authority의 `allocation.stableType`을 target commit fence에 포함해 검증합니다. [`actor-transfer-runtime.ts`]( /home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/host/actor-transfer-runtime.ts:1216 )

즉 relocation은 “type 없는 40/52 actor identity”를 Store-backed established authority와 coordinator-local state로 보완합니다. 반면 28 admission은 relocation prepare 전의 첫 target preparation 단계라, 그 source of truth와 exact failure semantics를 스펙이 아직 명시하지 않았습니다.

## 5. 스펙 판정

명문 사실은 다음뿐입니다.

- 51 §9는 28 body가 complete contract이고 다른 field, 특히 transfer id를 실을 수 없다고 규정합니다. [§51]( /home/hep7/project/zlink/framework/doc/framework/common/spec/server/51-internal-service-wire-protocol.en.md:510 )
- 15 §4.2는 Accepted 전에 target이 temporary queue와 factory execution을 준비해야 한다고 요구합니다. type을 어디서 얻는지는 쓰지 않습니다. [§15]( /home/hep7/project/zlink/framework/doc/framework/common/spec/server/15-spot-actor.en.md:478 )
- 28은 target이 factory/restore를 수행한다고 정하고, 30은 target selection 단계에서 stable type/factory capability를 확인한다고 정합니다. 둘 다 canonical 28 receiver의 type lookup algorithm은 규정하지 않습니다. [§28]( /home/hep7/project/zlink/framework/doc/framework/common/spec/server/28-relocation-flow.en.md:35 ), [§30]( /home/hep7/project/zlink/framework/doc/framework/common/spec/server/30-host-relocation-flow.en.md:286 )
- 52는 internal design이며, handoff-owned values에 stable type을 포함하지 않습니다. [§52]( /home/hep7/project/zlink/framework/doc/framework/common/spec/server/52-internal-relocation-handoff.en.md:56 )

따라서 “28 body는 complete”와 “target은 factory를 prepare” 사이의 stable-type resolution은 현재 명문 공백입니다. C++ local-map reject는 그 공백을 좁은 방식으로 해석한 구현일 뿐, 4언어 공통 규범으로 볼 수 없습니다.

## 6. 대안과 영향

| 옵션 | 내용 | 장점 | 4언어 영향 / 필요한 스펙 변경 |
|---|---|---|---|
| A. Authority Store lookup | target이 `authority(actorId)`를 읽고 actor fence와 exact match한 후 `allocation.stableType`으로 factory를 resolve | 28 schema를 유지, Store가 이미 가진 canonical per-Actor truth 사용 | C++는 synchronous local-map admission을 async Store-backed admission으로 바꾸어야 함. .NET/Java/Node는 canonical 28 receiver를 만들 때 같은 validation을 추가. §51에 lookup/fence/error behavior, §15에 prewarm type source를 명시해야 함. schema 변경은 불필요 |
| B. canonical 28에 `stableType` 추가 | Actor route fence 또는 28 body에 stable type을 추가 | target이 Store read 없이 prewarm/factory lookup 가능, C++ 경로가 단순 | service-wire schema, generated codecs/golden fixtures, §51 complete-body 규정, 4언어 encoder/decoder 모두 변경 필요. type은 sender 주장일 뿐이므로 Store가 있으면 Authority `stableType`과 equality 검증도 필요 |
| C. 별도 canonical admission/materialization record | 28 correlation/actor identity에 결합된 durable type record를 먼저 publish/read | type과 factory policy를 explicit하게 분리 가능 | 사실상 Authority row를 중복하는 새 distributed truth가 됨. lifecycle, CAS ownership, expiry, replay, cleanup을 새로 정의해야 하므로 가장 큰 스펙·4언어 비용. factory registry만으로 type을 추론하는 방식은 multi-type node에서 안전하지 않아 대안이 될 수 없음 |

판정상 가장 자연스러운 방향은 A입니다. canonical Store는 이미 per-Actor stable type과 모든 필요한 owner fence를 보유하고 있고, relocation target도 같은 Authority-derived identity를 사용합니다. 다만 이는 “기존 Store에 field가 있으니 구현만 연결”로 끝낼 수 없습니다. 28의 complete-body 원칙을 유지하려면, Store read의 필수성·정확 대조 항목·Store unavailable/stale/mismatch의 terminal 분류를 스펙에 먼저 추가해야 합니다.

이번 작업은 읽기 전용으로 수행했으며 파일 수정·커밋·테스트 실행은 하지 않았습니다.


