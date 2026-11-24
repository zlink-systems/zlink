using Xunit;
using Xunit.Abstractions;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Tests;

namespace Zlink.Framework.UnitTests;

public sealed class InMemoryLocationCreationContentionTests(ITestOutputHelper output)
{
    [Theory]
    [InlineData("unlimited")]
    [InlineData("at-limit")]
    public Task DifferentActorIdsCreateWithTwoWritesAndNoConflicts(string caseId) =>
        LocationCreationContentionContract.VerifyAsync(
            new ZLinkInMemoryProviderLocationStore(),
            caseId,
            output
        );
}
