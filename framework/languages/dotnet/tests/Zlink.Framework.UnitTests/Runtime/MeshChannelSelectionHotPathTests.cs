using System.Reflection;
using Zlink.Framework.Runtime.Service;
using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.UnitTests;

public sealed class MeshChannelSelectionHotPathTests
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task DirectSpotAsyncSubmissionAwaitsBusyActualPeerOwnerBeforeValidation(bool request)
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, "busy-direct-peer");
        using var release = new ManualResetEventSlim();
        var entered = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
        var holding = Task.Run(() => Field<ZLinkStateLane>(node, "_lane").RunAsync(() =>
        {
            entered.TrySetResult(true);
            release.Wait();
        }));
        try
        {
            await entered.Task.WaitAsync(TimeSpan.FromSeconds(5));
            var target = RoutingId.From("unknown-direct-target");
            Task pending;
            if (request)
            {
                var invocation = Task.Run(() => node.RequestToSpotDirectAsync("source", target,
                    "target", 1, [], SendFlags.None, default, TimeSpan.FromSeconds(1), default));
                var result = await invocation.WaitAsync(TimeSpan.FromSeconds(5));
                Assert.False(result.IsCompleted);
                pending = result.AsTask();
            }
            else
            {
                var invocation = Task.Run(() => node.SendToSpotDirectAsync("source", target,
                    "target", 1, [], SendFlags.None, default, default));
                var result = await invocation.WaitAsync(TimeSpan.FromSeconds(5));
                Assert.False(result.IsCompleted);
                pending = result.AsTask();
            }
            Assert.Equal(0UL, Field<ulong>(node, "_nextOperation"));
            release.Set();
            var failure = await Assert.ThrowsAsync<ZlinkSubmitException>(() => pending);
            Assert.Equal(ZlinkSubmitException.ErrorCode.NotConnected, failure.Result);
            Assert.Equal(0UL, Field<ulong>(node, "_nextOperation"));
        }
        finally
        {
            release.Set();
            await (await holding.WaitAsync(TimeSpan.FromSeconds(5)));
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task DirectRequestAwaitsBusyActualNonceOwnerAndPreservesInputParts(bool spot)
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, "busy-direct-nonce");
        var peer = AddReadyPeer(node);
        peer.LifecycleGeneration = 1;
        node.ObserveSpotAuthority(peer.RoutingId, "target", 1, peer.LifecycleGeneration, 1, 1);
        using var payload = Message.From("owned-input");
        using var release = new ManualResetEventSlim();
        var entered = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
        var holding = Task.Run(() => Field<ZLinkStateLane>(node, "_operationLane").RunAsync(() =>
        {
            entered.TrySetResult(true);
            release.Wait();
        }));
        try
        {
            await entered.Task.WaitAsync(TimeSpan.FromSeconds(5));
            var invocation = Task.Run(() => spot
                ? node.RequestToSpotDirectAsync("source", peer.RoutingId, "target", 1,
                    [payload], SendFlags.None, default, TimeSpan.FromSeconds(1), default)
                : node.RequestToNodeDirectAsync(peer.RoutingId, [payload], SendFlags.None,
                    default, TimeSpan.FromSeconds(1), default));
            var pending = await invocation.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.False(pending.IsCompleted);
            Assert.Equal(0UL, Field<ulong>(node, "_nextOperation"));
            release.Set();
            // No socket is started. Reaching this existing transport rejection
            // proves nonce admission finished before wire submission.
            await Assert.ThrowsAsync<ObjectDisposedException>(() => pending.AsTask());
            Assert.Equal(1UL, Field<ulong>(node, "_nextOperation"));
            Assert.Equal(2UL, node.AllocateOperationId().Low);
            Assert.Equal("owned-input", System.Text.Encoding.UTF8.GetString(payload.AsReadOnlySpan()));
        }
        finally
        {
            release.Set();
            await (await holding.WaitAsync(TimeSpan.FromSeconds(5)));
        }
    }

    [Fact]
    public async Task DirectSpotGenerationRejectionDoesNotAllocateNonceOrConsumeParts()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, "direct-generation");
        var peer = AddReadyPeer(node);
        peer.LifecycleGeneration = 1;
        node.ObserveSpotAuthority(peer.RoutingId, "target", 1, peer.LifecycleGeneration + 1, 1, 1);
        using var payload = Message.From("owned-input");
        var failure = await Assert.ThrowsAsync<ZlinkSubmitException>(() =>
            node.RequestToSpotDirectAsync("source", peer.RoutingId, "target", 1,
                [payload], SendFlags.None, default, TimeSpan.FromSeconds(1), default).AsTask());
        Assert.Equal(ZlinkSubmitException.ErrorCode.NotFound, failure.Result);
        Assert.Equal(0UL, Field<ulong>(node, "_nextOperation"));
        Assert.Equal("owned-input", System.Text.Encoding.UTF8.GetString(payload.AsReadOnlySpan()));
    }

    private delegate (bool Selected, RoutingId TargetRid, RoutingId PhysicalRid,
        SubmitResult Failure, string FailureReason, bool Wait, Task Changed)
        SelectTarget(string channel);

    private delegate ValueTask<(RoutingId TargetRid, RoutingId PhysicalRid)> WaitForTarget(
        string channel, TimeSpan timeout, CancellationToken cancellationToken);

    [Fact]
    public async Task ReadyRequestSelectionAllocatesNoDeadlineBeyondSynchronousSelection()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, "selection");
        var peer = AddReadyPeer(node);
        var select = Method<SelectTarget>(node, "TrySelectChannelTarget");
        var wait = Method<WaitForTarget>(node, "WaitForChannelTargetAsync");
        using var cancellation = new CancellationTokenSource();

        // Warm both existing selector paths, including JIT and AsyncLocal state.
        for (var index = 0; index < 256; index++)
        {
            select("worker");
            wait("worker", TimeSpan.FromMinutes(1), cancellation.Token).GetAwaiter().GetResult();
        }

        const int iterations = 1024;
        var start = GC.GetAllocatedBytesForCurrentThread();
        for (var index = 0; index < iterations; index++)
            select("worker");
        var synchronousBytes = GC.GetAllocatedBytesForCurrentThread() - start;
        start = GC.GetAllocatedBytesForCurrentThread();
        for (var index = 0; index < iterations; index++)
            wait("worker", TimeSpan.FromMinutes(1), cancellation.Token).GetAwaiter().GetResult();
        var requestBytes = GC.GetAllocatedBytesForCurrentThread() - start;

        // A linked CTS plus its deadline timer cannot fit in this allowance.
        // The reference path performs the same selector and peer lookup.
        Assert.True(requestBytes <= synchronousBytes + iterations * 32L,
            $"Ready request selection allocated {requestBytes} bytes; "
            + $"synchronous selection allocated {synchronousBytes} bytes.");

        cancellation.Cancel();
        var selected = await wait("worker", TimeSpan.FromMinutes(1), cancellation.Token);
        Assert.Equal(peer.RoutingId, selected.TargetRid);
        Assert.Equal(peer.PhysicalRoutingId, selected.PhysicalRid);
    }

    [Fact]
    public async Task SelectionTransfersTheChosenPhysicalIdentityWithoutRetainingMutablePeerState()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, "selection");
        var peer = AddReadyPeer(node);
        var originalPhysical = peer.PhysicalRoutingId;
        var wait = Method<WaitForTarget>(node, "WaitForChannelTargetAsync");
        var selected = await wait("worker", TimeSpan.FromSeconds(1), CancellationToken.None);

        peer.PhysicalRoutingId = RoutingId.From("replacement-physical");

        Assert.Equal(peer.RoutingId, selected.TargetRid);
        Assert.Equal(originalPhysical, selected.PhysicalRid);
        var next = await wait("worker", TimeSpan.FromSeconds(1), CancellationToken.None);
        Assert.Equal(peer.PhysicalRoutingId, next.PhysicalRid);
    }

    [Fact]
    public async Task UnresolvedFirstAdmissionPreservesCancellationAndDeadlineErrors()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, "selection");
        node.ConnectPeer("inproc://selection-not-started", RoutingId.From("pending-peer"));
        var wait = Method<WaitForTarget>(node, "WaitForChannelTargetAsync");
        using var cancellation = new CancellationTokenSource();
        cancellation.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
            wait("worker", TimeSpan.FromSeconds(1), cancellation.Token).AsTask());

        var timeout = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
            wait("worker", TimeSpan.FromTicks(1), CancellationToken.None).AsTask()
                .WaitAsync(TimeSpan.FromSeconds(3)));
        Assert.Equal(ZLinkFrameworkErrorKind.DeadlineExceeded, timeout.Kind);
    }

    private static ZLinkMeshPeer AddReadyPeer(ZLinkManagedMeshNode node)
    {
        var peer = new ZLinkMeshPeer(1, "inproc://selection-peer", null, "",
            ZLinkServiceConnectionDirection.Outbound)
        {
            RoutingId = RoutingId.From("logical-peer"),
            PhysicalRoutingId = RoutingId.From("physical-peer"),
            Admitted = true
        };
        Field<Dictionary<RoutingId, ZLinkMeshPeer>>(node, "_peersByRid")
            .Add(peer.RoutingId, peer);
        Field<ZLinkMeshChannelSelection>(node, "_channelSelection").Rebuild(
            ["worker"], _ => [new ZLinkMeshChannelTarget(peer.RoutingId, 1)]);
        return peer;
    }

    private static T Method<T>(ZLinkManagedMeshNode node, string name)
        where T : Delegate =>
        typeof(ZLinkManagedMeshNode).GetMethod(name, BindingFlags.Instance | BindingFlags.NonPublic)!
            .CreateDelegate<T>(node);

    private static T Field<T>(ZLinkManagedMeshNode node, string name) =>
        (T)typeof(ZLinkManagedMeshNode).GetField(name, BindingFlags.Instance | BindingFlags.NonPublic)!
            .GetValue(node)!;
}
