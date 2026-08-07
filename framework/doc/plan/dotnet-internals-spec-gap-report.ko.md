---
title: ".NET Framework server 스펙 구현 Gap 리포트"
---

# .NET Framework server 스펙 구현 Gap 리포트

- **작성일**: 2026-08-07
- **공개 계약 기준**: 현재 worktree의 `framework/doc/framework/common/spec/`와 .NET exact interface
- **내부 구조 기준**: 현재 worktree의 `framework/doc/framework/common/internals/` 01–12
- **메시지 크기 계약 보정**: 2026-08-07 사용자 확인에 따라 RouteMesh ServerServer에는 Framework message-size 상한이 없다. 별도의 기본 64 KiB·startup 설정 계약은 외부 Client가 StreamNode의 Core STREAM socket으로 보내는 C→S complete message에만 적용한다. ClientServer Channel은 기존 negotiated complete-message 계약을 유지한다.
- **구현 기준**: 전체 audit 기준은 `425b9c2a8272`, 복구·머지 뒤 증분 재검토 기준은 현재 `main` commit `e2119caeda`다. 이 commit의 .NET production 변경 중 RouteMesh message-size, ClientServer와 Actor 경로를 다시 대조했으며 기존 gap을 종결할 변경은 확인하지 못했다.
- **판정 방법**: exact public declaration, production 호출 경로, 오류·수명주기·동시성·HWM 의미, service wire codec과 실제 test assertion을 차례로 대조했다. Type이나 method가 존재하는지만으로 완료 판정하지 않았다. 2차 검토는 `gpt-5.6-sol` high 독립 reviewer가 기존 판정을 반증하고 누락을 찾은 뒤, 지적된 경로를 현재 source에서 다시 확인했다.

현재 `.NET Framework server` 구현은 지정된 스펙을 모두 만족하지 않는다. Public exact interface 3개 영역과 runtime·구조·비용 의미 15개 영역에서 구현 gap을 확인했다. 별도로 timer option은 language exact interface와 canonical wire schema가 서로 충돌하므로 구현 gap으로 확정하지 않고 contract 정본 정리가 필요한 보류 항목으로 분리했다. 특히 현재 worktree에서 추가된 object-location query는 contract test가 실제 누락을 검출했다. 일부 gap에는 스펙보다 약한 assertion이 있고, 나머지는 해당 의미 경계를 직접 검증하는 test가 없다.

Public API와 사용자에게 보이는 동작은 정식 spec과 exact interface만 계약 근거로 사용한다. 구조·POSDDD
gap은 그 계약을 구현하는 internals의 상태 표현, component 책임이나 불변 조건과 다르다는 판정이며,
internals 자체가 새 공개 계약을 만들지는 않는다. 현재 source만으로 사용자-visible 장애나 성능 저하
수치까지 측정됐다는 뜻도 아니며, 그런 주장은 별도 benchmark/process evidence가 있어야 한다.

## 병렬 구현 세션 주의 사항

이 보고서는 다른 언어의 gap 작업과 동시에 진행할 수 있지만, 모든 작업은 현재 `main` checkout에서
수행한다. 별도 `git worktree`나 작업용 branch를 만들지 않으며, 작업 시작 시점의 `main` commit SHA를
작업 기록에 남긴다.

- 이 세션은 해당 언어의 production source·test·package 자료와 이 gap 문서만 수정한다. 다른 언어
  디렉터리, 다른 언어 gap 문서와 공통 spec·internals는 수정하지 않는다.
- `framework/runtime/protocol/`의 schema·generated 파일, cross-language fixture, 공통 검증 script처럼
  여러 언어가 함께 소비하는 파일은 통합 담당자 한 명만 수정한다. 변경이 필요하면 이 문서에 요구사항과
  예상 wire/API 영향을 기록하고 공용 선행 commit을 요청한다.
- 다른 세션의 변경을 원복하거나 포맷하지 않는다. Stage와 commit은 명시적인 경로 목록으로 제한하고
  `git add -A`를 사용하지 않는다.
- Gap 종결은 source 수정만으로 판단하지 않는다. Owner-layer regression, public API/exact snapshot,
  package 또는 clean-consumer, 관련 process E2E 증거를 각각 기록하고 통과한 항목만 종결한다.
- 언어별 작업이 `main`에 반영된 뒤 통합 담당자가 cross-language contract, service-wire fixture, 전체 문서
  검사와 process E2E를 다시 실행한다. 개별 성공을 전체 종결로 승격하지 않는다.

### 구현 중 리팩터링·checkpoint 규칙

Gap 하나 또는 서로 강하게 연결된 작은 작업 묶음의 동작과 회귀 test가 통과하면 다음 Gap으로 넘어가기
전에 리팩터링 checkpoint를 둔다. 마지막에 한꺼번에 정리하지 않는다.

- Production code는 POSD 관점에서 deep module과 information hiding을 강화하고, 의미 없이 인자를 전달하는
  pass-through 계층, 호출 순서에 의존하는 temporal decomposition과 중복 helper를 제거한다. DDD 관점에서는
  lifecycle·ownership·state transition·terminal error invariant를 해당 domain owner가 책임하게 정리한다.
- 같은 checkpoint에서 unit test도 POSD/DDD 관점으로 리팩터링한다. 반복 setup은 의도를 드러내는 fixture나
  builder 안에 숨기고, test 이름과 helper는 domain 용어와 observable behavior를 표현하게 한다. Production
  내부 구조를 그대로 복제하거나 실행 순서와 private 구현에 결합된 test는 제거하거나 계약 중심으로 바꾼다.
- 리팩터링 뒤 dead code, 사용하지 않는 wrapper·alias·fixture·dependency를 제거하고, hot path의 불필요한
  allocation·copy와 lock·queue contention도 함께 점검한다. 동작 변경이 있으면 owner-layer regression을
  먼저 추가하고 관련 unit test를 다시 실행한다.
- 관련 test가 통과한 의미 있는 checkpoint마다 해당 언어 경로와 이 문서만 path-limited staging하여
  commit하고 `main`에 push한다. Commit에는 닫은 Gap ID와 실행한 test를 남기고, 검증되지 않은 변경이나
  다른 언어의 변경을 섞지 않는다. Push한 commit SHA와 gate 결과를 이 문서의 해당 항목에 기록한 뒤
  다음 작업으로 진행한다.

## 1. 판정 요약

| ID | 심각도 | 영역 | 판정 |
|---|---|---|---|
| DOTNET-API-001 | 상 | STREAM session send | 완료 — per-call admission timeout을 public surface와 기존 submit admission owner에 연결했다 |
| DOTNET-API-002 | 상 | Location 운영 query | 완료 — authority Store를 조회해 exact/list object location을 공개 계약으로 투영한다 |
| DOTNET-API-003 | 상 | StreamNode message-size API | 완료 — fluent `MaxMessageSize(bytes)`와 전용 socket config surface를 제공한다 |
| DOTNET-ROLE-001 | 상 | ClientServer role error | 완료 — Server role만 등록된 Channel send/request가 `NotConfigured`로 끝난다 |
| DOTNET-WIRE-001 | 상 | `framework-json-v1` | 완료 — application payload 전용 strict codec이 canonical golden profile을 강제한다 |
| DOTNET-SIZE-001 | 상 | RouteMesh SS message size | 완료 — public·admission·native receive·sender 상한을 제거하고 HWM과 wire guard만 유지한다 |
| DOTNET-WIRE-002 | 상 | ClientServer message bound | 완료 — negotiated complete-message 상한을 send/request/inbound/reply에 같은 계산으로 적용한다 |
| DOTNET-COMP-001 | 상 | completion overflow | 완료 — retention 포화가 pending과 이후 caller에게 `CapacityExceeded` terminal로 전달된다 |
| DOTNET-EXEC-001 | 상 | STREAM session queue | 완료 — session과 생성 ingress queue가 retained payload byte와 고정 비용을 함께 예약한다 |
| DOTNET-EXEC-002 | 중 | serial execution engine | 완료 — Spot/session/Actor 전달이 공통 serial engine의 lane policy로 수렴했다 |
| DOTNET-EXEC-003 | 상 | Entry Actor ingress HWM | 완료 — process-wide ingress와 Actor별 공통 serial lane이 count·retained byte를 제한한다 |
| DOTNET-LIFE-001 | 중 | Spot type model | 완료 — 공통 resource base 위에 User와 Instance activation type과 context surface를 분리했다 |
| DOTNET-LIFE-002 | 중 | Ready Instance owner loss | 완료 — 실제 owner crash·restart 뒤 `Unavailable`과 queue replay 부재를 process에서 검증했다 |
| DOTNET-COMP-002 | 중 | completion ordering | 완료 — sender가 응답 상관 값을 먼저 할당하고 waiter 등록 뒤 submit한다 |
| DOTNET-LAYER-001 | 중 | binding 경계·POSDDD | 완료 — 일반 socket은 binding public API를 직접 쓰고 의미 변환 adapter만 유지한다 |
| DOTNET-LAYER-002 | 상 | runtime/package ownership | relocate·shutdown 의미와 public runtime 구현이 ASP.NET Core 통합 package에 있다 |
| DOTNET-LAYER-003 | 중 | identifier type | mesh·channel·Actor·Spot ID를 내부 경계에서도 모두 `string`으로 섞어 쓴다 |
| DOTNET-LAYER-004 | 중 | STREAM protocol ownership | client connector와 Framework server가 같은 STREAM codec·pending·lifecycle stack을 별도로 구현한다 |
| DOTNET-OWN-001 | 중 | payload ownership/copy | 이미 소유한 managed payload를 public copying factory에 다시 통과시켜 full-buffer copy를 추가한다 |
| SPEC-TIMER-001 | 보류 | timer relocation contract | language spec은 non-catch-up 값을 검증하지 않지만 canonical schema는 `nonzero-u64`만 허용해 정본끼리 충돌한다 |

