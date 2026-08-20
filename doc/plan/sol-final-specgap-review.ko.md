# sol 전 문서 spec-gap 리뷰 결과 (체크리스트 항목 174, 완료 조건)

작성 2026-08-20 (codex sol, 정적 read-only, 기준 543a5c32c1). Claude 검토·트랙 배정.

---

## 판정

**NOT-CLEAN — 13개 gap: C 1 / H 7 / M 3 / L 2.**

최종 기준은 `main`의 `543a5c32c1e4299780d2782cc46ce5113bd921a4`와 현재 worktree입니다. 검토 도중 HEAD가 변경되어 핵심 발견을 최종 HEAD에서 다시 확인했습니다.

### 발견

1. **[심각도 C] C++ canonical actorJoin(28)이 admission Accepted를 relocation 완료로 잘못 보고한다.**

   - 근거: Spec 15 §4.2는 admission Accepted 뒤 source seal/capture와 Restore가 계속되어야 한다고 규정합니다([15-spot-actor.en.md:463](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/15-spot-actor.en.md:463), [15-spot-actor.en.md:494](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/15-spot-actor.en.md:494)). Spec 28은 command 40/52 전송, relay-ready, cutover, target CAS·queue open을 후속 단계로 규정합니다([28-relocation-flow.en.md:128](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/28-relocation-flow.en.md:128), [28-relocation-flow.en.md:205](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/28-relocation-flow.en.md:205), [28-relocation-flow.en.md:296](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/28-relocation-flow.en.md:296)).
   - Gap: C++는 canonical 경로를 선택한 뒤([mesh_node_runtime.cpp:2251](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp:2251)), Accepted reply를 즉시 `actor_join_reply_t`로 바꾸고 public completion callback을 실행합니다([mesh_node_runtime.cpp:2562](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp:2562)). 실제 seal/capture 후속 함수는 [mesh_node_runtime.cpp:2593](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp:2593)부터 있으나 canonical 경로에서는 호출되지 않고, 사설 JSON 경로만 이를 호출합니다([mesh_node_runtime.cpp:2388](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp:2388)).
   - 영향: target은 임시 queue와 admission 준비만 끝낸 상태인데 caller가 Accepted를 받고, source ownership·state·queue가 실제로 이동하지 않을 수 있습니다.
   - 제안: command 28 Accepted를 seal/capture/Restore/relay/cutover 파이프라인으로 연결하고, public Accepted는 target owner CAS와 queue-open 계약이 끝난 뒤에만 전달해야 합니다.

2. **[심각도 H] C++가 target의 negotiated receive chunk limit을 기록만 하고 실제 전송에 적용하지 않는다.**

   - 근거: Spec 28 §4.2는 실제 chunk 크기를 server limit, target 광고 limit, in-flight budget의 최솟값으로 정합니다([28-relocation-flow.en.md:137](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/28-relocation-flow.en.md:137)).
   - Gap: reply 값을 map에 기록하지만([mesh_node_runtime.cpp:2562](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp:2562)), 주석도 실제 consumer 연결이 deferred 상태라고 명시합니다([mesh_node_runtime.cpp:2565](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp:2565)). getter([mesh_node_runtime.cpp:2451](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp:2451))는 production transfer 계획에서 소비되지 않습니다.
   - 제안: actor/relocation-attempt identity별로 값을 한 번 소비하여 direct-transfer chunk planner에 전달하고, 세 상한의 `min`을 적용하십시오. high/low advertised limit interop vector도 필요합니다.

3. **[심각도 H] 네 언어의 실제 Actor Join 상위 경로에 서로 다른 사설 wire dialect가 남아 있다.**

   - 근거: Spec 51 §1은 schema를 유일한 wire authority로 두고 local compatibility encoding을 금지합니다([51-internal-service-wire-protocol.en.md:43](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/51-internal-service-wire-protocol.en.md:43)). §9는 command 28의 correlation·actor fence·entry·target Spot fence가 완전한 body이며 transfer ID 등은 wire에 실을 수 없다고 규정합니다([51-internal-service-wire-protocol.en.md:512](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/51-internal-service-wire-protocol.en.md:512)).
   - Gap:
     - C++ fallback은 `ActorTransferAdmission` JSON envelope에 transfer ID와 completion ID 등을 보냅니다([mesh_node_runtime.cpp:2323](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp:2323)).
     - .NET public Framework Join은 `__zlink.actor.join_spot.*` JSON envelope를 사용합니다([ZLinkRemoteActorJoinPackets.cs:3](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Actors/ZLinkRemoteActorJoinPackets.cs:3), [ZLinkRemoteActorJoinPackets.cs:183](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Actors/ZLinkRemoteActorJoinPackets.cs:183), [ZLinkActorRemoteJoiner.cs:451](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Host/ZLinkActorRemoteJoiner.cs:451)).
     - Java는 `__zlink.actor.joinSpot` multipart와 admission/commit phase를 사용합니다([ZLinkActorSpotRoutePackets.java:20](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkActorSpotRoutePackets.java:20)).
     - Node는 `__zlink.actor.join_spot.request` JSON body에 phase·transferId·언어별 필드를 둡니다([actor-remote-wire.ts:9](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/actors/actor-remote-wire.ts:9), [actor-remote-wire.ts:50](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/actors/actor-remote-wire.ts:50)).
   - 제안: cross-node Actor Join은 command 28과 40/52/cutover 계약만 사용하고, transfer bookkeeping은 runtime-local adapter 뒤에 숨기십시오.

