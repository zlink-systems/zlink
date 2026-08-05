# ZLink Framework .NET 공개 계약

이 디렉토리는 server framework의 정확한 .NET public interface를 소유한다. 기능의 언어 중립 의미는
[공통 스펙](../../../README.ko.md)이 정의하고, 이 디렉토리는 C# 타입, 메서드, generic 제약, nullable과
비동기 반환 타입을 고정한다.

| 문서 | 소유하는 계약 |
|---|---|
| [Exact interface 목차](interfaces/README.ko.md) | 기능별 C# public type, member, nullable annotation, generic constraint와 기본값을 정의한다. |
| [Configuration과 host](interfaces/02-configuration-host.ko.md) | ASP.NET Core 등록 방법, package 경계, DI와 startup 계약을 정의한다. |
| [Topology configuration](interfaces/03-configuration-topology.ko.md) | RouteMesh, ClientServer와 fanout builder 및 runtime option을 정의한다. |
| [Location 설정과 운영](interfaces/08-location-maintenance.ko.md) | Application용 Location option, readiness와 운영 query를 정의한다. |
| [Location·Relocation provider](interfaces/08-authority-relocation.ko.md) | Generic atomic Location Store와 immutable Relocation Store SPI를 정의한다. |
| [Host monitoring](interfaces/10-topology-monitoring.ko.md) | Host state, Relocate·Shutdown result와 운영 status를 정의한다. |

Stream connector client는 별도 package이며
[.NET Stream connector 계약](../../../stream-connector/languages/dotnet/03-stream-connector.ko.md)이
정확한 interface를 소유한다.

## 계약 적용 규칙

- [RouteMesh](../../../01-glossary.ko.md#routemesh) 등록은 `AddRouteMesh(meshName)`으로 시작하고 `Channel(channelName).Client()` 또는
  `.Server()`로 역할을 정한다. Server membership이 없는 MeshNode도 허용한다.
- Channel send/request는 ChannelName만 받고 process-local RouteMesh 또는 ClientServer 송신 경로를 고른다.
- Node direct handler와 [ChannelName](../../../01-glossary.ko.md#channelname) handler는 서로 다른 interface family를 사용한다.
- typed payload는 JSON을 기본으로 직렬화한다. JSON 사용을 위해 message type마다 codec을 등록하지 않는다.
- metadata는 handler에 변경할 수 없는 `ZLinkMessageMetadata` [snapshot](../../../01-glossary.ko.md#snapshot)으로 전달한다.
- Object role은 [MeshNode](../../../01-glossary.ko.md#meshnode)마다 `None`, `Client`, `Server` 중 하나이며 Client와 Server는
  `IZLinkLocationStore` 구현을 명시적으로 등록한다. Framework가 제공하는 공식 Redis package는 이
  interface의 provider 가운데 하나이며 Redis 사용 자체가 Object role의 필수 조건은 아니다.
- Actor·User Spot·Instance [Spot](../../../01-glossary.ko.md#spot)의 일반 message는 global ID만 받는다. Actor와 User Spot의 manager create는
  stable type과 optional Mesh·placement를 받고 remote placement를 수행하며 exact mutation은 `ActorRef` 또는
  `SpotRef`를 받는다. Missing [Instance Spot](../../../01-glossary.ko.md#entry-spot-user-spot과-instance-spot)은 Spot 전용 fluent call에서 activation을 명시한다.
- Host lifecycle은 `IZLinkFrameworkRuntime`의 `RelocateAsync(...)`와 `ShutdownAsync(...)`가 소유한다.
- Framework service runtime은 bindings의 public raw socket API만 사용하고 Core service C API, private SPI,
  reflection과 native symbol 직접 호출을 사용하지 않는다.
- Logical Multicast의 target별 ROUTER 제출과 manual peer의 expected RID 의미는
  [Topology configuration](interfaces/03-configuration-topology.ko.md)이 소유한다.

## 취소

.NET 비동기 operation은 시그니처에 `CancellationToken`이 있을 때만 명시적 취소를 받는다. Token이 없는
메서드에 취소 인자가 있다고 해석하지 않는다. 취소 후의 terminal 결과는
[비동기 실행 정책](../../../05-async-execution-policy.ko.md)을 따른다.

## 검증

Contract test는 source assembly와 실제 NuGet package의 public export를 이 디렉토리의 시그니처와
비교한다. Nullable annotation, 기본값, generic 제약과 overload도 계약에 포함한다.

## 회귀 테스트

| 테스트 | 확인 범위 |
|---|---|
| `ContractSurfaceCoverage.Fixed_spec_snapshot_matches_every_exported_contract_signature` | 정식 spec snapshot과 source·package의 공개 서명이 일치하는지 확인한다. |
| `RegressionTests.DotNetContractRegressionTestReferences_Resolve_ToActiveTestMethods` | 문서가 가리키는 회귀 테스트와 E2E 시나리오가 현재 test tree에 존재하는지 확인한다. |