## 2. 상세 발견 사항

### DOTNET-API-001 — session send의 per-call admission timeout 누락

**판정: 완료**

Exact interface는 `IZLinkSessionSendCall.Timeout(TimeSpan)`을 요구하고, 이 값이 STREAM socket `SendTimeout`을 늘리지 않고 더 짧게만 적용되며 만료 시 `DeadlineExceeded`로 정확히 한 번 끝나야 한다고 정한다(`interfaces/07-stream-session.ko.md:67-73,137-141`). 실제 interface에는 `Compress()`와 `Async(...)`만 있고 `Timeout(...)`이 없다(`Contracts/Streams/IZLinkSession.cs:82-93`). 따라서 application은 계약에 적힌 per-call timeout을 표현할 수 없고, runtime에도 양수 millisecond 범위 검증, socket timeout과의 `min`, 만료 뒤 재admission 차단 경로가 없다.

완료 조건은 public method와 call implementation을 함께 추가하고 다음을 검증하는 contract/runtime test다.

- 생략 시 socket `SendTimeout` 사용
- 지정 시 두 timeout 중 짧은 값 사용
- 0, 음수와 `Int32.MaxValue` millisecond 초과 거부
- timeout·cancellation·send-ready 경쟁에서 terminal-once
- 만료 뒤 늦은 send-ready가 제출을 시작하지 않음

`IZLinkSessionSendCall.Timeout(...)`을 추가하고, 양수 millisecond 범위로 올림한 값을 기존
`ZLinkAsyncSubmitter` admission deadline에 전달한다. Submitter는 STREAM socket timeout과 per-call timeout
중 짧은 deadline 하나만 만들며, timeout이나 cancellation이 terminal을 먼저 확정하면 이후 send-ready가
같은 operation을 다시 제출하지 않는다.

검증은 .NET 8에서 `ZLinkAsyncSubmitterTests` 32건과 `StreamContracts` 4건이 통과했다. 별도의 source
package를 만든 뒤 실행한 `verify_packaged_contract.sh --generate-snapshot`도 packaged contract와 standalone
HTTP clean consumer를 모두 통과했다. 고정 public API snapshot과 package XML hash를 같은 source 결과로
갱신했다. 이 checkpoint는 `b2ecdc54c3`으로 `main`에 push했다.

### DOTNET-API-002 — object location 운영 query 누락

**판정: 완료**

Exact interface는 object kind/state/entry/filter와 `FindActorLocationAsync`, `FindSpotLocationAsync`, `ListObjectLocationsAsync`를 요구한다(`interfaces/08-location-maintenance.ko.md:106-131,150-178`). 실제 `IZLinkLocationRuntimeQuery`는 status, topology, service summary 세 operation만 제공한다(`Contracts/Locations/RuntimeQuery.cs:8-21`). 구현 service도 MeshNode descriptor만 조회하며 object row를 조회하거나 `Creating`·`Ready`·`Unavailable`로 투영하는 경로가 없다(`Runtime/Locations/ZLinkLocationRuntimeQueryService.cs:63-176`).

이 gap은 정적 추정이 아니라 현재 contract test가 직접 검출했다. `DotNetExactInterfaceDeclarations_Match_Source_And_Package_Exports`와 `DotNetExactInterfaceTypes_Have_A_Single_DocumentOwner`가 `ZLinkLocationObjectEntry` export 부재로 실패했다. Exact lookup의 Missing=`null`, Spot kind 통합, 4 MiB page, Store 실패=`Unavailable`과 partial page 금지까지 production test로 확인해야 완료할 수 있다.

`IZLinkLocationRuntimeQuery`에 exact Actor·Spot 조회와 kind별 list query를 추가했다. 새 내부
`ZLinkLocationObjectQuery`가 authority key 해석, allocation state와 owner lease를 이용한
`Creating`·`Ready`·`Unavailable` projection, opaque continuation과 Store 오류 변환을 한 곳에서 소유한다.
Store key·version과 owner lease generation은 public entry에 노출하지 않는다.

`.NET 8`의 `LocationRuntimeQueryTests` 16건에서 Missing과 세 상태, User Spot·Instance Spot 구분,
filter·continuation, Store read 실패의 typed `Unavailable`, partial page 미반환과 4 MiB 초과 page 거절을
검증했다. `LocationContracts`와 exact type owner 검증 8건도 통과했다. 별도 source package를 생성한
`verify_packaged_contract.sh --generate-snapshot`은 packaged contract와 standalone HTTP clean consumer를
통과했고, 고정 public API snapshot은 생성 결과와 일치하며 hash는
`917792ee8f6a4f645f03b1b683041b819d7501bd0ec54807c728c4fd8ab164e1`이다. 전체 exact declaration
검증에는 이 항목과 무관한 `DOTNET-SIZE-001`의 기존 RouteMesh `MaxMessageSize` property 한 건이 남아 있으며,
해당 항목에서 제거한다.
이 checkpoint는 `9e7628e61e`으로 `main`에 push했다.

### DOTNET-API-003 — StreamNode message-size fluent API 불일치

**판정: 완료**

Exact interface는 StreamNode builder가 `MaxMessageSize(long bytes)`를 직접 제공하고 builder를 반환해 `.Bind(...).MaxMessageSize(64 * 1024).AddSession<...>()` 형태로 연결하도록 정한다(`interfaces/03-configuration-topology.ko.md:247-286`). `ConfigureSocket()`은 HWM·buffer·timeout용 `IZLinkStreamSocketConfig`를 반환하며 message-size property를 노출하지 않는다.

실제 public builder는 `ConfigureSocket()`만 제공하고 `IZLinkSocketConfig`를 반환한다(`Contracts/Configuration/Builders.cs`, `Runtime/Configuration/Builders/ZLinkStreamNodeBuilder.cs:41`). Application은 반환 객체의 mutable `MaxMessageSize` property에 대입해야 하므로 exact fluent call을 컴파일할 수 있고 없는지 여부가 반대로 되어 있다. 기본 64 KiB와 C→S 전용 runtime 적용 자체는 맞지만 public 호출 표면은 스펙과 다르다.

완료 조건은 `IZLinkStreamNodeBuilder.MaxMessageSize(long bytes)`를 추가해 같은 builder를 반환하고, StreamNode의 `ConfigureSocket()` 반환형에서 message-size property를 제거하는 것이다. 64 KiB 기본값, 양수·0·음수 검증과 fluent chaining을 public contract test와 clean consumer에서 확인해야 한다.

`IZLinkStreamNodeBuilder.MaxMessageSize(long bytes)`를 추가하고 `ConfigureSocket()`은 message-size property가
없는 `IZLinkStreamSocketConfig`를 반환하도록 분리했다. 기존 내부 config는 값을 한 곳에서 소유하므로 inbound
runtime 경로와 startup validator에는 별도 복제 상태를 만들지 않았다.

`.NET 8`의 `TopologyExactSurfaceTests` 4건, 관련 inbound validation 1건과 `BuilderContracts` 5건이
통과했다. Source package를 사용한 packaged contract와 standalone HTTP clean consumer도 통과했으며, 생성한
public API snapshot hash는 `68b461cb8de2ff286232a3d17ecc1e30fbd5c540b392dbbc2ba2c5e8b2da680f`다.
이 checkpoint는 `1e30131ff3`으로 `main`에 push했다.

### DOTNET-ROLE-001 — Server-only ClientServer Channel의 오류 kind가 틀림

**판정: 완료**

공통 ClientServer 계약은 같은 `ChannelName`의 Server role이 존재하더라도 local Client role이 없으면 send/request를 시작하지 않고 `NotConfigured`로 끝내며, `NotFound`는 ChannelName이나 선택할 target 자체가 없는 경우에만 사용하도록 정한다(`common/spec/09-client-server-channel.ko.md:57-66`).

실제 send는 등록된 channel의 `HasClientServerClient`가 true일 때만 ClientServer 경로에 들어가고, false이면 RouteMesh lookup으로 fallback한다(`Runtime/Host/ZLinkFrameworkRuntimeChannels.cs:21-63`). Request도 같은 분기다(`:65-110`). Server role만 등록된 같은 이름의 Channel은 존재하지만 Client role 조건을 통과하지 못하며, RouteMesh도 없으면 `ResolveRouteMeshNodeForChannel`이 `NotFound`를 던진다(`Runtime/Host/ZLinkFrameworkRuntimeSpots.cs:271-311`). Server-only send/request의 exact error kind를 주장하는 test도 없다.

완료 조건은 Channel 등록은 존재하지만 Client role이 없음을 topology fallback 전에 구분해 두 call 모두 `ZLinkFrameworkErrorKind.NotConfigured`로 끝내는 것이다. Server-only, 이름 자체 없음, Client role은 있으나 ready target 없음을 각각 분리한 회귀 test가 필요하다.

Channel call path가 ClientServer registration을 먼저 분류하고 local Client role이 없으면 RouteMesh lookup을
시작하지 않도록 바꿨다. 같은 분류를 send와 request가 공유하며, 실패 전에 Framework가 소유한 message parts를
반납한다.

