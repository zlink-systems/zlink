# .NET etcd client dependency inventory

The [multi-target lockfile](packages.lock.json) and [net8.0 lockfile](packages.net8.lock.json) pin the resolved provider dependencies, including the build-only Grpc.Tools package. The license declarations below come from the nuspec metadata in each restored NuGet package.

| Package | Version | Declared license | Source |
|---|---|---|---|
| Google.Protobuf | 3.30.0 | BSD-3-Clause | `google.protobuf.nuspec` |
| Grpc.Core.Api | 2.62.0 | Apache-2.0 | `grpc.core.api.nuspec` |
| Grpc.Net.Client | 2.62.0 | Apache-2.0 | `grpc.net.client.nuspec` |
| Grpc.Net.Common | 2.62.0 | Apache-2.0 | `grpc.net.common.nuspec` |
| Grpc.Tools | 2.62.0 | Apache-2.0 | `grpc.tools.nuspec` |
| Microsoft.Extensions.Logging.Abstractions | 6.0.0 | MIT | `microsoft.extensions.logging.abstractions.nuspec` |

The nuspec files above are extracted from the corresponding NuGet packages. The official etcd proto licenses are recorded in [THIRD_PARTY_NOTICES.md](../../../../../THIRD_PARTY_NOTICES.md).
