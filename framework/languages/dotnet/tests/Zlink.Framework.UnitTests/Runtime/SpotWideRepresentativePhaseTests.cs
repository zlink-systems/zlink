using System.Text.Json;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class RelocationBehaviorConformanceTests
{
    [Fact]
    public Task SpotWide_source_persists_the_shared_captured_record_only_on_the_representative()
    {
        var provider = new ZLinkInMemoryProviderLocationStore();
        return WithSpotWidePrepareAsync(
            provider,
            async (store, spotKey, memberKey, originalSpot, originalMember, probe) =>
            {
                var captured = await ReadSpotWideAuthorityAsync(store, spotKey);
                var relocation = Assert.IsType<ZLinkAuthorityRelocation>(captured.Relocation);
                var row = Assert.IsType<ZLinkStoreReadResult.Found>(
                    await provider.ReadAsync(
                        ZLinkProviderLocationRepository.AuthorityMetaKey(spotKey)
                    )
                );
                using var document = JsonDocument.Parse(row.Value.Bytes);
                var aggregate = document.RootElement.GetProperty("aggregate");
                Assert.Equal(relocation, ZLinkAuthorityAggregateCodec.Read(aggregate));
                using var golden = JsonDocument.Parse(
                    File.ReadAllBytes(
                        FindRepositoryFile(
                            "framework/runtime/protocol/golden/authority-aggregate-relocation-v1.json"
                        )
                    )
                );
                var capturedFixture = golden
                    .RootElement.GetProperty("cases")
                    .EnumerateArray()
                    .Single(item =>
                        item.GetProperty("name").GetString() == "relocation-only-captured"
                    )
                    .GetProperty("aggregate")
                    .GetProperty("relocation");
                var stored = aggregate.GetProperty("relocation");
                Assert.Equal(
                    capturedFixture.GetProperty("phase").GetString(),
                    stored.GetProperty("phase").GetString()
                );
                Assert.Equal(
                    capturedFixture.GetProperty("target").ValueKind,
                    stored.GetProperty("target").ValueKind
                );
                Assert.Equal(probe.RelocationId, relocation.RelocationId);
                Assert.Equal(ServiceWireCodec.RelocationPhase.Captured, relocation.Phase);
                Assert.Equal(
                    originalSpot.StoreVersion,
                    relocation.Coordinator.ExpectedStoreVersion
                );
                Assert.NotEqual(originalSpot.StoreVersion, captured.StoreVersion);
                var member = await ReadSpotWideAuthorityAsync(store, memberKey);
                Assert.Null(member.Relocation);
                Assert.Equal(originalMember.StoreVersion, member.StoreVersion);
            }
        );
    }

    private static string FindRepositoryFile(string path)
    {
        var directory = new DirectoryInfo(AppContext.BaseDirectory);
        while (!File.Exists(Path.Combine(directory.FullName, path)))
            directory = directory.Parent!;
        return Path.Combine(directory.FullName, path);
    }
}
