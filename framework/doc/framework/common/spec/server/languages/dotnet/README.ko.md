# ZLink Framework .NET 공개 계약

<!-- framework-adapter-nav:start -->
[언어별 interface 목차](../README.ko.md) | [스펙 목차](../../README.ko.md)
<!-- framework-adapter-nav:end -->
이 디렉토리는 server framework의 정확한 .NET public interface를 소유한다. 기능의 언어 중립 의미는
[공통 스펙](../../README.ko.md)이 정의하고, 이 디렉토리는 C# 타입, 메서드, generic 제약, nullable과
비동기 반환 타입을 고정한다.

| 문서 | 소유하는 계약 |
|---|---|
| [언어별 interface 목차](interfaces/README.ko.md) | 기능별 C# public type, member, nullable annotation, generic constraint와 기본값을 정의한다. |
| [Configuration과 host](interfaces/02-configuration-host.ko.md) | ASP.NET Core 등록 방법, package 경계, DI와 startup 계약을 정의한다. |
| [Topology configuration](interfaces/03-configuration-topology.ko.md) | RouteMesh, ClientServer와 fanout builder 및 runtime option을 정의한다. |
| [Location 설정과 운영](interfaces/08-location-maintenance.ko.md) | Application용 Location option, readiness와 운영 query를 정의한다. |
| [Location·Relocation provider](interfaces/08-authority-relocation.ko.md) | Generic atomic Location Store와 immutable Relocation Store SPI를 정의한다. |
| [Host monitoring](interfaces/10-topology-monitoring.ko.md) | Host state, Relocate·Shutdown result와 운영 status를 정의한다. |

Stream connector client는 별도 package이며
[.NET Stream connector 계약](../../../stream-connector/languages/dotnet/03-stream-connector.ko.md)이
정확한 interface를 소유한다.

## 계약 적용 규칙

공통 동작은 [Framework API](../../00-foundation/06-framework-api.ko.md),
[Channel topology](../../02-channel-transport/01-channel-topology.ko.md),
[Spot 모델](../../03-spot-actor/01-spot-model.ko.md),
[Actor 모델](../../03-spot-actor/04-actor-model.ko.md)과
[Submit과 completion](../../01-execution/01-submit-and-completion.ko.md)이 정한다.
아래 interface 문서는 .NET 타입과 시그니처를 정의한다.

## 취소

.NET 비동기 operation은 시그니처에 `CancellationToken`이 있을 때만 명시적 취소를 받는다. Token이 없는
메서드에 취소 인자가 있다고 해석하지 않는다. 취소 후의 terminal 결과는
[비동기 실행 정책](../../01-execution/README.ko.md)을 따른다.

## 검증

Contract test는 source assembly와 실제 NuGet package의 public export를 이 디렉토리의 시그니처와
비교한다. Nullable annotation, 기본값, generic 제약과 overload도 계약에 포함한다.

## 회귀 테스트

| 테스트 | 확인 범위 |
|---|---|
| `ContractSurfaceCoverage.Fixed_spec_snapshot_matches_every_exported_contract_signature` | 정식 spec snapshot과 source·package의 공개 서명이 일치하는지 확인한다. |
| `RegressionTests.DotNetContractRegressionTestReferences_Resolve_ToActiveTestMethods` | 문서가 가리키는 회귀 테스트와 E2E 시나리오가 현재 test tree에 존재하는지 확인한다. |

---
<!-- framework-adapter-nav:bottom:start -->
[언어별 interface 목차](../README.ko.md) | [스펙 목차](../../README.ko.md)
<!-- framework-adapter-nav:bottom:end -->
