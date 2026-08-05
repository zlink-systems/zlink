<!-- framework-adapter-nav:start -->
[문서 목록](../../../README.ko.md) | [Runtime Lifecycle](../../common/internals/README.ko.md)
<!-- framework-adapter-nav:end -->

# .NET v11 public boundary

[Exact interface](../../common/spec/server/languages/dotnet/interfaces/README.ko.md) ·
[Runtime lifecycle](../../common/internals/README.ko.md)

## 1. 목적

이 문서는 Core service runtime을 Framework로 옮긴 뒤 `.NET` public boundary에 남는 항목을 분류한다.
정확한 signature와 enum 숫자는 [exact interface](../../common/spec/server/languages/dotnet/interfaces/README.ko.md)가
소유한다.

## 2. Core 이관 경계

Framework는 bindings의 public raw socket API만 사용한다. Core service object, private member,
reflection과 native symbol 우회 option은 public API에 노출하지 않는다.

| 구분 | v11 public boundary |
|---|---|
| Application messaging | Channel, Spot, Actor와 STREAM의 typed builder·handler |
| Object location | global `SpotId`, `ActorId`와 immutable `SpotRef`·`ActorRef` |
| Host lifecycle | `IZLinkFrameworkRuntime.RelocateAsync(...)`, `ShutdownAsync(...)`와 status/result |
| Provider SPI | `IZLinkLocationStore`, `IZLinkRelocationStore` |
| Redis extension | `ZLinkRedisLocationStore`, `ZLinkRedisRelocationStore` |
| Internal only | authority record, reservation, recovery state machine, wire command와 raw socket |

Actor·Spot application은 provider record나 wire command를 알 필요가 없다. Provider도 Framework의 private
record type을 구현하지 않고 두 Store SPI가 받는 key와 opaque bytes만 저장한다.

## 3. Relocation과 shutdown

`RelocateAsync(...)`는 stateful workload를 옮기고 infrastructure를 `Relocated` 상태로 유지한다.
`ShutdownAsync(...)`는 workload relocation을 시작하지 않고 host를 종료한다. Application은 continuity가
필요하면 relocation 성공을 확인한 뒤 shutdown을 별도로 호출한다.

Relocation mode는 두 가지다.

| Mode | Target |
|---|---|
| `PlannedMaintenance` | 같은 application version의 eligible node |
| `RollingUpdate` | option에 지정한 exact application version의 eligible node |

Actor·Spot factory는 `DisableRelocation`, `RecreateOnRelocation` 또는 `PreserveStateWith` policy를 고정한다.
`PreserveStateWith` adapter는 opaque
`byte[]` application state만 capture·restore한다. Authority phase, participant metadata, accepted journal,
queue와 timer 복원은 Framework 내부 책임이다.

## 4. Store 경계

Location Store는 owner, generation, relocation phase와 payload reference를 원자적으로 공개한다.
Relocation Store는 application state, accepted journal, queue와 timer payload를 immutable root로 저장한다.
두 Store 사이의 distributed transaction은 요구하지 않는다.

Framework는 payload를 Relocation Store에 먼저 기록하고 검증한다. 그다음 Location Store의 한 CAS로
reference를 공개한다. 공개되지 않은 payload는 retention 정책으로 정리한다.

## 5. 회귀 테스트

| 테스트 케이스 | 확인 기준 |
|---|---|
| `FrameworkRuntimeContracts.Public_values_match_the_exact_contract` | runtime·relocation enum 숫자가 exact interface와 일치한다. |
| `FrameworkRuntimeContracts.Relocation_and_shutdown_are_separate_host_operations` | relocation과 shutdown이 별도 public operation이다. |
| `FrameworkRuntimeContracts.Retire_surface_is_not_public` | 이전 host maintenance method가 public interface에 다시 노출되지 않는다. |
| `ProviderStoreContracts.Location_provider_exposes_only_opaque_store_operations` | Location provider가 opaque Store operation만 구현한다. |
| `ProviderStoreContracts.Relocation_provider_exposes_only_immutable_blob_operations` | Relocation provider가 immutable payload Store operation만 구현한다. |
| `ScaffoldSmokeTests.PublicSurface_DoesNotExpose_BackendConcreteTypes` | raw backend service object가 application public API에 노출되지 않는다. |

---
<!-- framework-adapter-nav:bottom:start -->
[문서 목록](../../../README.ko.md) | [Runtime Lifecycle](../../common/internals/README.ko.md)
<!-- framework-adapter-nav:bottom:end -->
