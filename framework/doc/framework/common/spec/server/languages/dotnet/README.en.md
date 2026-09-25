# ZLink Framework .NET Public Contract

<!-- framework-adapter-nav:start -->
[Language interface table of contents](../README.en.md) | [Spec table of contents](../../README.en.md)
<!-- framework-adapter-nav:end -->
This directory owns the .NET public interface of the server
framework. The language-neutral meaning of a feature is defined by the
[common spec](../../README.en.md), and this directory fixes the C#
types, methods, generic constraints, nullable, and async return types.

| Document | Contract owned |
|---|---|
| [ interface table of contents](interfaces/README.en.md) | Defines the C# public type, member, nullable annotation, generic constraint, and default per feature. |
| [Configuration and host](interfaces/02-configuration-host.en.md) | Defines the ASP.NET Core registration method, package boundary, DI, and startup contract. |
| [Topology configuration](interfaces/03-configuration-topology.en.md) | Defines RouteMesh, ClientServer, and fanout builder and runtime options. |
| [Location configuration and operations](interfaces/08-location-maintenance.en.md) | Defines application-facing Location options, readiness, and operational queries. |
| [Location/Relocation provider](interfaces/08-authority-relocation.en.md) | Defines the generic atomic Location Store and immutable Relocation Store SPI. |
| [Host monitoring](interfaces/10-topology-monitoring.en.md) | Defines host state, Relocate/Shutdown results, and operational status. |

The Stream connector client is a separate package, and the
[.NET Stream Connector Contract](../../../stream-connector/languages/dotnet/03-stream-connector.en.md)
owns its per-language interface.

## Contract Application Rules

The [Framework API](../../00-foundation/06-framework-api.en.md),
[Channel topology](../../02-channel-transport/01-channel-topology.en.md),
[Spot model](../../03-spot-actor/01-spot-model.en.md),
[Actor model](../../03-spot-actor/04-actor-model.en.md), and
[Submit and completion](../../01-execution/01-submit-and-completion.en.md)
define common behavior. The interface documents below define .NET types and signatures.

## Cancellation

A .NET async operation only receives explicit cancellation when its
signature has a `CancellationToken`. A method with no token isn't
interpreted as having a cancellation argument. The terminal result after
cancellation follows the
[Async Execution Policy](../../01-execution/README.en.md).

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

---
<!-- framework-adapter-nav:bottom:start -->
[Language interface table of contents](../README.en.md) | [Spec table of contents](../../README.en.md)
<!-- framework-adapter-nav:bottom:end -->