Owner-layer regression은 Server-only send/request=`NotConfigured`, 이름 없음=`NotFound`, Client role은 있지만
ready target 없음=`DeadlineExceeded`를 각각 분리해 3건 모두 통과했다. 실제 process scenario
`CH-E2E-05`도 `logs/20260807-172841-1093480/`에서 통과했으며 Server-only handler evidence가 생성되지
않고 정상 Client role request만 Server handler에 한 번 도달했다.
이 checkpoint는 `c873a99aa0`으로 `main`에 push했다.

### DOTNET-WIRE-001 — `framework-json-v1` strict profile 미구현

**판정: 완료**

공개 message model은 property와 enum name의 대소문자를 구분하고, duplicate·누락 required property를 거부하며, 64-bit integer는 범위를 검사한 정규 decimal **문자열**로 처리하도록 요구한다(`common/spec/04-message-model.ko.md:95-118`). Internals §6도 별도 규칙을 만들지 않고 이 public profile을 runtime validation의 정본으로 위임한다(`common/internals/12-service-wire-protocol.ko.md:272-279`). Golden fixture도 `property-case`, `duplicate-property`, numeric signed64를 invalid로 고정한다(`framework/runtime/protocol/golden/framework-json-v1.json`).

실제 공통 JSON option은 `new JsonSerializerOptions(JsonSerializerDefaults.Web)` 하나뿐이다(`Runtime/Messaging/ZLinkJsonSerializerOptions.cs:6-15`). 이 preset은 스펙 전용 validator가 아니며 property 대소문자, duplicate 탐지, required-field completeness와 64-bit 문자열 정규형을 profile대로 강제하는 단계가 없다. Route/Spot/STREAM typed dispatch는 이 option을 그대로 사용한다(`Runtime/Messaging/ZLinkEnvelopeCodec.cs:368-371`, `Runtime/Streams/ZLinkStreamPacketPayloadCodec.cs:53-54`). Schema self-test 성공은 schema와 golden asset이 유효하다는 뜻일 뿐 이 consumer 동작을 검증하지 않는다.

완료하려면 allocation을 크게 만들기 전에 golden fixture 전체를 검증하는 .NET profile reader를 두고, 네 언어 공통 fixture를 .NET runtime decode 경로에 직접 통과시켜야 한다. 일반 `System.Text.Json` round-trip test는 대체 증거가 아니다.

`ZLinkFrameworkJsonPayloadCodec`을 application typed payload의 단일 JSON 경계로 추가했다. 이 codec은
deserialize 전에 BOM과 중복 property를 거부하고, case-sensitive property, required·nullable 선언,
64-bit 정수의 정규 decimal string, exact enum name, padded base64와 유한 number 규칙을 적용한다. UUID처럼
언어 runtime이 암묵적으로 변환하는 type은 거부하며, 내부 relocation DTO가 UUID를 사용하는 한 곳은
property-level canonical lowercase `D` string converter로 계약을 명시했다. Envelope header와 runtime control
payload는 기존 protocol JSON helper로 분리해 application profile 변경이 내부 wire 표현을 바꾸지 않는다.

공통 `framework-json-v1.json`의 valid·invalid vector 전체와 nested duplicate, BOM, non-nullable null,
비정규 64-bit 문자열, 암묵적 UUID 거절을 `FrameworkJsonProfileTests` 10건에서 실제 envelope decode 경로에
통과시켰다. Envelope·custom codec·Actor·STREAM 관련 78건과 처음 실패한 내부 relay·relocation 회귀 7건이
통과했고, 최종 strict number 설정을 포함한 전체 .NET unit suite도 1,572건 모두 통과했다.
`verify_packaged_contract.sh`의 packaged contract와
standalone HTTP clean consumer가 통과했으며 public API snapshot hash는
`917792ee8f6a4f645f03b1b683041b819d7501bd0ec54807c728c4fd8ab164e1`이다. 실제 process `RegistrationCodec`
`RC-B6`도 `logs/20260807-175928-2425161/`에서 int64·bytes·nullable typed JSON round trip과 handler evidence를
확인하고 통과했다.
이 checkpoint는 `14b720a2d1`으로 `main`에 push했다.

### DOTNET-SIZE-001 — RouteMesh ServerServer에 없어야 할 16 MiB 상한이 있음

확인된 계약은 RouteMesh ServerServer transport에 Framework-level `MaxMessageSize`를 두지 않는 것이다. 공통 channel spec도 이 결정을 그대로 적고 있다(`common/spec/07-channel-topology.ko.md:609-634`). Transport·service-wire 표현 한계와 process memory/HWM은 남지만 별도 complete-message 크기 설정이나 그 이유의 거절은 없어야 한다.

실제 .NET은 `IZLinkMeshNodeSocketConfig.MaxMessageSize`를 public surface로 노출한다(`Contracts/Configuration/MeshNodeBuilders.cs:23-41`). RouteMesh router registration은 일반 `ZLinkSocketConfig`를 사용해 기본 16 MiB를 받고(`Runtime/Configuration/ZLinkFrameworkRegistration.cs:497-513`, `Runtime/Configuration/ZLinkSocketConfigs.cs:6-21`), initializer가 이를 managed MeshNode에 적용한다(`Runtime/Spots/ZLinkSpotNodeInitializer.cs:28-45`). Managed node는 native router receive option에 이 값을 설정할 뿐 아니라(`Runtime/Service/ZLinkManagedMeshNode.cs:248-258`) peer와 작은 값을 골라 모든 send 전에 complete-message 합계를 검사한다(`:8048-8079,8132-8144`). Unit test도 RouteMesh 기본값을 16 MiB로 고정한다(`UnitTests/Configuration/Registration/InboundDispatchOptionsTests.cs:197-217`).

완료 조건은 RouteMesh public `MaxMessageSize` 설정과 admission field, native receive cap 및 sender-side complete-message check를 제거하는 것이다. HWM/mailbox byte budget과 protocol 표현 한계는 별도 자원·wire guard로 유지한다. 회귀 검증은 임의의 새 payload 상한을 암시하지 않도록 public API snapshot, admission wire fixture와 일반 payload 무결성 E2E로 구성한다.

구현과 검증을 완료했다. `IZLinkMeshNodeSocketConfig`에서 `MaxMessageSize`를 제거했고 RouteMesh
admission descriptor에서도 해당 field를 제거해 공통 schema의 field 순서와 맞췄다. Managed RouteMesh
socket은 native inbound cap을 `-1`로 명시해 binding이나 Core 기본값에 의존하지 않으며, sender와 receiver의
complete-message 비교는 제거했다. HWM, mailbox byte budget, control frame 개수와 .NET byte array 표현 한계는
서로 다른 자원·wire guard로 유지했다.

RouteMesh admission round trip은 lifecycle generation이 security identity 바로 뒤에서 시작하는 정확한 byte
위치와 재인코딩 byte 일치를 검증한다. 1 byte와 기존 기본값을 넘는 17 MiB payload는 실제 managed node
request/reply production socket 경로에서 hash가 아니라 전체 byte 일치로 통과했다. 관련 focused test 23건과
전체 .NET unit suite 1,573건이 통과했다. Packaged contract와 standalone HTTP clean consumer가 통과했으며
public API snapshot hash는
`c1987f4b98e4fac7a30b7d038a56ee0d20e1272d00e79bdc88d3271e3c3ab958`이다. 실제 process
`ToActorMessaging` `TA-A1`도 `logs/20260807-183121-3907372/`에서 RouteMesh request/reply와 owner handler
evidence를 확인하고 통과했다.

별도 `LocationMessaging` `RM-C8`은 `logs/20260807-183255-3962502/`에서 첫 1-byte Channel request가
provider handler에 도달하지 않고 timeout되어 payload 크기 검증 전 실패했다. 같은 실행의 connection admission은
Ready였고 17 MiB owner-layer 회귀는 통과하므로 이 log를 SIZE-001 완료 근거로 사용하지 않았다. 이
multi-provider Channel routing 실패는 SIZE-001에서 제거한 message-size 정책과 구분해 후속 process 회귀
조사 입력으로 남긴다.
이 checkpoint는 `ce9b881ae0`으로 `main`에 push했다.

### DOTNET-WIRE-002 — ClientServer negotiated complete-message 상한 미강제

스펙은 sender가 local/remote `normalizedEffectiveMaxMessageBytes`의 작은 값을 admitted connection lifetime 동안 고정해 submit 전에 적용하도록 정한다(`common/internals/12-service-wire-protocol.ko.md:99-110`). ClientServer admission은 이 값을 보존하고 update가 바꾸지 못하게 막는다(`Runtime/Channels/ZLinkClientServerClientRuntime.cs:1454-1493`). 그러나 실제 send는 선택한 socket에 parts를 그대로 넘기고(`:136-151`), request도 같은 방식으로 `ZLinkRawRequestSubmitter`에 넘긴다(`:154-190`). Connection의 `_normalizedEffectiveMaxMessageBytes`는 hello/admit 검증에만 쓰이고 application submit 크기 검사에는 쓰이지 않는다. Server reply도 `Socket.Reply(...)`를 직접 호출한다(`:1511-1523`).

조사 당시 RouteMesh에는 반대로 complete-message 합계 검사가 있었지만 SIZE-001에서 제거했다. ClientServer에는
send/request/reply 각각에 exact admitted bound를 적용하고, remote 한도가 더 작은 경우와 negotiated
complete-message 경계의 바로 아래·위 사례를 production socket test로 검증해야 했다.

