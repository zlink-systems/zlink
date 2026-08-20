# canonical actorJoin(28) transport 스왑 스코핑 (canon-S4/S5 근거)

작성 2026-08-21 (codex terra high, read-only). Claude 검토.
**키스톤 블로커: 생성 코덱이 body-only — S4a(생성기 multipart payload 확장)가 선행.**

---

## 결론

canon-S4/S5의 transport 스왑은 가능하지만, 먼저 두 경계를 닫아야 합니다.

1. `actorJoin(28)`의 application request는 body tail이 아니라 **별도 multipart payload frame**이다.
2. bound Session 정보는 28에 덧붙일 sideband가 아니라 **42/43/44 session-relocation control**로 처리해야 한다. command 40의 Node 전용 `ZLNI` sideband 선례는 28 확장 권한이 아니다.

현재 W-1 생성 코덱은 body-only라 payload가 있는 실제 Join transport에는 아직 사용할 수 없습니다.

### 1. Canonical 28의 실제 wire

`actorJoin(28)` body는 정확히 다음 네 항목이다.

| Canonical 28 field | 하위 필드 | 용도 |
|---|---|---|
| `correlation` | non-zero `u64` | request/reply 매칭. private 128-bit operation ID와 동일시하지 말고 local registry에서 연결 |
| `actor` route fence | ActorId, ObjectGeneration, current owner Node RID/lifecycle generation, expected authority-owner generation, expected owner-lease generation | Authority row exact-match fence |
| `entry` | `bool8` | Entry Spot join인지 User Spot admission인지 |
| `targetSpot` route fence | SpotId, ObjectGeneration, target Node RID/lifecycle generation, expected authority-owner/lease generation | target Spot authority fence |
| optional application payload | **Frame 1**: version + length + packet name + content type + opaque bytes | application의 Join request |

명세는 body 외 transfer ID 등을 금지하며, stable type은 canonical Location Store Authority row에서 해석하도록 정한다. bound Actor도 canonical 28 수용 자체에 Session을 요구하지 않는다. [spec 51 §9]( /home/hep7/project/zlink/framework/doc/framework/common/spec/server/51-internal-service-wire-protocol.ko.md:467 )

C++의 현행 canonical hand codec도 payload가 “header/body 뒤 tail”이 아니라 별도 multipart frame이라고 명시한다. [service_wire_codec.hpp](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.hpp:775)

중요한 현재 공백은 생성 코덱입니다. schema의 command 28은 optional payload를 선언하지만, pilot generator는 flags `0` body만 encode하고 decode 시 body 뒤 byte를 모두 거부합니다. [generator](/home/hep7/project/zlink/framework/runtime/protocol/generate-service-wire-pilot-codecs.mjs:198) 따라서 “28 body byte-equivalence”는 통과해도 application Join request transport는 아직 구현 범위 밖입니다.

### 2. 사설 dialect delta

| 언어 | Canonical 28과 겹치는 값 | canonical 28에 없는 사설 wire 값 | 처리 |
|---|---|---|---|
| C++ | Actor ID/generation, owner node/lifecycle, authority/lease fence, source·target Spot ID, request bytes | `transferId`, `actorType`, completion operation high/low, source Spot ID, private admission reply의 completion-root reference/checksum | stable type은 Store 검증만 하고 송신 금지. transfer/completion ID는 source-local registry. request bytes는 canonical payload frame. source membership은 local state/후속 commit. |
| .NET | Actor ID/generation, actor authority generation, source/target Spot 관련 값, request content type/bytes | `handoffId`, deadline, source node/Spot, predicted/reserved bytes, reservation token, relocation root/reference/checksum/inventory, handoff frames, bound Session 전체 좌표·binding token·high-water, target reservation fields, coordinator fence | request는 canonical payload frame. reservation/relocation payload/frames는 28이 아닌 relocation command 흐름. bound Session은 42/43/44. token/high-water는 28에 절대 넣지 않음. |
| Java | Actor ref, actor generation, request는 독립 third frame | phase, transfer ID, timeout, stable type, source Entry Spot RID/ID, routerChannelId, source node/session RID, adapter key, backlog count, core transfer ID/epoch/final sequence/reservation, relocation manifest, command-44 bytes, authority fields | `phase`와 transfer bookkeeping은 local coordinator. app request는 28 payload frame. session route command bytes를 28에 재포장하지 않고 canonical 42–44 사용. |
| Node | Actor/target Spot identity 및 authority fence, request content type/bytes | stable type, membership epoch, entry-node RID, create request, phase, transfer ID/adapter/state, source Spot/router channel, bound Session 좌표·seal/previous generations, handoff backlog, completion operation high/low | stable type은 Store only. membership epoch은 command-20 actorJoin reply tail/후속 commit 소유. app request는 canonical payload. bound Session은 42–44; transfer state/backlog는 local/direct relocation 경로. |

근거가 되는 사설 request DTO/field 목록은 각각 [C++]( /home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/spot_route_packets.hpp:28 ), [.NET]( /home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Actors/ZLinkRemoteActorJoinPackets.cs:613 ), [Java]( /home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkActorSpotRoutePackets.java:202 ), [Node]( /home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/actors/actor-remote-wire.ts:50 )에 있다.

