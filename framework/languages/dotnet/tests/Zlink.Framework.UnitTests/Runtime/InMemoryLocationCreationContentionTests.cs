using Xunit;
using Xunit.Abstractions;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Tests;

namespace Zlink.Framework.UnitTests;

public sealed class InMemoryLocationCreationContentionTests(ITestOutputHelper output)
{
    [Theory]
    [InlineData(0)]
    [InlineData(200)]
    public Task DifferentActorIdsCreateWithTwoWritesAndNoConflicts(int actorLimit) =>
        LocationCreationContentionContract.VerifyAsync(
            new ZLinkInMemoryProviderLocationStore(),
            actorLimit,
            output
        );
}
