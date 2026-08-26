using Zlink.Framework.Runtime.Actors;
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

        Assert.True(admission.TryAcquire(Binding("actor-a", "binding-a")));
        Assert.True(admission.TryAcquire(Binding("actor-a", "binding-a")));
        Assert.False(admission.TryAcquire(Binding("actor-a", "binding-a")));
        Assert.True(admission.TryAcquire(Binding("actor-b", "binding-b")));
        Assert.False(admission.TryAcquire(Binding("actor-c", "binding-c")));

        admission.Release(Binding("actor-a", "binding-a"));
        Assert.True(admission.TryAcquire(Binding("actor-c", "binding-c")));
        admission.Clear();
        Assert.True(admission.TryAcquire(Binding("actor-a", "binding-a")));

        static ZLinkSessionBindingKey Binding(string actorId, string bindingToken) =>
            ZLinkSessionBindingKey.FromBoundary(actorId, bindingToken);
    }

    [Fact]
    public async Task ConcurrentOperationsForTheSameActorRemainSeparate()
    {
        using var assembler = new ZLinkRemoteRelayFrameAssembler(
            TimeSpan.FromSeconds(1),
            static () => CancellationToken.None);
        var first = Key(operationLow: 1);
        var second = Key(operationLow: 2);

        var incompleteFirst = await AppendAsync(assembler, first, [1], true);
        var incompleteSecond = await AppendAsync(assembler, second, [2], true);
        Assert.Null(incompleteFirst);
        Assert.Null(incompleteSecond);

        var completedFirst = await AppendAsync(assembler, first, [3], false);
        var completedSecond = await AppendAsync(assembler, second, [4], false);
        Assert.Equal(new byte[][] { [1], [3] }, completedFirst!.Parts);
        Assert.Equal(new byte[][] { [2], [4] }, completedSecond!.Parts);
        await assembler.CommitAsync(completedFirst);
        await assembler.CommitAsync(completedSecond);
    }

    [Fact]
    public async Task RejectedTerminalPartKeepsOnlyTheAcceptedPrefixForRetry()
    {
        using var assembler = new ZLinkRemoteRelayFrameAssembler(
            TimeSpan.FromSeconds(1),
            static () => CancellationToken.None);
        var key = Key(operationLow: 3);

        _ = await AppendAsync(assembler, key, [1], true);
        var rejected = await AppendAsync(assembler, key, [2], false);
        await assembler.RejectAsync(rejected!);
        var retried = await AppendAsync(assembler, key, [3], false);

        Assert.Equal(new byte[][] { [1], [3] }, retried!.Parts);
        await assembler.CommitAsync(retried);
    }

    [Fact]
    public async Task FullFrameRetryReplacesTheRejectedAttemptPrefix()
    {
        using var assembler = new ZLinkRemoteRelayFrameAssembler(
            TimeSpan.FromSeconds(1),
            static () => CancellationToken.None);
        var key = Key(operationLow: 7);

        _ = await AppendAsync(assembler, key, [1], true);
        var rejected = await AppendAsync(assembler, key, [2], false);
        await assembler.RejectAsync(rejected!);

        var restarted = await AppendAsync(assembler, key, [3], true);
        Assert.Null(restarted);
        var retried = await AppendAsync(assembler, key, [4], false);

        Assert.Equal(new byte[][] { [3], [4] }, retried!.Parts);
        await assembler.CommitAsync(retried);
    }

    [Fact]
    public async Task SourceRestartWithReusedOperationIdCannotReuseRetainedPrefix()
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

        _ = await AppendAsync(assembler, previousLifecycle, [1], true);
        var rejected = await AppendAsync(assembler, previousLifecycle, [2], false);
        await assembler.RejectAsync(rejected!);

        _ = await AppendAsync(assembler, restartedLifecycle, [3], true);
        var restarted = await AppendAsync(assembler, restartedLifecycle, [4], false);
        Assert.Equal(new byte[][] { [3], [4] }, restarted!.Parts);
        await assembler.CommitAsync(restarted);

        var previousRetry = await AppendAsync(assembler, previousLifecycle, [5], false);
        Assert.Equal(new byte[][] { [1], [5] }, previousRetry!.Parts);
        await assembler.CommitAsync(previousRetry);
    }

    [Fact]
    public async Task ExpiredOrShutdownAssemblyCannotReuseItsPrefix()
    {
        using var shutdown = new CancellationTokenSource();
        using var assembler = new ZLinkRemoteRelayFrameAssembler(
            TimeSpan.FromMilliseconds(20),
            () => shutdown.Token);
        var expired = Key(operationLow: 4);
        _ = await AppendAsync(assembler, expired, [1], true);
        await Task.Delay(100);

        var afterExpiry = await AppendAsync(assembler, expired, [2], false);
        Assert.Single(afterExpiry!.Parts);

        var stopped = Key(operationLow: 5);
        _ = await AppendAsync(assembler, stopped, [1], true);
        shutdown.Cancel();
        await Task.Delay(20);
        var stoppedAppend = await assembler.TryAppendAsync(stopped, [2], false);
        Assert.False(stoppedAppend.Accepted);
    }

    [Fact]
    public async Task FrameAndProcessByteBoundsReturnBackpressure()
    {
        using var assembler = new ZLinkRemoteRelayFrameAssembler(
            TimeSpan.FromSeconds(1),
            static () => CancellationToken.None);
        var oversized = new byte[(16 * 1024 * 1024) + 1];

        var oversizedAppend = await assembler.TryAppendAsync(
            Key(operationLow: 6),
            oversized,
            true);
        Assert.False(oversizedAppend.Accepted);
    }

    private static async ValueTask<ZLinkRemoteRelayFrameAssembler.CompletedFrame?> AppendAsync(
        ZLinkRemoteRelayFrameAssembler assembler,
        ZLinkRemoteRelayFrameKey key,
        byte[] part,
        bool hasMore)
    {
        var append = await assembler.TryAppendAsync(key, part, hasMore);
        Assert.True(append.Accepted);
        return append.Completed;
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
