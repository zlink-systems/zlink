using Xunit.Abstractions;
using Zlink.Framework.Tests;

namespace Zlink.Framework.Locations.Redis.Tests;

[Collection(RedisTestCollection.Name)]
public sealed class RedisLocationCreationContentionTests(
    RedisTestFixture fixture,
    ITestOutputHelper output
)
{
    [SkippableTheory]
    [InlineData(0)]
    [InlineData(200)]
    public async Task DifferentActorIdsCreateWithTwoWritesAndNoConflicts(int actorLimit)
    {
        Skip.IfNot(fixture.RedisAvailable, fixture.SkipReason);
        await using var store = fixture.CreateStore();
        await LocationCreationContentionContract.VerifyAsync(store, actorLimit, output);
    }
}
