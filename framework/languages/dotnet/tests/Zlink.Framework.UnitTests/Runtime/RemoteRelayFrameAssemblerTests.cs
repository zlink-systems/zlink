using Zlink.Framework.Runtime.Host;

namespace Zlink.Framework.UnitTests;

public sealed class RemoteRelayFrameAssemblerTests
{
    [Fact]
    public void RemoteRequestAdmissionBoundsProcessAndBindingIndependently()
    {
        var admission = new ZLinkBoundedRemoteRequestAdmission(
            maxTotal: 3,
            maxPerBinding: 2);

        Assert.True(admission.TryAcquire("actor-a", "binding-a"));
        Assert.True(admission.TryAcquire("actor-a", "binding-a"));
        Assert.False(admission.TryAcquire("actor-a", "binding-a"));
        Assert.True(admission.TryAcquire("actor-b", "binding-b"));
        Assert.False(admission.TryAcquire("actor-c", "binding-c"));

        admission.Release("actor-a", "binding-a");
        Assert.True(admission.TryAcquire("actor-c", "binding-c"));
        admission.Clear();
        Assert.True(admission.TryAcquire("actor-a", "binding-a"));
    }

    [Fact]
    public void ConcurrentOperationsForTheSameActorRemainSeparate()
    {
        using var assembler = new ZLinkRemoteRelayFrameAssembler(
            TimeSpan.FromSeconds(1),
            static () => CancellationToken.None);
        var first = Key(operationLow: 1);
        var second = Key(operationLow: 2);

        Assert.True(assembler.TryAppend(first, [1], true, out var incompleteFirst));
        Assert.True(assembler.TryAppend(second, [2], true, out var incompleteSecond));
        Assert.Null(incompleteFirst);
        Assert.Null(incompleteSecond);

        Assert.True(assembler.TryAppend(first, [3], false, out var completedFirst));
        Assert.True(assembler.TryAppend(second, [4], false, out var completedSecond));
        Assert.Equal(new byte[][] { [1], [3] }, completedFirst!.Parts);
        Assert.Equal(new byte[][] { [2], [4] }, completedSecond!.Parts);
        assembler.Commit(completedFirst);
        assembler.Commit(completedSecond);
    }

    [Fact]
    public void RejectedTerminalPartKeepsOnlyTheAcceptedPrefixForRetry()
    {
        using var assembler = new ZLinkRemoteRelayFrameAssembler(
            TimeSpan.FromSeconds(1),
            static () => CancellationToken.None);
        var key = Key(operationLow: 3);

        Assert.True(assembler.TryAppend(key, [1], true, out _));
        Assert.True(assembler.TryAppend(key, [2], false, out var rejected));
        assembler.Reject(rejected!);
        Assert.True(assembler.TryAppend(key, [3], false, out var retried));

        Assert.Equal(new byte[][] { [1], [3] }, retried!.Parts);
        assembler.Commit(retried);
    }

    [Fact]
    public void FullFrameRetryReplacesTheRejectedAttemptPrefix()
    {
        using var assembler = new ZLinkRemoteRelayFrameAssembler(
            TimeSpan.FromSeconds(1),
            static () => CancellationToken.None);
        var key = Key(operationLow: 7);

        Assert.True(assembler.TryAppend(key, [1], true, out _));
        Assert.True(assembler.TryAppend(key, [2], false, out var rejected));
        assembler.Reject(rejected!);

        Assert.True(assembler.TryAppend(key, [3], true, out var restarted));
        Assert.Null(restarted);
        Assert.True(assembler.TryAppend(key, [4], false, out var retried));

        Assert.Equal(new byte[][] { [3], [4] }, retried!.Parts);
        assembler.Commit(retried);
    }

    [Fact]
    public void SourceRestartWithReusedOperationIdCannotReuseRetainedPrefix()
    {
        using var assembler = new ZLinkRemoteRelayFrameAssembler(
            TimeSpan.FromSeconds(1),
            static () => CancellationToken.None);
        var previousLifecycle = Key(
            operationLow: 8,
            sourceNodeGeneration: 11,
            requestSourceOwnerId: "source-owner-1",
            requestSourceLeaseGeneration: 12,
            requestSourceNodeGeneration: 11);
        var restartedLifecycle = Key(
            operationLow: 8,
            sourceNodeGeneration: 21,
            requestSourceOwnerId: "source-owner-2",
            requestSourceLeaseGeneration: 22,
            requestSourceNodeGeneration: 21);

        Assert.True(assembler.TryAppend(previousLifecycle, [1], true, out _));
        Assert.True(assembler.TryAppend(previousLifecycle, [2], false, out var rejected));
        assembler.Reject(rejected!);

        Assert.True(assembler.TryAppend(restartedLifecycle, [3], true, out _));
        Assert.True(assembler.TryAppend(restartedLifecycle, [4], false, out var restarted));
        Assert.Equal(new byte[][] { [3], [4] }, restarted!.Parts);
        assembler.Commit(restarted);

        Assert.True(assembler.TryAppend(previousLifecycle, [5], false, out var previousRetry));
        Assert.Equal(new byte[][] { [1], [5] }, previousRetry!.Parts);
        assembler.Commit(previousRetry);
    }

    [Fact]
    public async Task ExpiredOrShutdownAssemblyCannotReuseItsPrefix()
    {
        using var shutdown = new CancellationTokenSource();
        using var assembler = new ZLinkRemoteRelayFrameAssembler(
            TimeSpan.FromMilliseconds(20),
            () => shutdown.Token);
        var expired = Key(operationLow: 4);
        Assert.True(assembler.TryAppend(expired, [1], true, out _));
        await Task.Delay(100);

        Assert.True(assembler.TryAppend(expired, [2], false, out var afterExpiry));
        Assert.Single(afterExpiry!.Parts);

        var stopped = Key(operationLow: 5);
        Assert.True(assembler.TryAppend(stopped, [1], true, out _));
        shutdown.Cancel();
        await Task.Delay(20);
        Assert.False(assembler.TryAppend(stopped, [2], false, out _));
    }

    [Fact]
    public void FrameAndProcessByteBoundsReturnBackpressure()
    {
        using var assembler = new ZLinkRemoteRelayFrameAssembler(
            TimeSpan.FromSeconds(1),
            static () => CancellationToken.None);
        var oversized = new byte[(16 * 1024 * 1024) + 1];

        Assert.False(assembler.TryAppend(
            Key(operationLow: 6),
            oversized,
            true,
            out _));
    }

    private static ZLinkRemoteRelayFrameKey Key(
        ulong operationLow,
        ulong sourceNodeGeneration = 11,
        string requestSourceOwnerId = "source-owner",
        ulong requestSourceLeaseGeneration = 12,
        ulong requestSourceNodeGeneration = 11) => new(
        RouteKind: 1,
        ActorId: "actor",
        ActorGeneration: 3,
        BindingIdentity: "binding",
        SourceNodeRid: "source-node",
        SourceNodeGeneration: sourceNodeGeneration,
        SourceSessionRid: "source-session",
        RequestSourceOwnerId: requestSourceOwnerId,
        RequestSourceLeaseGeneration: requestSourceLeaseGeneration,
        RequestSourceNodeRid: "source-node",
        RequestSourceNodeGeneration: requestSourceNodeGeneration,
        OperationHigh: 7,
        OperationLow: operationLow,
        ReplyRequestId: operationLow,
        TargetNodeGeneration: 8,
        AuthorityOwnerGeneration: 9,
        OwnerLeaseGeneration: 10);
}
