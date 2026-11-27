using Zlink.Framework.Tests;

namespace Zlink.Framework.Locations.Redis.Tests;

[Collection(RedisTestCollection.Name)]
public sealed class RedisLocationStoreCounterTests(RedisTestFixture fixture)
{
    [SkippableFact]
    public async Task CounterOperationsAreAtomic()
    {
        Skip.IfNot(fixture.RedisAvailable, fixture.SkipReason);
        await using var store = fixture.CreateStore();
        await LocationStoreCounterContract.VerifyAsync(store);
    }

    [SkippableFact]
    public async Task ConcurrentBatchesUpdateTheSameCounterWithoutConflicts()
    {
        Skip.IfNot(fixture.RedisAvailable, fixture.SkipReason);
        await using var store = fixture.CreateStore();
        await LocationStoreCounterContract.VerifyConcurrentBatchesAsync(store);
    }
}
