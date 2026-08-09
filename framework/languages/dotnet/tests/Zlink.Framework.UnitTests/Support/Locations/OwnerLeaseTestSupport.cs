namespace Zlink.Framework.UnitTests;

internal static class OwnerLeaseTestSupport
{
    internal static async ValueTask<ZLinkLocationOwnerToken> ClaimLiveOwnerAsync(
        this IZLinkLocationRepository store,
        string ownerId,
        TimeSpan leaseTtl)
    {
        var claimed = Assert.IsType<ZLinkOwnerLeaseClaimResult.Claimed>(
            await store.ClaimOwnerLeaseAsync(ownerId, leaseTtl));
        var read = Assert.IsType<ZLinkOwnerLeaseReadResult.Found>(
            await store.ReadOwnerLeaseAsync(ownerId));
        Assert.Equal(claimed.Token, read.Token);
        return read.Token;
    }

    internal static async ValueTask<ZLinkLocationOwnerToken> RenewLiveOwnerAsync(
        this IZLinkLocationRepository store,
        string ownerId,
        TimeSpan leaseTtl)
    {
        var before = Assert.IsType<ZLinkOwnerLeaseReadResult.Found>(
            await store.ReadOwnerLeaseAsync(ownerId));
        Assert.IsType<ZLinkOwnerLeaseRenewResult.Renewed>(
            await store.RenewOwnerLeaseAsync(before.Token, leaseTtl));
        var after = Assert.IsType<ZLinkOwnerLeaseReadResult.Found>(
            await store.ReadOwnerLeaseAsync(ownerId));
        Assert.Equal(before.Token, after.Token);
        return after.Token;
    }
}