특히 C++의 private JSON admission은 `actorType`, transfer ID, completion operation high/low까지 실제로 JSON body에 실어 보낸다. [spot_route_packets.cpp](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/spot_route_packets.cpp:184)

### 3. 없는 필드의 올바른 행선지

| 필드 종류 | canonical 28 처리 |
|---|---|
| Application request payload | 허용됨. body tail이 아니라 schema `application-payload-envelope-v1`의 별도 payload frame. |
| Stable type | 송신 금지. target이 Actor Authority row의 `allocation.stableType`을 exact fence 확인 후 해석. 현재 4언어 receiver Store 해석은 c878d4faca 계열 작업으로 갖춰진 상태. |
| Transfer ID, phase, adapter key, completion operation ID, handoff bookkeeping | language-internal state. wire 금지. correlation은 request/reply 매칭용이며 128-bit operation identity의 축약본이 아님. |
| Relocation manifests, transfer state, accepted backlog | 28 payload에 넣지 않음. command 40/52/34와 기존 direct-transfer owner가 소유. |
| Membership epoch | request field가 아님. acceptance 결과의 command-20 `actor-join-reply-tail` 및 commit state가 소유. |
| Bound-session coordinates/router channel/binding token/seal ID/high-water | 28에 넣지 않음. canonical `sessionRelocationSeal(42)`, `…Sealed(43)`, `…Route(44)`로 전달. |
| `ZLNI` sideband | command 40 Prepare에 한정된 Node-internal implementation 선례. 28에 붙이면 §9 “body 외 wire field 없음”과 충돌하므로, schema/명세 변경 없이는 금지. [existing Node sideband](/home/hep7/project/zlink/framework/languages/node/packages/framework/src/runtime/host/service-relocation-host-runtime.ts:5151) |

### 4. 스왑 슬라이스

1. **Shared generator slice**
   - generated `ActorJoin28`을 “Frame 0 body + optional payload frame” 모델로 확장.
   - four-language positive/negative fixtures를 추가: no-payload, JSON payload, non-JSON content type, truncated envelope, extra frame, malformed fence.
   - body-only golden이 없으므로 common request golden도 이 단계에서 고정. 현재 C++ test도 request golden 부재를 명시한다.

2. **Receiver-first, 네 언어 공통**
   - raw mesh dispatch가 command 28 multipart를 받아 generated decoder로 body와 payload envelope을 decode.
   - Store fence/stable-type resolution은 현행 receiver를 재사용.
   - request payload를 기존 typed message 경로로 복원하고, command-20 canonical reply tail을 반환.
   - 기존 private dialect receiver는 default path로 유지.

3. **Sender slice, 네 언어 공통**
   - source의 private DTO에서 canonical fence + correlation + `entry` + application payload envelope만 추출.
   - transfer ID, operation registry, source membership, reservation은 local state에 남김.
   - wire 선택은 observed target authority와 admitted peer generation이 canonical capability를 증명할 때만 enable; 그렇지 않으면 현 JSON/dialect를 유지.

4. **Bound Session slice**
   - source가 28 전에 필요한 42/43 seal을 수행하고, target commit 후 44 route update를 보냄.
   - private join payload에 있던 Session coordinates를 28 sideband로 옮기는 작업은 하지 않음.

5. **Cross-language harness slice**
   - 현 `JoinEntrySpot` relocation stage는 보존한다. 이는 admission-free relocation 검증이다.
   - 별도 **User Spot normal `JoinSpot`** stage를 추가해 Node↔.NET, Java↔.NET, 이후 C++ 포함 pairwise matrix에서 canonical 28 request, canonical reply, rejected admission, payload round-trip, Store-fence rejection을 확인한다.
   - 현 harness가 JoinEntrySpot만 쓰는 직접 이유는 네 private admission reply dialect가 호환되지 않기 때문이다. [run_cross_language_smoke.sh](/home/hep7/project/zlink/framework/languages/cpp/cross-language/run_cross_language_smoke.sh:935)

### 5. W-1/W-2/W-3 관계

체크인된 W-1 문서의 명칭상:

- **W-1**: generated pilot 및 language-local byte equivalence 준비.
- **W-2**: actorJoin(28) byte layer를 generated module로 바꾸는 단계.
- **W-3**: relocation-envelope-v1의 generated production swap.

즉 저장소 문서 기준으로 actorJoin production swap은 W-2이며, W-3는 relocation envelope이다. [W-1 adoption note](/home/hep7/project/zlink/framework/runtime/protocol/W-1-codec-generator-adoption.md:3) canon-S4/S5에서 “W-3”을 actorJoin transport activation의 캠페인 이름으로 쓰려면 이 W-3와 구분해야 합니다.

또한 generated body codec을 production에 교체하는 일만으로 transport swap이 끝나지 않습니다. transport swap은 generated multipart payload support, raw dispatch, reply correlation, Store-fenced receive, session control 분리를 모두 포함합니다. 현재 generated pilot은 언어 production에서 소비되지 않고, .NET test project에만 링크되어 있습니다.

읽기 전용으로 조사했고 파일 변경·커밋·테스트 실행은 없었습니다. 작업 트리는 시작/종료 시 동일한 기존 untracked 항목만 확인했습니다.