구현과 검증을 완료했다. Client connection은 admission이 반환한
`NormalizedEffectiveMaxMessageBytes`를 admitted lifetime의 불변값으로 읽고, send와 request를 native submit하기
전에 complete message의 모든 part byte를 합산한다. Server는 Hello에서 local·remote 중 작은 값을 peer별로
보관하고 inbound application message와 reply에 같은 값을 적용한다. Handler reply가 상한을 넘으면 원래
payload를 보내지 않고 같은 correlation의 `CapacityExceeded` error terminal을 상한 안에서 반환한다.

공통 `ZLinkClientServerMessageBound`가 send, request, receiver와 reply의 계산을 한 곳에서 소유한다. 512-byte
remote와 4 KiB local 조합에서 oversized send/request가 handler 실행 전에 `CapacityExceeded`로 끝나고,
oversized server reply도 caller가 같은 terminal을 관찰했다. 512 byte exact boundary는 허용하고 513 byte는
거절하는 회귀를 포함해 ClientServer 30건과 전체 .NET unit suite 1,576건이 통과했다. Packaged contract와
standalone HTTP clean consumer도 통과했으며 public API snapshot hash는
`c1987f4b98e4fac7a30b7d038a56ee0d20e1272d00e79bdc88d3271e3c3ab958`이다. 실제 process
`ChannelEgressRouting` `CH-REG-06`은 `logs/20260807-184635-466543/`에서 RouteMesh와 ClientServer request가
application retry 없이 각각 1초 안에 완료되는 것을 확인하고 통과했다.
이 checkpoint는 `d69e505ad2`로 `main`에 push했다.

### DOTNET-COMP-001 — completion retention 전체 포화 시 caller terminal 유실

Internals는 early completion 보관 자리가 가득 차면 source runtime 소유 자원 부족을 caller가 `CapacityExceeded`로 관찰해야 하며, 응답을 버리고 timeout으로 바꾸는 것을 명시적으로 금지한다(`common/internals/04-completion.ko.md:110-127`). 현재 구현은 early payload store와 failure tombstone store 두 단계를 두지만, 둘 다 가득 차면 `OverflowCount`와 metric만 증가시키고 parts를 dispose한다(`Runtime/Backend/DotNet/ZLinkMeshCompletionTable.cs:135-177`). 이후 같은 operation을 등록하면 overflow 사실을 찾을 표식이 없으므로 `_pending`에 들어가 timeout까지 기다린다(`:73-103`).

현재 test도 caller terminal을 확인하지 않는다. `CompletionTable_OverflowBeyondRetentionIsObservable`은 세 번째 완료 뒤 `OverflowCount == 1`만 주장한다(`UnitTests/Runtime/ServiceRuntimeFoundationTests.cs:1639-1651`). 이는 스펙의 “caller가 관찰 가능한 결과”보다 약한 assertion이다. Bounded 구조를 유지하되 등록 전 overflow operation이 나중에 반드시 `CapacityExceeded`로 끝나는 소유 구조가 필요하다.

구현과 검증을 완료했다. Early payload와 failure tombstone이 모두 포화되면 completion table은 retention
자원 전체를 fail-closed 상태로 전환한다. 이 전이는 table이 이미 소유한 pending callback을 모두
`Backpressured` terminal로 완료하고, 보관 중인 payload를 dispose하며, 이후 등록도 같은 terminal로 즉시
완료한다. 따라서 operation별 marker를 무한히 늘리지 않으면서 overflow terminal이 caller timeout으로
바뀌는 경로를 제거했다. Public 오류 변환에서는 source runtime의 이 terminal을 `CapacityExceeded`로
관찰한다.

Completion table focused test 6건은 overflow operation의 늦은 등록, 이미 pending인 operation과 overflow
이후 등록을 검증하고 통과했다. .NET unit project는 두 번 실행했으며 매번 1,576건이 통과했지만, 첫
실행에서는 `LogicalMulticastSubmitsEachPositiveRemoteOnceRegardlessOfWeight`, 두 번째 실행에서는
`RemoteUserSpotTerminalReplaysAfterDeadlineAndExpiresWithoutReexecution`이 각각 한 번 실패했다. 두 test는
각각 단독 재실행에서 통과했으므로 completion 변경과 별개인 간헐 실패로 분리한다. Packaged contract와
standalone HTTP clean consumer는 통과했고 public API snapshot hash는
`c1987f4b98e4fac7a30b7d038a56ee0d20e1272d00e79bdc88d3271e3c3ab958`로 유지되었다. 실제 process
`ChannelEgressRouting` `CH-REG-06`은 `logs/20260807-190203-1199732/`에서 통과했다. 구현 checkpoint는
`d91088d688`로 `main`에 push했다.

### DOTNET-EXEC-001 — STREAM session execution queue가 payload byte를 세지 않음

Internals는 실행 owner의 각 lane이 count와 byte를 모두 예약하고 payload 외에 작업당 고정 비용도 포함하도록 정한다(`common/internals/08-object-lifecycle.ko.md:219-268`). `ZLinkSerialExecutionQueue.TryPostAccepted`는 Spot ingress에서 `payloadLength + 256`을 올바르게 예약한다(`Runtime/Execution/ZLinkSerialExecutionQueue.cs:293-378`).

반면 STREAM session은 application packet의 header/payload를 closure로 capture한 뒤 payload 길이 없이 `EnqueueApplication`을 호출한다(`Runtime/Streams/ZLinkStreamSessionRuntime.cs:249-274`). Executor는 이를 payload-aware overload가 아닌 `TryPostApplicationWithAdmission`으로 넘기고(`Runtime/Streams/ZLinkStreamSessionSerialExecutor.cs:79-83`), queue는 항상 고정 256 byte만 예약한다(`Runtime/Execution/ZLinkSerialExecutionQueue.cs:179-207`). 새 session을 만드는 node-level ingress도 header/payload/lease를 capture하면서 같은 고정비 전용 executor에 넣는다(`Runtime/Streams/ZLinkStreamNodeRuntime.cs:663-742`). Host-wide inbound HWM lease가 payload를 세더라도 session execution queue의 독립 byte 한도를 대신하지 못한다. 큰 packet이 한 session 또는 session-creation ingress에 몰리면 64 MiB lane 상한이 아니라 4,096건 count 한도가 먼저 적용될 수 있다.

Application packet과 session-creation ingress 모두 complete retained bytes를 넘겨 예약하고 handler terminal에서 반납해야 한다. 작은 packet count 포화와 큰 packet byte 포화를 따로 재현하는 test가 필요하다.

구현과 검증을 완료했다. `ZLinkSerialExecutionQueue`의 application admission이 retained byte를 받아 작업당
고정비 256 byte와 함께 count·byte를 한 번에 예약하며, 기존 work item completion 경로에서 같은
`AccountingBytes`를 반납한다. 기존 STREAM session과 session 생성 ingress는 모두 header와 payload 크기의
합을 공통 계산 함수로 구한 뒤 이 admission에 전달한다. Host-wide inbound lease와 session execution
reservation의 책임은 합치지 않았으며 각 owner가 자신의 수명 동안 독립적으로 byte를 보유한다.

Byte exact-boundary, active work 중 byte 포화, completion 뒤 재수락과 STREAM cleanup을 포함한 focused test
25건과 전체 .NET unit suite 1,580건이 통과했다. Packaged contract와 standalone HTTP clean consumer도
통과했고 public API snapshot hash는
`c1987f4b98e4fac7a30b7d038a56ee0d20e1272d00e79bdc88d3271e3c3ab958`로 유지되었다. 실제 STREAM
연결·request를 포함하는 `ChannelEgressRouting` `CH-REG-02`는
`logs/20260807-191555-1965910/`에서 통과했다. 이 checkpoint는 `fdca4650c2`로 `main`에 push했다.

### DOTNET-EXEC-002 — 직렬 실행 engine이 공통 기관 하나로 수렴하지 않음

Internals는 Spot, session과 Actor 전달의 순서·수락·준비 집합을 다루는 실행 engine을 하나만 두고, 자리별 차이는 별도 engine이 아니라 유효 상태만 표현하는 lane policy type으로 모델링하도록 정한다(`common/internals/09-session-binding.ko.md:33-59`). 확인 기준에도 “직렬 실행 원시 타입이 runtime 안에서 하나”라고 명시한다(`:115-124`).

Spot과 session은 `ZLinkSerialExecutionQueue`를 공통으로 사용한다(`Runtime/Spots/ZLinkSpotSerialExecutor.cs:5-16,55-60`, `Runtime/Streams/ZLinkStreamSessionSerialExecutor.cs:3-23`). 그러나 Actor dispatch는 별도 `ZLinkActorDispatchMailbox`가 ordinary/barrier waiter queue, admission close와 ready handoff를 자체 구현한다(`Runtime/Actors/ZLinkActorDispatchMailbox.cs:3-115`). Message Follow Actor 전달도 자체 `ConcurrentQueue`, count·byte admission과 drain ownership을 가진 `ActorQueue`를 별도로 구현한다(`Runtime/Spots/ZLinkActorMessageFollower.cs:777-845`). 각각이 개별 경로에서 맞게 동작하더라도 공통 engine 수정이 이 경로들에 자동 적용되지 않으므로 스펙이 금지한 중복 기관 구조다.

이 항목은 DOTNET-EXEC-001의 payload byte 누락과 다르다. 완료 조건은 공통 engine 하나에 Spot/session/Actor-delivery lane policy를 주입하고, 불가능한 lifecycle 조합을 타입으로 만들 수 없게 하는 것이다. FIFO, barrier, admission close, count·byte bound와 drain race test를 동일 engine contract suite로 실행해야 한다.

