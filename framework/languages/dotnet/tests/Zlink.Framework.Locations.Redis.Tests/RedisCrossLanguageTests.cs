namespace Zlink.Framework.Locations.Redis.Tests;

/// <summary>
/// The smoke runner executes this provider check against an isolated Redis
/// namespace. The provider remains responsible only for the opaque SPI and
/// does not expose Framework domain DTOs.
/// </summary>
[Collection(RedisTestCollection.Name)]
public sealed class RedisProviderSmokeTests(RedisTestFixture fixture)
{
    [SkippableFact]
    public async Task Dotnet_Opaque_Store_Round_Trip()
    {
        Skip.IfNot(fixture.RedisAvailable, fixture.SkipReason);
        await using var store = fixture.CreateStore();
        var alphaKey = new ZLinkStoreKey("golden/dotnet/alpha");
        var betaKey = new ZLinkStoreKey("golden/dotnet/beta");

        Assert.IsType<ZLinkStoreWriteResult.Applied>(
            await store.WriteAsync(new ZLinkStoreWriteRequest(
                [
                    new ZLinkStoreCondition.Missing(alphaKey),
                    new ZLinkStoreCondition.Missing(betaKey)
                ],
                [
                    new ZLinkStoreMutation.Put(
                        alphaKey,
                        new byte[] { 0, 1, 255 },
                        null),
                    new ZLinkStoreMutation.Put(
                        betaKey,
                        System.Text.Encoding.UTF8.GetBytes("dotnet-opaque-value"),
                        null)
                ])));

        var alpha = Assert.IsType<ZLinkStoreReadResult.Found>(
            await store.ReadAsync(alphaKey));
        Assert.Equal(new byte[] { 0, 1, 255 }, alpha.Value.Bytes.ToArray());
        var beta = Assert.IsType<ZLinkStoreReadResult.Found>(
            await store.ReadAsync(betaKey));
        Assert.Equal(
            "dotnet-opaque-value",
            System.Text.Encoding.UTF8.GetString(beta.Value.Bytes.Span));

        var conflict = Assert.IsType<ZLinkStoreWriteResult.Conflict>(
            await store.WriteAsync(new ZLinkStoreWriteRequest(
                [new ZLinkStoreCondition.Missing(alphaKey)],
                [new ZLinkStoreMutation.Put(
                    alphaKey,
                    System.Text.Encoding.UTF8.GetBytes("must-not-commit"),
                    null)])));
        Assert.NotNull(conflict);
        var unchanged = Assert.IsType<ZLinkStoreReadResult.Found>(
            await store.ReadAsync(alphaKey));
        Assert.Equal(new byte[] { 0, 1, 255 }, unchanged.Value.Bytes.ToArray());
    }
}
