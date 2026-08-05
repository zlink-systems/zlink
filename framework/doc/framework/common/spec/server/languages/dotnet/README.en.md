# ZLink Framework .NET Public Contract

This directory owns the exact .NET public interface of the server
framework. The language-neutral meaning of a feature is defined by the
[common spec](../../../README.en.md), and this directory fixes the C#
types, methods, generic constraints, nullable, and async return types.

| Document | Contract owned |
|---|---|
| [Exact interface table of contents](interfaces/README.ko.md) | Defines the C# public type, member, nullable annotation, generic constraint, and default per feature. |
| [Configuration and host](interfaces/02-configuration-host.ko.md) | Defines the ASP.NET Core registration method, package boundary, DI, and startup contract. |
| [Topology configuration](interfaces/03-configuration-topology.en.md) | Defines RouteMesh, ClientServer, and fanout builder and runtime options. |
| [Location configuration and operations](interfaces/08-location-maintenance.ko.md) | Defines application-facing Location options, readiness, and operational queries. |
| [Location/Relocation provider](interfaces/08-authority-relocation.ko.md) | Defines the generic atomic Location Store and immutable Relocation Store SPI. |
| [Host monitoring](interfaces/10-topology-monitoring.en.md) | Defines host state, Relocate/Shutdown results, and operational status. |

The Stream connector client is a separate package, and the
[.NET Stream Connector Contract](../../../stream-connector/languages/dotnet/03-stream-connector.en.md)
owns its exact interface.

## Contract Application Rules

- [RouteMesh](../../../01-glossary.en.md#routemesh) registration starts
  with `AddRouteMesh(meshName)` and fixes the role with
  `Channel(channelName).Client()` or `.Server()`. A MeshNode with no
  Server membership is also allowed.
- Channel send/request only takes a ChannelName and picks the
  process-local RouteMesh or ClientServer send path.
- The Node direct handler and the
  [ChannelName](../../../01-glossary.en.md#channelname) handler use
  different interface families.
- A typed payload is serialized as JSON by default. A codec doesn't need
  to be registered per message type to use JSON.
- Metadata is delivered to the handler as an immutable
  `ZLinkMessageMetadata`
  [snapshot](../../../01-glossary.en.md#snapshot).
- Object role is one of `None`, `Client`, `Server` per
  [MeshNode](../../../01-glossary.en.md#meshnode), and Client and Server
  explicitly register an `IZLinkLocationStore` implementation. The
  official Redis package the framework provides is one of this
  interface's providers — using Redis itself isn't a required condition
  for Object role.
- A regular message to an Actor/User Spot/Instance
  [Spot](../../../01-glossary.en.md#spot) only takes a global ID. The
  manager create for Actor and User Spot takes stable type and an
  optional Mesh/placement, performs remote placement, and the exact
  mutation takes an `ActorRef` or `SpotRef`. A Missing
  [Instance Spot](../../../01-glossary.en.md#entry-user-instance-spot)
  specifies activation on the Spot-dedicated fluent call.
- Host lifecycle is owned by `IZLinkFrameworkRuntime`'s
  `RelocateAsync(...)` and `ShutdownAsync(...)`.
- The Framework service runtime only uses the bindings' public raw
  socket API, and doesn't use the Core service C API, private SPI,
  reflection, or a direct native symbol call.
- The per-target ROUTER submit for Logical Multicast and the meaning of
  manual peer's expected RID are owned by
  [Topology Configuration](interfaces/03-configuration-topology.en.md).

## Cancellation

A .NET async operation only receives explicit cancellation when its
signature has a `CancellationToken`. A method with no token isn't
interpreted as having a cancellation argument. The terminal result after
cancellation follows the
[Async Execution Policy](../../../05-async-execution-policy.en.md).

## Verification

Contract tests compare the public exports of the source assembly and the
actual NuGet package against this directory's signatures. Nullable
annotations, defaults, generic constraints, and overloads are also part
of the contract.

## Regression Tests

| Test | Verification scope |
|---|---|
| `ContractSurfaceCoverage.Fixed_spec_snapshot_matches_every_exported_contract_signature` | Confirms the formal spec snapshot matches the public signatures of the source and package. |
| `RegressionTests.DotNetContractRegressionTestReferences_Resolve_ToActiveTestMethods` | Confirms the regression tests and E2E scenarios the document points to exist in the current test tree. |