구현과 검증을 완료했다. Actor dispatch mailbox는 자체 waiter queue와 busy/drain 선택을 제거하고 공통
`ZLinkSerialExecutionQueue` 위에 Actor lifecycle 정책만 남겼다. 일반 dispatch와 terminal barrier는
application lane을 사용하고, 이미 대기 중인 일반 dispatch보다 먼저 실행해야 하는 deferred Join barrier는
`ZLinkSerialWorkLane.Lifecycle`을 사용한다. Admission close/reopen과 pending request는 Actor aggregate의
상태로 유지하지만 FIFO, lane 선택, count·byte reservation과 drain은 공통 engine이 소유한다.

Message Follow의 Actor별 `ConcurrentQueue`, drain flag와 별도 count·byte counter도 제거했다. 각 route는
같은 공통 engine에 encoded payload byte를 넘기며, engine의 application-drained signal이 route queue의
안전한 retirement를 결정한다. 기존 전역 admission slot은 Message Follow 전체 호출 수를 제한하는 별도
owner이므로 유지했다. 사용되지 않던 mailbox의 pending-message counter와 인자도 함께 제거했다.

공통 engine, Actor FIFO·cancellation·barrier·terminal close/reopen·handoff와 Message Follow 경로 focused
test 260건이 통과했다. 전체 .NET unit project는 1,580건이 통과하고
`LogicalMulticastSubmitsEachPositiveRemoteOnceRegardlessOfWeight` 한 건이 5초 condition timeout으로
실패했지만, 같은 test의 단독 재실행은 통과했다. 이 간헐 실패는 Actor serial 변경과 별도로 유지한다.
Packaged contract와 standalone HTTP clean consumer는 통과했고 public API snapshot hash는
`c1987f4b98e4fac7a30b7d038a56ee0d20e1272d00e79bdc88d3271e3c3ab958`로 유지되었다. 실제 process
`ToActorMessaging` `TA-A1`은 `logs/20260807-193610-3123003/`에서 통과했다. 이 checkpoint는
`a05ebdc421`로 `main`에 push했다.

### DOTNET-EXEC-003 — Entry Actor ingress가 count·byte 상한 없이 적재됨

**판정: 완료**

Internals는 실행 대기열마다 count와 byte reservation을 모두 강제하고 상한 없는 실행 대기열을 금지한다(`common/internals/08-object-lifecycle.ko.md:219-268`). Host-wide payload HWM은 process 수신 byte 회계이며 owner execution queue의 count·byte 한도를 대체하지 않는다.

Entry Spot Actor ingress는 `Channel.CreateUnbounded<ZLinkSpotActorFrameBatch>`를 사용한다(`Runtime/Spots/ZLinkEntrySpotDispatchPump.cs:18-24`). 수신한 batch는 별도 queue reservation 없이 `TryWrite`되고(`:153-177`), reader는 batch를 bounded lane에 넘기는 대신 Actor ID별 선행 `Task` continuation chain으로 계속 전환한다(`:180-203,234-249`). Batch가 가진 `ZLinkInboundDispatchLease`는 payload byte를 host-wide budget에 유지하지만(`Runtime/Spots/ZLinkSpotActorFrameReader.cs:86-117`) batch/task/envelope count와 고정비를 제한하지 않는다. 따라서 빈 payload나 작은 payload가 몰리면 host payload HWM 아래에서도 Channel item과 Task가 무제한 늘 수 있다.

완료 조건은 Entry Actor ingress를 count와 retained byte를 원자적으로 예약하는 공통 bounded lane으로 옮기고, 포화 위치와 call 종류에 맞는 terminal/admission 결과를 내는 것이다. Zero/small-payload count 포화와 large-payload byte 포화, Actor별 FIFO와 sibling Actor progress를 함께 검증해야 한다.

구현과 검증을 완료했다. Entry Actor ingress의 unbounded `Channel`과 Actor별 `Task` continuation chain을
제거했다. Process-wide ingress admission은 batch 수와 retained body·application metadata byte에 작업당
고정비 256 byte를 더해 원자적으로 예약한다. 각 Actor는 공통 `ZLinkSerialExecutionQueue`를 사용하므로
Actor별 count·byte 한도와 FIFO를 같은 execution engine이 소유하며, Actor마다 lane을 분리해 한 Actor의
handler가 대기해도 다른 Actor의 dispatch를 막지 않는다. Lane이 비면 공통 engine의 drained signal로
dictionary entry와 queue를 함께 정리한다.

Process-wide 또는 Actor lane admission이 포화되면 request는 `CapacityExceeded`와 retry-after-backoff로
끝나고, one-way message는 handler를 실행하지 않은 채 Framework가 보유한 payload를 반납한다. 종료 시에는
새 ingress를 닫고 이미 수락한 batch의 reservation과 dispatch가 모두 끝난 뒤 lane을 해제한다.

`EntrySpotActorDispatchTests` 136건에서 byte 상한 초과의 request terminal과 payload 반납, Actor별 count
포화, 같은 Actor의 후속 handler 미실행과 sibling Actor progress를 포함해 통과했다. 전체 .NET unit
project는 1,583건 중 1,582건이 통과하고 기존
`LogicalMulticastSubmitsEachPositiveRemoteOnceRegardlessOfWeight` 한 건이 5초 condition timeout으로
실패했지만 같은 test의 단독 재실행은 통과했다. Packaged contract와 standalone HTTP clean consumer가
통과했고 public API snapshot hash는
`c1987f4b98e4fac7a30b7d038a56ee0d20e1272d00e79bdc88d3271e3c3ab958`로 유지되었다. 실제 process
`ToActorMessaging` `TA-A1`은 `logs/20260807-195320-4080230/`에서 통과했다. 이 checkpoint는
`45a3658fd8`로 `main`에 push했다.

### DOTNET-LIFE-001 — User/Instance Spot이 서로 다른 runtime type이 아님

**판정: 완료**

Internals는 Entry, User, Instance Spot의 생성·이동·idle 반환 규칙이 다르므로 세 종류를 서로 다른 타입으로 표현하고, 한 타입의 tag/interface 검사로 구분하지 않도록 정한다(`common/internals/08-object-lifecycle.ko.md:20-38`). 판정 기준은 상속·합성·tagged union 중 어떤 문법을 썼는지가 아니라 불가능한 종류·기능 조합을 만들 수 있는지다.

Entry Spot은 `ZLinkEntrySpotActivation`으로 분리되어 있지만 User와 Instance는 같은 `ZLinkSpotActivation`이 `IZLinkSpotContext`와 `IZLinkInstanceSpotContext`를 동시에 구현한다(`Runtime/Spots/ZLinkSpotActivation.cs:14-19`). 실제 종류는 내부 `Spot` 객체가 `IZLinkInstanceSpot`인지 반복 검사해 가른다(`Runtime/Spots/ZLinkSpotActivationConfiguration.cs:121-166`, `Runtime/Spots/ZLinkSpotActivationExecution.cs:439-448,790`, `Runtime/Spots/ZLinkSpotNodeCatalog.cs:1137-1149,1173-1184`). 공통 타입에는 User 전용 relocation-ready와 Instance 전용 close/handler surface가 함께 존재해 유효 조합을 호출부가 알아야 한다.

완료 조건은 공통 resource ownership을 base/component로 공유하되 User/Instance activation과 허용 lifecycle operation을 타입 경계에서 분리하는 것이다. Instance에 User-only relocation-ready를, User에 Instance-only idle close를 구성할 수 없음을 compile-time 또는 closed-union exhaustive test로 검증해야 한다.

구현과 검증을 완료했다. 공통 socket·scope·timer·serial executor·payload ownership은 abstract
`ZLinkSpotActivation`이 유지하고, factory는 `ZLinkUserSpotActivation`과
`ZLinkInstanceSpotActivation`을 각각 만든다. User activation만 `IZLinkSpotContext`와 전체 handler registry,
Actor leave와 relocation-ready operation을 제공한다. Instance activation은
`IZLinkInstanceSpotContext`와 packet handler registry, Instance initialize·close lifecycle만 제공한다.
따라서 하나의 activation에 두 context interface나 두 handler capability를 함께 구성할 수 없다.

Configuration descriptor bind, scanned handler 허용 범위, closing callback과 Instance initialization은 subtype의
override가 소유한다. Catalog와 retire scheduler가 application `Spot` 객체의 interface를 반복 검사하던 분기도
activation이 제공하는 kind·placement policy로 바꿨다. Instance evidence wait runner는 첫 번째 node의 10초
HTTP timeout 때문에 실제 owner인 두 번째 node를 조회하지 못하던 문제를 함께 고쳐, 두 node의 공개 evidence를
bounded polling한다.

Runtime type·context·handler capability의 상호 배타성과 User/Instance lifecycle을 포함한 focused test 157건이
통과했다. 전체 .NET unit project는 1,584건 중 1,583건이 통과하고 기존
`LogicalMulticastSubmitsEachPositiveRemoteOnceRegardlessOfWeight` 한 건이 5초 condition timeout으로
실패했지만 같은 test의 단독 재실행은 통과했다. Packaged contract와 standalone HTTP clean consumer가
통과했고 public API snapshot hash는
`c1987f4b98e4fac7a30b7d038a56ee0d20e1272d00e79bdc88d3271e3c3ab958`로 유지되었다. 실제 process
`InstanceSpot` `IS-E2E-01`은 `SpotService/logs/20260807-201508-822914/`에서 cold activation,
initialization 1회와 request handler 1회를 확인하고 통과했다. 이 checkpoint는 `4e4b2a626d`로 `main`에
push했다.

