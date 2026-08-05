using Zlink.Framework.Runtime.Dispatch;

namespace Zlink.Framework.UnitTests.Runtime.Dispatch;

public sealed class CompletionAdmissionOwnerTests
{
    [Fact]
    public async Task Request_limit_waits_without_blocking_existing_completion()
    {
        using var owner = new ZLinkCompletionAdmissionOwner(1, 2, 1024);
        using var first = await owner.AcquireRequesterAsync(64);
        var secondTask = owner.AcquireRequesterAsync(64).AsTask();

        Assert.False(secondTask.IsCompleted);
        first.Complete();

        using var second = await secondTask.WaitAsync(TimeSpan.FromSeconds(2));
        Assert.Equal(1, owner.Snapshot().PendingRequests);
    }

    [Fact]
    public async Task Responder_permit_is_held_until_core_accepts_reply()
    {
        using var owner = new ZLinkCompletionAdmissionOwner(2, 1, 1024);
        using var first = await owner.AcquireResponderAsync();
        var secondTask = owner.AcquireResponderAsync().AsTask();

        Assert.False(secondTask.IsCompleted);
        await first.ReserveReplyAsync(128);
        Assert.Equal(
            new ZLinkCompletionAdmissionSnapshot(0, 2, 1, 1, 0, 128, 1024),
            owner.Snapshot());

        // A backpressured Core submit does not call TransferToCore. The reply
        // bytes and permit therefore remain owned by Framework for retry.
        Assert.False(secondTask.IsCompleted);
        first.TransferToCore();

        using var second = await secondTask.WaitAsync(TimeSpan.FromSeconds(2));
        Assert.Equal(1, owner.Snapshot().PendingCompletionSends);
    }

    [Fact]
    public async Task Requester_and_responder_share_the_host_byte_bound()
    {
        using var owner = new ZLinkCompletionAdmissionOwner(2, 2, 100);
        using var requester = await owner.AcquireRequesterAsync(80);
        using var responder = await owner.AcquireResponderAsync();
        var reserveTask = responder.ReserveReplyAsync(30).AsTask();

        Assert.False(reserveTask.IsCompleted);
        requester.Complete();
        await reserveTask.WaitAsync(TimeSpan.FromSeconds(2));

        Assert.Equal(0UL, owner.Snapshot().RequesterReserveBytes);
        Assert.Equal(30UL, owner.Snapshot().ResponderReserveBytes);
        responder.TransferToCore();
        Assert.Equal(0UL, owner.Snapshot().ResponderReserveBytes);
    }

    [Fact]
    public async Task Cancelled_waiter_does_not_consume_count_or_bytes()
    {
        using var owner = new ZLinkCompletionAdmissionOwner(1, 1, 100);
        using var active = await owner.AcquireRequesterAsync(100);
        using var cancellation = new CancellationTokenSource();
        var waiting = owner.AcquireRequesterAsync(1, cancellation.Token).AsTask();

        cancellation.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => waiting);

        Assert.Equal(
            new ZLinkCompletionAdmissionSnapshot(1, 1, 0, 1, 100, 0, 100),
            owner.Snapshot());
    }

    [Fact]
    public async Task Cancelled_reply_reservation_can_be_retried()
    {
        using var owner = new ZLinkCompletionAdmissionOwner(1, 1, 100);
        using var requester = await owner.AcquireRequesterAsync(100);
        using var responder = await owner.AcquireResponderAsync();
        using var cancellation = new CancellationTokenSource();
        var waiting = responder.ReserveReplyAsync(1, cancellation.Token).AsTask();

        cancellation.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => waiting);
        requester.Complete();
        await responder.ReserveReplyAsync(1);
        responder.TransferToCore();
    }

    [Fact]
    public async Task Shutdown_terminates_waiters_and_clears_owned_reserves()
    {
        var owner = new ZLinkCompletionAdmissionOwner(1, 1, 100);
        using var active = await owner.AcquireRequesterAsync(100);
        var waiting = owner.AcquireRequesterAsync(1).AsTask();

        owner.Dispose();

        await Assert.ThrowsAsync<ObjectDisposedException>(() => waiting);
        Assert.Equal(
            new ZLinkCompletionAdmissionSnapshot(0, 1, 0, 1, 0, 0, 100),
            owner.Snapshot());
        active.Dispose(); // stale generation cleanup is idempotent
    }

    [Fact]
    public async Task Empty_reserve_allows_one_oversized_completion_to_progress()
    {
        using var owner = new ZLinkCompletionAdmissionOwner(1, 1, 64);
        using var responder = await owner.AcquireResponderAsync();

        await responder.ReserveReplyAsync(1024);

        Assert.Equal(1024UL, owner.Snapshot().ResponderReserveBytes);
        responder.TransferToCore();
    }
}
