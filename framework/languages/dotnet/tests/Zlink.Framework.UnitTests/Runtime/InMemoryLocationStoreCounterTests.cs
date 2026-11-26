using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Tests;

namespace Zlink.Framework.UnitTests;

public sealed class InMemoryLocationStoreCounterTests
{
    [Fact]
    public Task CounterOperationsAreAtomic() =>
        LocationStoreCounterContract.VerifyAsync(new ZLinkInMemoryProviderLocationStore());

    [Fact]
    public Task ConcurrentBatchesUpdateTheSameCounterWithoutConflicts() =>
        LocationStoreCounterContract.VerifyConcurrentBatchesAsync(
            new ZLinkInMemoryProviderLocationStore()
        );
}