4. **[심각도 H] Java가 full-range u64 opaque token의 high-bit 값을 거부한다.**

   - 근거: LifecycleGeneration은 non-zero opaque equality token이며 숫자 크기로 순서를 판정하지 않습니다([01-glossary.en.md:1512](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/01-glossary.en.md:1512)). Spec 51 §12도 lifecycle token의 numeric-order 비교를 금지합니다([51-internal-service-wire-protocol.en.md:740](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/51-internal-service-wire-protocol.en.md:740)).
   - Gap: Java decoder는 u64 raw bits를 signed `long`에 보존하고 `== 0`으로 검사할 수 있게 구현했지만([ZLinkServiceM6AWireCodec.java:719](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceM6AWireCodec.java:719)), 이후 correlation을 `<= 0`으로 재검사합니다([ZLinkServiceM6AWireCodec.java:448](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceM6AWireCodec.java:448)). 같은 잔재가 Spot/Actor correlation([ZLinkServiceM6BWireCodec.java:23](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceM6BWireCodec.java:23), [ZLinkServiceM6BWireCodec.java:135](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceM6BWireCodec.java:135)), replyRouteId([ZLinkServiceFrozenRecordCodec.java:201](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceFrozenRecordCodec.java:201)), Message Follow target generation writer([ZLinkServiceMessageFollowWireCodec.java:200](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceMessageFollowWireCodec.java:200))에 남아 있습니다.
   - 영향: `0x8000_0000_0000_0000` 이상인 정상 .NET/C++/Node 토큰이 Java에서 protocol error가 됩니다.
   - 제안: opaque u64 helper는 `value == 0`만 거부하고, `<= 0`은 deadline, revision, bounded counter에만 사용하십시오. 모든 관련 codec에 high-bit golden vector를 추가해야 합니다.

5. **[심각도 H] W-2 생성 codec이 production byte layer로 교체되지 않았다.**

   - 근거: Spec 51은 각 언어 codec/constants가 schema에서 생성되어야 한다고 규정합니다([51-internal-service-wire-protocol.en.md:43](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/51-internal-service-wire-protocol.en.md:43)). Adoption 문서는 W-2가 runtime actorJoin byte layer를 생성 module로 교체한다고 명시합니다([W-1-codec-generator-adoption.md:3](/home/hep7/project/zlink/framework/runtime/protocol/W-1-codec-generator-adoption.md:3)).
   - Gap: generator는 최근 9개 command 5/6/16/17/18/19/23/26/27을 생성합니다([generate-service-wire-pilot-codecs.mjs:28](/home/hep7/project/zlink/framework/runtime/protocol/generate-service-wire-pilot-codecs.mjs:28)), 생성 Java surface도 존재합니다([ServiceWirePilotCodec.java:16](/home/hep7/project/zlink/framework/runtime/protocol/generated/jvm/ServiceWirePilotCodec.java:16)). 그러나 production source에는 generated codec 참조가 없고, .NET은 unit-test project에서만 포함합니다([Zlink.Framework.UnitTests.csproj:173](/home/hep7/project/zlink/framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Zlink.Framework.UnitTests.csproj:173)). 실제 runtime은 계속 handwritten codec을 호출합니다([ZLinkServiceWireCodec.cs:1170](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkServiceWireCodec.cs:1170), [service-stateful-wire-codec.ts:624](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/foundation/service-stateful-wire-codec.ts:624)).
   - 제안: generated type을 internal adapter 뒤에서 production encode/decode에 연결하고 handwritten byte layout을 제거하십시오. 교체가 W-3이라면 Spec 51과 adoption 문서의 W-2/W-3 완료 정의도 함께 정정해야 합니다.