### DOTNET-LIFE-002 — Ready Instance owner loss의 process 증거 누락

**판정: 완료**

공개 계약인 failure spec §4.4는 `Ready` authority의 owner process 종료나 lease 만료를 Missing으로 바꾸지 않고,
다른 node의 cold activation 없이 call을 bounded `Unavailable`로 끝내도록 요구한다. Relocation Store의
activation record는 같은 target node와 lifecycle에서 끝나지 않은 initial cold activation만 재개한다
(`common/spec/31-failure-failover-policy.ko.md`, .NET exact interface
`server/languages/dotnet/interfaces/05-spots.ko.md`).

Internals 06·08·10은 이 결과를 resolver의 닫힌 결과, activation state와 liveness 책임 분리로 구현하고,
12는 same-target initial recovery root와 scan을 설명한다. 이 구조 문서들은 공개 오류나 failover 범위를
추가하지 않는다.

현재 source는 이 의미를 이미 구분한다. Instance address lookup은
`ResolveSpotRowWithStatusAsync(...)`의 `KnownUnavailable`을 받으면 `Unavailable`을 던지고 cold activation에
넘기지 않는다 (`Runtime/Host/ZLinkFrameworkRuntimeLocationAccess.cs:39-46`). Unit test도 lease 만료 뒤
row가 null이어도 resolution kind가 `KnownUnavailable`인지 확인한다
(`UnitTests/Runtime/LocationResolverTests.cs:243-263`). 따라서 source 수준의 새 구현 GAP으로 판정하지
않는다.

조사 당시 남은 조건은 실제 process 증거였다. `IS-E2E-05`와 `IS-E2E-35`가 미구현이어서 owner process 종료,
lease invalidation, bounded `Unavailable`, 새 factory·handler 실행 부재와 자동 queue recovery 부재를
검증하지 못했다 (`framework/languages/dotnet/e2e/InstanceSpot/feature-map.ko.md:17,47`). 이 두 E2E와
같은 target node/lifecycle의 미완료 initial cold activation recovery positive scenario가 필요했다.

요구한 process 검증을 구현하고 통과했다. `IS-E2E-05`는 public location query로 Ready owner와
`ObjectGeneration`을 확정한 뒤 실제 owner process를 SIGKILL한다. Owner lease 무효화 뒤 public location은
같은 generation의 `Unavailable`이 되었고, 후속 Instance intent request도 `Unavailable`로 한 번 끝났다.
다른 owner의 handler와 추가 factory initialization은 모두 0건이었다. 실행 log는
`SpotService/logs/20260807-202307-1312454/`다.

`IS-E2E-35`는 Ready Spot의 first handler를 Application gate에서 대기시키고 follow-up request를 같은 queue에
넣은 뒤 owner를 SIGKILL하고 같은 role을 restart했다. 두 caller는 각각 `ShuttingDown`과
`DeadlineExceeded` terminal을 받았으며, queued operation의 handler replay는 0건이었다. Restart 뒤 public
location과 후속 request는 같은 generation의 `Unavailable`이고 factory initialization은 최초 1건뿐이었다.
실행 log는 `SpotService/logs/20260807-202604-1475569/`다.

Ready owner failover와 구분할 positive control도 추가했다. Initial `OnInitializeAsync`를 gate로 대기시켜
public location이 `Creating`인 동안 두 번째 request를 보냈고, 같은 target·generation에 합류한 뒤 gate를
해제했다. Initialization은 한 번, 두 operation handler는 각각 한 번 실행되고 Ready publication까지
generation이 유지되었다. 실행 log는 `SpotService/logs/20260807-202813-1625005/`다. 기존 Track A도
`SpotService/logs/20260807-202935-1718197/`에서 다시 통과했다. 이 E2E checkpoint는
`c797108cce`로 `main`에 push했다.

### DOTNET-COMP-002 — submit 뒤 waiter 등록 구조 유지

Internals는 응답 상관 값을 sender가 먼저 만들고 waiter를 등록한 다음 submit하도록 정한다. Submit이 operation ID를 출력하면 same-process 응답이 등록보다 먼저 도착해 early-result table이 필요해지므로 이 구조 자체를 제거 대상으로 명시한다(`common/internals/04-completion.ko.md:60-105`).

`.NET` wrapper는 `_spot.RequestToSpot(..., out var operationId, ...)`를 먼저 호출한 다음 `_completions.RegisterRequest(...)`를 호출한다(`Runtime/Backend/DotNet/Wrappers/ZLinkBackendSpotWrapper.cs:217-231`). `ZLinkMeshCompletionTable` 주석과 `_early` map도 이 경쟁을 정상 구조로 전제한다(`Runtime/Backend/DotNet/ZLinkMeshCompletionTable.cs:6-13,69-72`). DOTNET-COMP-001은 이 구조가 만든 유한 보관 자원에서 실제 terminal 유실로 이어진다.

완료 조건은 Framework가 correlation ID를 먼저 할당해 input으로 전달하는 binding/public surface와 register-before-submit 회귀 test다. Early store 크기를 늘리는 것은 gap을 닫지 않는다.

구현과 검증을 완료했다. Framework backend는 node가 만든 응답 상관 값을 completion table에 먼저
등록한 뒤 managed submit의 입력으로 전달한다. Spot·Message Follow·Actor request와 join·Instance Spot
activation·remote object operation·STREAM bind/unbind가 같은 `RegisterBeforeSubmit` 경로를 사용한다.
동기 submit이 거절되거나 예외를 던지면 같은 경계에서 waiter를 제거한다. 따라서 dispatch pump가
등록되지 않은 terminal을 먼저 받는 정상 경로가 사라졌고, early payload store와 tombstone store도
제거했다.

회귀 test는 submit callback 안에서 completion을 동기 발생시켜 waiter가 이미 실행 가능한 상태인지
확인하고, submit 거절 뒤 늦게 도착한 payload는 handler에 연결하지 않고 dispose하는지 확인한다.
Completion table focused test 4건과 전체 .NET unit suite 1,582건이 통과했다. 관련 test 묶음에서
`RemoteUserSpotTerminalReplaysAfterDeadlineAndExpiresWithoutReexecution`이 한 번 실패했지만 같은 test의
단독 재실행과 전체 suite 재실행은 통과했다. Packaged contract와 standalone HTTP clean consumer도
통과했으며 public API snapshot hash는
`c1987f4b98e4fac7a30b7d038a56ee0d20e1272d00e79bdc88d3271e3c3ab958`로 유지되었다. 실제 process는
ToActor `TA-A1`을 `ToActorMessaging/logs/20260807-205106-2541281/`에서, Instance Spot Track A를
`SpotService/logs/20260807-205128-2543009/`에서 통과했다. 구현 checkpoint는 `4005fe5c8e`로
`main`에 push했다.

### DOTNET-LAYER-001 — 의미 없는 binding pass-through 계층

**판정: 완료**

Internals의 POSDDD 관문은 binding API를 그대로 복제하는 adapter와 test fake만을 이유로 둔 단일 구현 `IBackend*`를 금지한다(`common/internals/01-layering.ko.md:80-124`). 조사 당시 publisher wrapper는 bind, socket option, send-ready, publish와 dispose를 같은 인자·결과로 전달할 뿐 Framework 의미를 숨기지 않았다(`Runtime/Backend/DotNet/Wrappers/ZLinkBackendPublisherSocketWrapper.cs:3-61`). Router wrapper도 option과 send/recv 대부분을 동일하게 전달했다(`Runtime/Backend/DotNet/Wrappers/ZLinkBackendRouterSocketWrapper.cs:3-167`). 대응 `IZLinkBackend*` interface는 binding surface를 다시 선언했고(`Runtime/Backend/Contracts/IZLinkBackendSocketContracts.cs:9-176`), production 구현은 각각 이 wrapper 하나뿐이었다.

MeshNode/Spot/Stream처럼 ownership, pull-dispatch, completion과 fencing을 결합하는 adapter까지 제거하라는 뜻은 아니다. Socket별 pass-through interface/wrapper를 직접 binding public API 사용으로 바꾸고, 실제 의미 변환만 좁은 adapter에 남겨야 한다. 변경 전후 throughput, p99, allocation과 lock contention을 측정해야 이 구조 gap을 완료할 수 있다.

일반 DEALER, ROUTER, PUB, SUB의 `IZLinkBackend*Socket`과 단일 production wrapper를 제거했다. Runtime
context와 Channel 실행 경로는 binding의 public socket interface를 직접 사용한다. Socket option 변환,
poller와 monitor 연결은 Framework 정책을 적용하는 좁은 adapter로 유지했고, MeshNode·Spot·STREAM adapter는
ownership, completion과 lifecycle 의미를 결합하므로 유지했다. Channel bundle은 socket interface를 다시
선언하지 않고 dispose와 manual connection ownership만 관리하며, 사용되지 않던 automatic connection
bookkeeping도 제거했다. Test는 binding surface를 복제한 fake 대신 실제 socket과 runtime이 기록하는
socket 생성 횟수·monitoring 상태를 사용한다.

