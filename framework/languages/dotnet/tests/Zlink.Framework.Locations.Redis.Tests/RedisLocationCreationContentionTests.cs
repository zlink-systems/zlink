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
    [InlineData("unlimited")]
    [InlineData("at-limit")]
    public async Task DifferentActorIdsCreateWithTwoWritesAndNoConflicts(string caseId)
    {
        Skip.IfNot(fixture.RedisAvailable, fixture.SkipReason);
        await using var store = fixture.CreateStore();
        await LocationCreationContentionContract.VerifyAsync(store, caseId, output);
    }
}