6. **[심각도 H] Config 10 ST-C4의 checksum-mismatch actual-process 계약이 미완료다.**

   - 근거: ST-C4는 checksum mismatch와 exact-identity conflict 두 variant 모두를 contracted fault injection과 public terminal/source restoration으로 검증해야 합니다([config-10-spot-actor-relocation.en.md:234](/home/hep7/project/zlink/framework/doc/framework/common/e2e/config-10-spot-actor-relocation.en.md:234)).
   - Gap: C++ feature map은 checksum variant를 role-server E2E로 구현하지 않고 unit test로 대체했다고 명시합니다([C++ feature-map.ko.md:25](/home/hep7/project/zlink/framework/languages/cpp/e2e/SpotActorTransfer/feature-map.ko.md:25)). .NET도 selector 부재를 명시합니다([.NET feature-map.ko.md:17](/home/hep7/project/zlink/framework/languages/dotnet/e2e/SpotActorTransfer/feature-map.ko.md:17)).
   - 제안: 단일 relocation에만 적용되는 one-shot chunk-corruption seam을 process harness에 제공하고, explicit terminal 1회·retry 없음·target restore 없음·source state 보존을 public evidence로 판정하십시오.

7. **[심각도 H] Config 10 전체 completion gate가 네 언어 모두 미완료다.**

   - 근거: 공통 계약은 G1–G6, H1–H5/H4A/H4B, I1–I6을 정의합니다([config-10-spot-actor-relocation.en.md:523](/home/hep7/project/zlink/framework/doc/framework/common/e2e/config-10-spot-actor-relocation.en.md:523), [config-10-spot-actor-relocation.en.md:647](/home/hep7/project/zlink/framework/doc/framework/common/e2e/config-10-spot-actor-relocation.en.md:647), [config-10-spot-actor-relocation.en.md:770](/home/hep7/project/zlink/framework/doc/framework/common/e2e/config-10-spot-actor-relocation.en.md:770)).
   - Gap: C++ client dispatcher는 F6까지만 실행합니다([Client/main.cpp:62](/home/hep7/project/zlink/framework/languages/cpp/e2e/SpotActorTransfer/Client/main.cpp:62)); feature map도 G/I 전부와 H 다수를 미구현으로 기록합니다([C++ feature-map.ko.md:38](/home/hep7/project/zlink/framework/languages/cpp/e2e/SpotActorTransfer/feature-map.ko.md:38)). .NET([feature-map.ko.md:32](/home/hep7/project/zlink/framework/languages/dotnet/e2e/SpotActorTransfer/feature-map.ko.md:32)), Java([feature-map.ko.md:49](/home/hep7/project/zlink/framework/languages/java/e2e/SpotActorTransfer/feature-map.ko.md:49)), Node([feature-map.ko.md:31](/home/hep7/project/zlink/framework/languages/node/e2e/SpotActorTransfer/feature-map.ko.md:31))에도 미구현·부분·diagnostic-only 항목이 남아 있습니다.
   - 제안: canonical scenario ID별 selector와 aggregate gate를 일치시키고, component test나 diagnostic run을 P0 actual-process 완료로 계산하지 마십시오.

8. **[심각도 H] Config 6 Store Failure/Recovery completion gate도 네 언어 모두 미완료다.**

   - 근거: 공통 계약은 SF-A1부터 SF-G2까지 28개 scenario를 정의하며, Track F에는 cross-language state, direct-transfer failure, recovery, chunk boundary가 포함됩니다([config-6-store-failure-recovery.en.md:338](/home/hep7/project/zlink/framework/doc/framework/common/e2e/config-6-store-failure-recovery.en.md:338), [config-6-store-failure-recovery.en.md:559](/home/hep7/project/zlink/framework/doc/framework/common/e2e/config-6-store-failure-recovery.en.md:559)).
   - Gap: C++ aggregate는 14개만 실행합니다([run_e2e.sh:41](/home/hep7/project/zlink/framework/languages/cpp/e2e/DiscoveryRegistryHa/run_e2e.sh:41), [run_e2e.sh:68](/home/hep7/project/zlink/framework/languages/cpp/e2e/DiscoveryRegistryHa/run_e2e.sh:68)). .NET은 B3/C3–C5A/F1–F11/G1–G2를 미구현으로 기록합니다([feature-map.ko.md:24](/home/hep7/project/zlink/framework/languages/dotnet/e2e/StoreFailure/feature-map.ko.md:24)). Java는 cross-language·relocation Track F와 capacity fixture 부재를 기록합니다([feature-map.ko.md:78](/home/hep7/project/zlink/framework/languages/java/e2e/StoreFailure/feature-map.ko.md:78)). Node도 F1/F2/F3/F5/F8/F10/F11이 없습니다([feature-map.ko.md:22](/home/hep7/project/zlink/framework/languages/node/e2e/DiscoveryRegistryHa/feature-map.ko.md:22)).
   - 제안: 우선 P0 selector를 모두 실제 process로 연결하고, SF-F1은 언어 방향별 mixed-language matrix로 실행하십시오.

