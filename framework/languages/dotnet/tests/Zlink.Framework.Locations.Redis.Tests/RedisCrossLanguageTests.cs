namespace Zlink.Framework.Locations.Redis.Tests;

/// <summary>
/// The smoke runner executes this provider check against an isolated Redis
/// namespace. The provider remains responsible only for the opaque SPI and
/// does not expose Framework domain DTOs.
/// </summary>
public sealed class RedisProviderSmokeTests
{
    [Fact]
    public async Task Dotnet_Opaque_Store_Round_Trip()
    {
        await using var store = CreateStore("dotnet");
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

    private static ZLinkRedisLocationStore CreateStore(string side)
    {
        var endpoint = Environment.GetEnvironmentVariable(
            "ZLINK_REDIS_TEST_ENDPOINT");
        var prefix = Environment.GetEnvironmentVariable(
            "ZLINK_REDIS_CROSS_LANGUAGE_PREFIX");
        if (string.IsNullOrWhiteSpace(endpoint)
            || string.IsNullOrWhiteSpace(prefix))
        {
            throw new InvalidOperationException(
                "Redis cross-language tests require ZLINK_REDIS_TEST_ENDPOINT "
                + "and ZLINK_REDIS_CROSS_LANGUAGE_PREFIX.");
        }

        return new ZLinkRedisLocationStore(new ZLinkRedisLocationOptions
        {
            ConnectionString = endpoint,
            KeyPrefix = $"{prefix}:{side}"
        });
    }
}