같은 host에서 1 KiB payload, request window 100, warmup 1,000회, active 30초 조건으로 변경 전
`40255cd707`과 변경 뒤를 차례로 측정했다. Throughput은 23.05 KOPS에서 27.08 KOPS로 17.49% 증가했고,
mean latency는 3.673 ms에서 3.337 ms로, p99는 101.018 ms에서 99.790 ms로 낮아졌다. `System.Runtime`
counter의 server allocation은 초당 613,769,509 byte에서 676,926,534 byte로, monitor lock contention은
초당 9,761.625회에서 10,812.538회로 늘었다. 처리량 차이를 반영해 완료 1건당 환산하면 allocation은
26,625.74 byte에서 24,994.55 byte로 6.13%, contention은 0.423466회에서 0.399238회로 5.72% 감소했다.
Counter 표본은 변경 전 8개, 변경 뒤 13개이며 throughput·latency와 같은 active 구간에서 수집했다.

관련 owner-layer test 85건과 전체 .NET unit suite 1,582건이 통과했다. Packaged contract와 standalone
HTTP clean consumer가 통과했고
public API snapshot hash는
`c1987f4b98e4fac7a30b7d038a56ee0d20e1272d00e79bdc88d3271e3c3ab958`로 유지되었다. 실제 process는
PubSub `PS-A1`을 `PubSub/logs/20260807-211921-3611470/`에서, ChannelEgressRouting `CH-E2E-01`을
`ChannelEgressRouting/logs/20260807-211942-3612322/`에서 통과했다. 구현 checkpoint는
`8ad6e969cc`로 `main`에 push했다.

### DOTNET-LAYER-002 — runtime 종료 의미가 ASP.NET Core 통합 package에 있음

Internals는 host 통합 계층이 runtime 시작·종료를 host lifecycle에 **연결만** 하고, 수락 중지·drain·relocate·close의 의미와 순서는 runtime이 소유해야 한다고 정한다(`common/internals/01-layering.ko.md:150-173`). .NET package 계약도 `Zlink.Framework`가 location runtime을 소유하고 `Zlink.Framework.AspNetCore`는 DI 등록과 host lifecycle 연결만 담당한다고 구분한다(`interfaces/02-configuration-host.ko.md:12-21`).

실제 public `IZLinkFrameworkRuntime`의 유일한 production 구현은 ASP.NET Core package의 `ZLinkFrameworkMaintenanceRuntime`이다(`Zlink.Framework.AspNetCore/ZLinkFrameworkMaintenanceRuntime.cs:8-44`). 이 타입이 runtime state, relocate/shutdown operation, deadline, observer와 drain coordinator를 직접 소유한다. DI도 이 타입을 `IZLinkFrameworkRuntime`으로 등록한다(`Zlink.Framework.AspNetCore/ZLinkFrameworkServiceRegistrar.cs:145-165`). 반면 core package의 `Runtime/Host/ZLinkFrameworkRuntime`은 Spot manager와 내부 resource를 소유하지만 public maintenance runtime을 구현하지 않는다(`Runtime/Host/ZLinkFrameworkRuntime.cs:31-78`). 따라서 ASP.NET Core 통합 없이 같은 종료·재배치 의미를 조립할 수 없으며 package 책임도 exact 문서와 반대다.

완료 조건은 maintenance state machine과 종료·재배치 순서를 `Zlink.Framework`로 옮기고, ASP.NET Core package에는 hosted-service 연결만 남기는 것이다. Console/test host에서도 같은 runtime API로 동일한 terminal-once와 resource close 순서를 검증해야 한다.

### DOTNET-LAYER-003 — 수명이 다른 식별자를 내부에서도 `string`으로 혼용

Internals는 mesh 이름, node RID, channel 이름, object ID처럼 범위와 수명이 다른 식별자를 각각 전용 타입으로 두고 문자열 변환은 경계에서 한 번만 하도록 정한다(`common/internals/01-layering.ko.md:317-359`). Public exact API가 application 편의를 위해 `string`을 받는 것과 runtime 내부 표현까지 문자열이어야 한다는 것은 다른 문제다.

현재 node RID만 `RoutingId`로 감싸고, Actor runtime의 `ActorId`는 `string`이며 mesh 이름도 record에서 `string`이다(`Runtime/Actors/ZLinkActorRuntimeState.cs:48,1783-1803`). Spot activation은 `ChannelName`, `MeshName`, `SpotId`를 모두 `string`으로 노출한다(`Runtime/Spots/ZLinkSpotActivation.cs:140-142,223`). ClientServer와 Spot registry도 `Dictionary<string,...>`로 서로 다른 식별자 공간을 표현한다(`Runtime/Channels/ZLinkClientServerClientRuntime.cs:16-17`, `Runtime/Spots/ZLinkSpotNodeCatalog.cs:57-64`). 이는 단순 style 선호가 아니라 문서가 구체적으로 금지한 내부 모델이며 잘못된 ID 교차 대입을 compile time에 막지 못한다.

완료 조건은 public/binding/store 경계에서 한 번 validation·변환하고 runtime key와 method parameter에는 `MeshName`, `ChannelName`, `ActorId`, `SpotId` 등 구분된 value type을 사용하는 것이다. 동일 문자열 값이 서로 다른 identifier domain에 있어도 섞이지 않는 compile/runtime test가 필요하다.

### DOTNET-LAYER-004 — STREAM client와 server가 같은 protocol stack을 중복 구현

Internals는 client 접속 library와 Framework가 같은 protocol을 별도 구현하지 않고 protocol 처리 한 곳을 양쪽이 사용하도록 정한다(`common/internals/01-layering.ko.md:175-181`). 이는 public client/server API를 합치라는 뜻이 아니라 wire header, correlation, pending request, lifecycle/close 같은 공통 protocol mechanism의 정본을 하나로 두라는 결정이다.

현재 `Systems.Zlink.Stream.Connector`는 자체 `ZlinkStreamHeaderCodec`, metadata/closing codec, correlation, pending-request table, receive loop와 lifecycle을 구현한다(`Systems.Zlink.Stream.Connector/Runtime/Protocol/`, `Runtime/ZlinkStreamPendingRequests.cs`, `Runtime/ZlinkStreamConnectorLifecycle.cs`). Framework server에는 별도의 `ZLinkStreamHeaderCodec`, `ZLinkStreamSessionClosingCodec`, frame reader/writer, session liveness와 request/reply 경로가 있다(`Zlink.Framework/Runtime/Streams/`). 실제로 같은 request sequence·header validation·closing record를 양쪽 소스에서 각각 유지하므로 한쪽 수정이 다른 쪽에 자동 반영되지 않는다.

완료 조건은 transport와 client/server orchestration은 분리하되 공통 wire codec, frozen validation과 correlation primitives를 shared internal protocol package로 옮기는 것이다. 기존 client-server golden/negative fixture를 shared codec에 한 번 적용하고 connector와 Framework 양쪽 clean consumer가 그 package를 사용하는지 확인해야 한다.

### DOTNET-OWN-001 — 소유한 payload를 public copying factory로 다시 복사

Internals는 binding이 강제하지 않은 framework full-buffer copy를 0으로 줄이고, public immutable payload의 안전성은 유지하되 runtime 내부 소유권 이전에는 복사하지 않는 경로를 별도로 두도록 정한다(`common/internals/11-message-ownership.ko.md:19-45,95-98`).

`ZLinkEncodedPayload.From(...)`의 세 overload는 public caller buffer를 보호하기 위해 모두 `ToArray()`로 복사한다(`Zlink.Framework.Contracts/Codecs/ZLinkEncodedPayload.cs:10-31`). 이 public 동작 자체는 맞다. 문제는 runtime도 이미 자신이 소유한 memory에 같은 factory를 사용한다는 점이다. JSON 송신은 `SerializeToUtf8Bytes`가 새 배열을 만든 직후 `From(byte[])`으로 전체를 한 번 더 복사한다(`Runtime/Messaging/ZLinkMessageRuntime.cs:128-143`). Custom codec 수신도 runtime-owned `_payload`와 STREAM packet memory를 `From(span)`으로 다시 복사한 뒤 serializer에 넘긴다(`Runtime/Messaging/ZLinkMessageRuntime.cs:112-115`, `Runtime/Streams/ZLinkStreamPacketPayloadCodec.cs:56-60`). 이는 source에서 확인한 추가 full-buffer copy이며 실제 throughput/p99 영향 크기는 아직 benchmark하지 않았다.

완료 조건은 public factory의 defensive copy는 유지하면서 friend/internal owned-memory factory 또는 ownership token을 추가하는 것이다. JSON encode와 custom serializer decode에서 payload 크기만큼의 추가 배열·copy가 사라졌음을 allocation/copy benchmark와 buffer lifetime test로 확인해야 한다.

### SPEC-TIMER-001 — non-catch-up timer option 계약 충돌로 판정 보류

Exact interface는 `MaxCatchUpTicks`를 `CatchUpBounded`에서만 사용하고 `1..Int32.MaxValue`로 검증하며, 다른 overrun policy에서는 이 범위로 검증하지 않는다고 정한다. 또한 relocation은 `ZLinkTimerOptions`를 자동으로 포함한다(`interfaces/05-spots.ko.md:499-509`). 등록 경로는 실제로 `CatchUpBounded`일 때만 양수를 검사하므로 이 부분은 맞다(`Runtime/Spots/ZLinkSpotTimerRegistry.cs:333-352`).