9. **[심각도 M] Node exact-interface가 private로 규정한 Authority/domain DTO를 public package가 export한다.**

   - 근거: governance는 내부 storage row/state command가 package 내부에 남아야 한다고 규정합니다([00-public-contract-governance.en.md:174](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/00-public-contract-governance.en.md:174)). Node exact interface는 Authority·reservation·capacity·fence DTO와 domain operation을 명시적으로 private로 분류합니다([08-location-maintenance.en.md:302](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/languages/node/interfaces/08-location-maintenance.en.md:302)).
   - Gap: `Locations/index.ts`가 `Authority` 전체를 export하고([Locations/index.ts:49](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/contracts/Locations/index.ts:49)), 그 파일은 authority key/snapshot/mutation/CAS/capacity DTO를 public 선언합니다([Authority.ts:9](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/contracts/Locations/Authority.ts:9)). 최상위 package도 contracts를 재-export합니다([src/index.ts:1](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/index.ts:1)).
   - 제안: public barrel export에서 domain DTO를 제거해 runtime-internal로 이동하십시오. 외부 provider SPI로 의도했다면 네 언어 exact interface와 governance 변경이 선행되어야 합니다.

10. **[심각도 M] Spec 51과 C++ 주석의 command 28 originator 상태가 현재 구현과 어긋난다.**

   - Gap: Spec 51은 C++/.NET이 live cross-node actorJoin을 originate하지 않는다고 설명합니다([51-internal-service-wire-protocol.en.md:600](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/51-internal-service-wire-protocol.en.md:600)). 그러나 C++는 authority를 관측하고 canonical 경로를 호출합니다([mesh_node_runtime.cpp:2251](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp:2251)). .NET lower-level runtime도 command 28을 encode/send합니다([ZLinkManagedMeshNode.cs:3074](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:3074)). C++ header와 함수 주석은 여전히 “nothing calls this yet”라고 말합니다([mesh_node_runtime.hpp:443](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.hpp:443), [mesh_node_runtime.cpp:2469](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp:2469)).
   - 제안: live originator 표와 주석을 현재 상태로 수정하되, C++ source continuation gap이 해결되기 전에는 “지원 완료”라고 기록하지 마십시오.

11. **[심각도 M] E2E 문서에 제거된 ACK/retry 용어가 남아 있다.**

   - 근거: ST-E1C는 command 44가 one-way이고 ACK·internal retry 단언이 없다고 규정합니다([config-10-spot-actor-relocation.en.md:328](/home/hep7/project/zlink/framework/doc/framework/common/e2e/config-10-spot-actor-relocation.en.md:328)). Glossary도 reply가 없고 late update를 무시한다고 규정합니다([01-glossary.en.md:2200](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/01-glossary.en.md:2200)).
   - Gap: .NET feature map은 “Session location update retry” 검증을 요구합니다([feature-map.ko.md:21](/home/hep7/project/zlink/framework/languages/dotnet/e2e/SpotActorTransfer/feature-map.ko.md:21)). Java는 `commit ack`([feature-map.ko.md:30](/home/hep7/project/zlink/framework/languages/java/e2e/SpotActorTransfer/feature-map.ko.md:30)), Node는 `commit ACK`와 `durable commit ACK`를 성공 증거로 기술합니다([feature-map.ko.md:12](/home/hep7/project/zlink/framework/languages/node/e2e/SpotActorTransfer/feature-map.ko.md:12), [feature-map.ko.md:21](/home/hep7/project/zlink/framework/languages/node/e2e/SpotActorTransfer/feature-map.ko.md:21)).
   - 제안: target `location_committed`, relay-ready, one-way route update, seal-timeout·late-no-op 관측으로 용어와 판정을 교체하십시오.

