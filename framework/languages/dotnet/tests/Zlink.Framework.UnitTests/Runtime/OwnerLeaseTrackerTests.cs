using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests;

public sealed class OwnerLeaseTrackerTests
{
    [Fact]
    public async Task Lease_Expiry_Uses_Store_Time_Plus_Monotonic_Elapsed_Never_The_Wall_Clock()
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryLocationStore(time);
        await store.ClaimLiveOwnerAsync("owner-a", TimeSpan.FromSeconds(15));

        // A long polling interval keeps the snapshot cached, so expiry is
        // judged from the snapshot's StoreNow plus local monotonic elapsed.
        var options = new ZLinkLocationOptions { PollingInterval = TimeSpan.FromMinutes(10) };
        var tracker = new ZLinkOwnerLeaseTracker(store, options, time);
        Assert.True(await tracker.IsOwnerLiveAsync("owner-a"));

        // An application wall-clock jump alone never expires an owner:
        // wall clocks are not part of the expiry computation (draft 6.6).
        time.AdvanceWallClockOnly(TimeSpan.FromHours(1));
        Assert.True(await tracker.IsOwnerLiveAsync("owner-a"));

        // Monotonic elapsed time past the TTL does.
        time.AdvanceMonotonicOnly(TimeSpan.FromSeconds(16));
        Assert.False(await tracker.IsOwnerLiveAsync("owner-a"));
    }

    [Fact]
    public async Task Snapshot_Staleness_Is_Bounded_By_The_Polling_Interval()
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryLocationStore(time);
        var owner = await store.ClaimLiveOwnerAsync(
            "owner-a",
            TimeSpan.FromMinutes(5));
        var options = new ZLinkLocationOptions { PollingInterval = TimeSpan.FromSeconds(1) };
        var tracker = new ZLinkOwnerLeaseTracker(store, options, time);
        Assert.True(await tracker.IsOwnerLiveAsync("owner-a"));

        // The owner disappears from the store; the cached snapshot may keep
        // it alive for at most one polling interval.
        Assert.Equal(
            ZLinkOwnerLeaseReleaseResult.Released,
            await store.ReleaseOwnerLeaseAsync(owner));
        Assert.True(await tracker.IsOwnerLiveAsync("owner-a"));

        time.Advance(TimeSpan.FromSeconds(1));
        Assert.False(await tracker.IsOwnerLiveAsync("owner-a"));
    }

    [Fact]
    public async Task Route_Admission_Stops_At_The_Lease_Fencing_Deadline()
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryLocationStore(time);
        var owner = await store.ClaimLiveOwnerAsync(
            "owner-a",
            TimeSpan.FromSeconds(15));
        var options = new ZLinkLocationOptions
        {
            PollingInterval = TimeSpan.FromMinutes(10),
            OwnerLeaseFencingMargin = TimeSpan.FromSeconds(5)
        };
        var tracker = new ZLinkOwnerLeaseTracker(store, options, time);

        Assert.Equal(
            TimeSpan.FromSeconds(10),
            await tracker.GetOwnerTokenRemainingAdmissionLifetimeAsync(owner));

        time.AdvanceMonotonicOnly(TimeSpan.FromSeconds(9));
        Assert.Equal(
            TimeSpan.FromSeconds(1),
            await tracker.GetOwnerTokenRemainingAdmissionLifetimeAsync(owner));

        // The provider lease is still present for five more seconds, but new
        // routes must not use that interval for admission.
        time.AdvanceMonotonicOnly(TimeSpan.FromSeconds(1));
        Assert.Null(
            await tracker.GetOwnerTokenRemainingAdmissionLifetimeAsync(owner));
        Assert.True(await tracker.IsOwnerTokenLiveAsync(owner));
    }
}