하지만 canonical relocation writer는 policy와 무관하게 `Math.Max(1, timer.Options.MaxCatchUpTicks)`를 기록한다(`Runtime/Spots/ZLinkCanonicalSpotRelocationWriter.cs:128-132`). `SkipLateTicks`나 `DelayNextTick`에 0 또는 음수를 준 timer는 등록에는 성공하지만 재배치하면 값이 1로 바뀐다. Canonical decoder도 policy와 무관하게 0을 거부한다(`Runtime/Spots/ZLinkSpotTimerRelocationCodec.cs:118-131`). 같은 파일의 legacy decoder가 `CatchUpBounded`에서만 검증하는 동작(`:73-82`)과도 불일치한다.

그러나 canonical service-wire schema 자체가 `maxCatchUpTicks`를 `nonzero-u64`로 정의한다(`framework/runtime/protocol/service-wire-v1.schema.json:3897`). 따라서 현재 구현은 language exact 의미와는 어긋나지만 canonical wire에는 맞으며, 구현만 고쳐서는 두 정본을 동시에 만족할 수 없다. 이 항목을 확정 implementation gap 수에 넣지 않는다. 쟁점은 non-catch-up에서 사용하지 않는 값을 relocation이 그대로 보존해야 하는지 canonical 값으로 정규화해도 되는지다. Contract가 이를 하나로 정한 뒤 구현과 세 policy round-trip test를 맞춰야 한다.

## 3. 반증 검토와 제외한 후보

각 발견을 “코드 모양이 다르다”는 이유만으로 gap 처리하지 않고, 반대 근거가 있는 후보는 제외했다.

- ClientServer update에 `MaintenanceWave`·placement capacity가 없다는 사실은 gap으로 세지 않았다. Canonical `client-server-admission` schema 자체가 server descriptor를 channel, RID/generation/revision, weight/state, identity, message bound와 endpoint로 닫고 있기 때문이다(`framework/runtime/protocol/service-wire-v1.schema.json:2888-2922`). Internals의 generic mutable-field 문장만 떼어 schema보다 넓게 적용하지 않았다.
- StreamNode의 외부 STREAM C→S runtime 의미는 64 KiB 단방향 계약과 일치한다. Registration은 전용 64 KiB 기본값을 사용하고(`Runtime/Configuration/ZLinkFrameworkRegistration.cs:295-315`), receive buffer가 prefix를 제외한 header+payload만 검사하며(`Runtime/Streams/ZLinkStreamReceiveBuffer.cs:34-52`), server outbound writer에는 이 설정의 크기 검사가 없다. 다만 public 설정 표면은 DOTNET-API-003으로 분리한 fluent API gap이므로 전체 종결로 판정하지 않는다. 이 계약은 ClientServer Channel이나 RouteMesh SS에 적용하지 않는다.
- Message Follow는 type/handler 존재만 본 것이 아니라 source peer lifecycle fence, object identity/generation, hop/count/byte bounds와 cache invalidation fence test가 있는 경로를 확인했다. 이번 재검토에서 그 종결 판정을 뒤집을 반례는 찾지 못했다.

## 4. 이번 검토에서 source/unit 범위 종결을 확인한 과거 후보

과거 gap 기록을 그대로 재사용하지 않고 현재 source에서 다시 확인했다.

- RouteMesh inbound HWM: process와 owner 회계를 분리하는 결정(`common/internals/08-object-lifecycle.ko.md:219-268`)에 대해 managed node/pump가 `ZLinkInboundDispatchBudget` lease를 유지하고(`Runtime/Service/ZLinkManagedMeshNode.cs:3700-3760`, `Runtime/Backend/DotNet/ZLinkMeshDispatchPump.cs:385-480`), RouteMesh mailbox pause/resume assertion이 있다(`UnitTests/Runtime/StatefulServiceRuntimeTests.cs:471-590`).
- Weighted selection: smooth weighted round-robin과 stable RID tie-break 결정(`common/internals/06-routing-and-cache.ko.md:203-255`)을 공통 selection plan이 구현하고(`Runtime/Channels/ZLinkWeightedSelector.cs`, `Runtime/Channels/ZLinkWeightedSelectionPlan.cs`), exact order·tie-break·candidate replacement assertion이 있다(`UnitTests/Runtime/WeightContractTests.cs:61-230`).
- Instance Spot idle eviction: bounded scan 결정에 대해 catalog가 64개 batch와 cursor wrap을 사용하고(`Runtime/Spots/ZLinkSpotNodeCatalog.cs:29,580-638`), 연속 batch가 뒤 후보까지 도달하는 assertion이 있다(`UnitTests/Runtime/StatefulServiceRuntimeTests.cs:2402-2455`).
- Observer queue: intermediate coalescing과 bounded terminal loss 공개 결정에 대해 별도 terminal queue와 loss count를 구현하고(`Runtime/Diagnostics/ZLinkObservationQueue.cs`), terminal 보존·oldest discard·subscriber loss assertion이 있다(`UnitTests/Runtime/ZLinkObservationQueueTests.cs:6-85`).
- Cross-owner session rebind: 이전 binding 정리 확인 뒤 새 binding을 확정하는 결정(`common/internals/09-session-binding.ko.md:61-100`)에 대해 tombstone/publish 단계가 분리되어 있고(`Runtime/Host/ZLinkActorBoundSessionCoordinator.cs:560-624`), tombstone ACK가 source swap보다 앞서는 assertion이 있다(`UnitTests/Runtime/SessionActorCoordinatorTests.cs:983-1075`).
- Message Follow: route fence와 cache invalidation 결정(`common/internals/12-service-wire-protocol.ko.md:185-210`)에 대해 admitted-peer handler와 exact route invalidation이 있고(`Runtime/Service/ZLinkManagedMeshNode.cs:543-565`, `Runtime/Locations/ZLinkStoreLocationResolvers.cs:294-350`), generation fence·matching cache-only invalidation assertion이 있다(`UnitTests/Runtime/ActorHandoffTests.cs:627-680`, `UnitTests/Runtime/LocationResolverTests.cs:90-125`).

이 항목들은 source와 표시한 unit assertion 범위에서 해당 결정과 일치한다는 판정이다. 실제 multi-process E2E 전체 통과나 package/clean-consumer 종결을 뜻하지 않는다.

## 5. 검증 결과와 남은 gate

| 검증 | 결과 | 해석 |
|---|---|---|
| service wire schema validator self-test | 통과: 41 commands, 234 negative self-tests | schema/golden asset 유효성만 확인. .NET JSON consumer parity는 확인하지 않음 |
| `.NET` contract test | 실패: 74 passed, 2 failed | object-location exact interface export 누락을 직접 검출 |
| Sol High 독립 반증 리뷰 | 기존 14건 유지, 2건 추가, timer 1건 보류 유지 | `gpt-5.6-sol` high가 기존 판정을 제거할 반례 없이 ROLE-001과 EXEC-003 누락을 검출. 이후 사용자 확정 S→S 계약으로 SIZE-001, StreamNode fluent API 계약으로 API-003을 추가했고 main reviewer가 현재 source에서 재확인 |
| internals 집중 unit test | build 실패 | 현재 worktree의 `EndpointConnectionsTests.cs:5`가 존재하지 않는 `NativeRuntimeCollection`을 참조해 test 실행 전 `CS0103` 발생 |
| packaged-contract/clean consumer | 미실행 | exact-interface gate가 먼저 실패했으므로 package 일치가 semantic gap을 바꾸지 않음 |
| process E2E | 미실행 | 정적·unit 경계에서 확정한 위 gap을 닫은 뒤 별도 실행 필요 |

Contract test와 unit test를 동시에 build했을 때 shared output assembly copy retry warning도 발생했다. 최종 재검증은 build output을 분리하거나 serial로 실행해야 한다. 이 warning은 위 구현 gap의 근거로 사용하지 않았다.

## 6. 권장 수정 순서

1. Public exact interface gap 세 건과 Server-only ClientServer role error를 source, fixed API snapshot과 clean consumer까지 맞춘다.
2. RouteMesh SS public `MaxMessageSize`, admission field, native receive cap과 sender check를 제거하고 API snapshot·wire fixture로 회귀를 고정한다. ClientServer의 기존 negotiated complete-message bound는 send/request/reply submit 전에 강제한다.
3. `framework-json-v1` golden fixture를 실제 .NET decode path에 연결하고 strict profile을 구현한다. STREAM per-session byte reservation을 production admission 경로에서 강제한다. Entry Actor ingress의 unbounded Channel/task chain을 bounded lane으로 바꾸고 직렬 실행 기관을 공통 engine과 lane policy로 수렴한다.
4. Correlation ID를 register-before-submit 구조로 옮긴 뒤 completion early store를 제거한다. 전환 중에는 전체 overflow에서도 caller `CapacityExceeded`를 보장한다.
5. Ready Instance owner loss의 `IS-E2E-05`와 `IS-E2E-35`를 추가해 현재 `KnownUnavailable` source 의미가 실제 process에서도 새 activation이나 queue recovery 없이 유지되는지 검증한다. Timer option 정본 충돌을 먼저 해소하고 그 결정대로 relocation round-trip을 맞춘다. Maintenance runtime은 core package로 옮긴다.
6. Identifier value type 전환, STREAM shared protocol package와 payload owned-memory 경로를 적용하고 compile-time misuse test, 공통 golden test 및 allocation benchmark를 남긴다.
7. 의미 없는 socket backend wrapper를 걷어내고 남는 semantic adapter의 POSD/DDD 책임과 성능 근거를 기록한다.
8. Contract, full unit, packaged contract, clean consumer와 실제 process E2E를 각각 실행한다. 한 gate의 성공으로 다른 gate를 대체하지 않는다.