12. **[심각도 L] C++ Observability feature map이 존재하지 않는 `phase=error`를 완료 증거로 기술한다.**

   - 근거: Spec 26의 phase 집합에는 error가 없고([26-message-flow-tracing.en.md:47](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/26-message-flow-tracing.en.md:47)), dispatch error는 `outcome=failed`를 사용합니다([26-message-flow-tracing.en.md:144](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/26-message-flow-tracing.en.md:144)).
   - Gap: C++ feature map은 OBS-A2가 `phase=error`를 대조한다고 기술합니다([feature-map.ko.md:14](/home/hep7/project/zlink/framework/languages/cpp/e2e/ObservabilityOps/feature-map.ko.md:14)). 실제 scenario assertion은 이미 `outcome=failed`를 검사하므로([obs_a2_scenario.hpp:10](/home/hep7/project/zlink/framework/languages/cpp/e2e/ObservabilityOps/Client/Scenarios/obs_a2_scenario.hpp:10)) 문서만 stale합니다.
   - 제안: `dispatch_error, outcome=failed`로 고치고 phase가 없음을 명시하십시오.

13. **[심각도 L] Java StoreFailure 문서가 공통 계약에 없는 SF-G3을 invent한다.**

   - 근거: Config 6 Track G는 SF-G1과 SF-G2로 끝납니다([config-6-store-failure-recovery.en.md:559](/home/hep7/project/zlink/framework/doc/framework/common/e2e/config-6-store-failure-recovery.en.md:559), [config-6-store-failure-recovery.en.md:578](/home/hep7/project/zlink/framework/doc/framework/common/e2e/config-6-store-failure-recovery.en.md:578)).
   - Gap: Java feature map은 SF-G3도 공통 parity gap으로 기록합니다([feature-map.ko.md:83](/home/hep7/project/zlink/framework/languages/java/e2e/StoreFailure/feature-map.ko.md:83), [feature-map.ko.md:100](/home/hep7/project/zlink/framework/languages/java/e2e/StoreFailure/feature-map.ko.md:100)).
   - 제안: SF-G3을 제거하거나 실제 독립 계약이라면 common config의 governance 절차를 통해 먼저 정의하십시오.

## 확인된 정합 항목

- 최근 .NET actorDestroy 수정은 정합합니다. `OwnerLeaseGeneration`이 operation에 포함되고([FrameworkServiceTypes.cs:206](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/FrameworkServiceTypes.cs:206)), handwritten codec encode/decode에도 포함되며([ZLinkServiceWireCodec.cs:1170](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkServiceWireCodec.cs:1170)), generated byte-equivalence test도 해당 필드를 확인합니다([ServiceWireActorDestroyCodecTests.cs:5](/home/hep7/project/zlink/framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/ServiceWireActorDestroyCodecTests.cs:5)).

- C++ command 44는 현재 exactly-once one-way submit으로 구현되어 있습니다([public_host_runtime.cpp:3095](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp:3095), [public_host_runtime.cpp:3195](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp:3195)). C++ exact interface의 설명도 일치합니다([06-stream-session.en.md:188](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/languages/cpp/interfaces/06-stream-session.en.md:188)).

- C++ command 28 receiver의 approval-only 전환 자체에는 새 위반을 찾지 않았습니다. 문제는 Accepted 이후 source continuation입니다.

- Store 21/22/23의 canonical record/key/value와 golden fixture를 production encoder/decoder에 다시 대조했으나, 현재 스냅샷에서 새 record-layout semantic gap은 확정하지 못했습니다. 다만 Config 6 process 증거는 위 finding 8처럼 미완료입니다.

- Spec 26/27의 phase/outcome 분리, flow propagation, Classic fanout 정상-flow 제외에 관해 네 언어 runtime에서 새 위반은 확정하지 못했습니다. C++ feature-map 문구만 finding 12처럼 stale합니다.

## 검토 범위와 증거 한계

`server/` 문서 트리를 contract symbol 기준으로 검색하고, 01/14/15/20/21/22/23/26/27/28/29/30/48/49/51/52, service-wire schema, common guides, Config 6/10, 네 언어 exact-interface 및 production/runtime codec·runner·feature-map을 집중 대조했습니다. 사용한 compliance-review skill에 따라 계약, runtime, protocol, E2E 증거를 별도 판정했습니다.

이번에는 build/test/E2E를 실행하지 않은 정적 read-only 리뷰입니다. 기존 로그의 green 주장은 새로 재현한 결과로 계산하지 않았습니다. 파일 수정·생성·커밋은 하지 않았으며, 현재 표시되는 .NET/Node 변경과 임시 파일은 기존 사용자 작업입니다.


