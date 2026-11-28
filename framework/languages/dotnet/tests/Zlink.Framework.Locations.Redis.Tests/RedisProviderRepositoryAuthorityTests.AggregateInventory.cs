using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.Locations.Redis.Tests;

public sealed partial class RedisProviderRepositoryAuthorityTests
{
    [SkippableFact]
    public async Task DirectJoinInventoryRoundTripsAtGenerationOne()
    {
        Skip.IfNot(fixture.RedisAvailable, fixture.SkipReason);
        await using var provider = fixture.CreateStore();
        var writer = new ZLinkProviderLocationRepository(provider);
        var reader = new ZLinkProviderLocationRepository(provider);
        var id = Guid.NewGuid();
        var fence = ZLinkAggregateInventory.RelocationFence(id);
        var key = ZLinkActorAuthorityPayloadCodec.AuthorityKey("direct-join");
        var payload = new byte[] { 0, 127, 255 };
        var participant = ZLinkDirectRelocationInventory.Participant(key, "original", payload);
        var digest = await writer.SealAggregateInventoryAsync(fence, [participant], default);

        Assert.Equal(1UL, fence.AggregateGeneration);
        Assert.IsType<ZLinkStoreReadResult.Found>(
            await provider.ReadAsync(new($"zlink:v11:aggregate-inventory:{id:D}:1:root"))
        );
        Assert.Equal(
            digest,
            await writer.SealAggregateInventoryAsync(fence, [participant], default)
        );
        var stored = Assert.Single(
            await reader.ReadAggregateInventoryAsync(fence, digest, default)
        );
        Assert.Equal(key.Value, stored.Entry.AuthorityKey);
        Assert.Equal("original", stored.Entry.ExpectedStoreVersion);
        Assert.Equal(payload, stored.Payload.ToArray());
    }

}
