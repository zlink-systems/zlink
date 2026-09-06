using Zlink.Framework.Runtime.Backend.Contracts;

namespace Zlink.Framework.UnitTests;

public sealed partial class StatefulServiceRuntimeTests
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task OutboundInboundOutboundHandover_RemovesEndpointRegistrationAndCompletesRemoteJoin(bool removeBeforeInbound)
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        var ownerContext = new CapturingMeshSocketContext(context);
        await using var owner = NewNode(ownerContext, "mesh-mid");
        var ownerEndpoint = $"tcp://127.0.0.1:{FindFreeTcpPort()}";
        var endpoint = $"tcp://127.0.0.1:{FindFreeTcpPort()}";
        owner.SetBind(ownerEndpoint);
        var spot = (ZLinkManagedSpot)owner.GetOrCreateSpot("zone-nw", out _);
        owner.Start();
        ulong oldIntent;
        await using (var old = NewNode(context, "mesh-z-old"))
        {
            old.SetBind(endpoint);
            old.Start();
            oldIntent = owner.ConnectPeer(endpoint, old.RoutingId);
            await WaitUntilAsync(() => owner.Status().AdmittedPeerCount == 1
                && old.Status().AdmittedPeerCount == 1);
        }
        await WaitUntilAsync(() => owner.Status().AdmittedPeerCount == 0
            && owner.Peers().Any(peer => peer.ConnectionIntentId == oldIntent
                && peer.State == MeshPeerState.Connecting));
        if (removeBeforeInbound)
            Assert.True(owner.RemovePeerConnectionIfNotAdmitted(oldIntent));
        await using (var middle = NewNode(context, "mesh-a-middle"))
        {
            middle.SetBind(endpoint);
            middle.ConnectPeer(ownerEndpoint, owner.RoutingId);
            middle.Start();
            await WaitUntilAsync(() => owner.Peers().Any(peer =>
                peer.RoutingId == middle.RoutingId && peer.State == MeshPeerState.Admitted)
                && middle.Status().AdmittedPeerCount == 1);
            var inbound = Assert.Single(owner.Peers(), peer => peer.RoutingId == middle.RoutingId);
            if (!removeBeforeInbound)
                Assert.True(owner.RemovePeerConnectionIfNotAdmitted(oldIntent));
            Assert.DoesNotContain(owner.Peers(), peer => peer.ConnectionIntentId == oldIntent);
            // The public disconnect reports NotFound only when the native
            // endpoint registration is gone; no binding internals are inspected.
            var absent = Assert.Throws<ZlinkConnectException>(
                () => ownerContext.Router!.Disconnect(endpoint));
            Assert.Equal(ZlinkConnectException.ErrorCode.NotFound, absent.Result);
            var retained = Assert.Single(owner.Peers(), peer => peer.RoutingId == middle.RoutingId);
            Assert.Equal(MeshPeerState.Admitted, retained.State);
            Assert.Equal(inbound.ConnectionIntentId, retained.ConnectionIntentId);
            Assert.Equal(inbound.LifecycleGeneration, retained.LifecycleGeneration);
            Assert.Equal(inbound.DescriptorRevision, retained.DescriptorRevision);
            Assert.Equal(inbound.LastChangedMs, retained.LastChangedMs);
            middle.PublishDraining();
            await WaitUntilAsync(() => owner.Peers().Any(peer =>
                peer.RoutingId == middle.RoutingId && peer.State == MeshPeerState.Draining));
        }
        await WaitUntilAsync(() => owner.Status().AdmittedPeerCount == 0);
        await using var replacement = NewNode(context, "mesh-z-replacement");
        replacement.SetBind(endpoint);
        replacement.Start();
        var replacementIntent = owner.ConnectPeer(endpoint, replacement.RoutingId);
        replacement.ObserveSpotAuthority(owner.RoutingId, spot.SpotId,
            spot.LifecycleGeneration, owner.Status().LifecycleGeneration,
            spot.AuthorityOwnerGeneration, 7);
        var operation = replacement.AllocateOperationId();
        Assert.Equal(SubmitResult.Ok, replacement.TryRequestCanonicalActorJoin(
            new ZLinkBackendCanonicalActorJoinRequest(
                new ZLinkBackendActorRef(replacement.RoutingId, "a1", 11),
                replacement.Status().LifecycleGeneration, 13, 7, false,
                owner.RoutingId, spot.SpotId, spot.LifecycleGeneration,
                owner.Status().LifecycleGeneration, spot.AuthorityOwnerGeneration,
                7, "ZLinkFrameworkActorJoinRequest", "application/json", "{}"u8.ToArray()),
            operation, TimeSpan.FromSeconds(2)));
        var joins = 0;
        int? completion = null;
        await WaitUntilAsync(() =>
        {
            foreach (var record in DrainRecords(owner))
            {
                if (record.OperationKind != MeshOperationKind.ActorJoin) continue;
                joins++;
                Assert.Equal(SubmitResult.Ok,
                    record.ReplyJoin(ActorJoinResult.Accepted, Array.Empty<Message>()));
            }
            foreach (var record in DrainRecords(replacement))
            {
                if (record.OperationId != operation) continue;
                completion = record.TerminalResult;
            }
            return completion is not null;
        });
        Assert.Equal((int)RequestResult.Ok, completion);
        Assert.Equal(1, joins);
        var outbound = Assert.Single(owner.Peers(), peer => peer.RoutingId == replacement.RoutingId);
        Assert.Equal(replacementIntent, outbound.ConnectionIntentId);
        Assert.Equal(MeshPeerState.Admitted, outbound.State);
        Assert.Equal(MeshPeerState.Admitted, Assert.Single(replacement.Peers()).State);
    }

    [Fact]
    public async Task RemovingUnadmittedIntent_PreservesAnotherOutboundReplacement()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var owner = NewNode(context, "mesh-mid");
        await using var replacement = NewNode(context, "mesh-z-replacement");
        var endpoint = $"tcp://127.0.0.1:{FindFreeTcpPort()}";
        owner.SetBind($"tcp://127.0.0.1:{FindFreeTcpPort()}");
        owner.Start();
        var staleIntent = owner.ConnectPeer(endpoint, replacement.RoutingId);
        var replacementIntent = owner.ConnectPeer(endpoint, replacement.RoutingId);

        Assert.True(owner.RemovePeerConnectionIfNotAdmitted(staleIntent));
        Assert.Equal(replacementIntent, Assert.Single(owner.Peers()).ConnectionIntentId);
        replacement.SetBind(endpoint);
        replacement.Start();
        await WaitUntilAsync(() => owner.Status().AdmittedPeerCount == 1
            && replacement.Status().AdmittedPeerCount == 1);
        Assert.Equal(replacementIntent, Assert.Single(owner.Peers()).ConnectionIntentId);
        Assert.False(owner.RemovePeerConnectionIfNotAdmitted(replacementIntent));
    }

    private sealed class CapturingMeshSocketContext(IContext inner) : IContext
    {
        public IRouterSocket? Router { get; private set; }
        public IContextOptions Options => inner.Options;
        public IRouterSocket CreateRouterSocket() => Router = inner.CreateRouterSocket();
        public IPairSocket CreatePairSocket() => inner.CreatePairSocket();
        public IDealerSocket CreateDealerSocket() => inner.CreateDealerSocket();
        public IPubSocket CreatePubSocket() => inner.CreatePubSocket();
        public ISubSocket CreateSubSocket() => inner.CreateSubSocket();
        public IXPubSocket CreateXPubSocket() => inner.CreateXPubSocket();
        public IXSubSocket CreateXSubSocket() => inner.CreateXSubSocket();
        public IStreamSocket CreateStreamSocket() => inner.CreateStreamSocket();
        public void Shutdown() => inner.Shutdown();
        public void RecalculateAutoHwm() => inner.RecalculateAutoHwm();
        public CoreHwmBudgetSnapshot GetCoreHwmBudgetSnapshot() => inner.GetCoreHwmBudgetSnapshot();
        public void ResetCoreHwmBudgetMetrics() => inner.ResetCoreHwmBudgetMetrics();
        public void Dispose() => inner.Dispose();
        public ValueTask DisposeAsync() => inner.DisposeAsync();
    }
}
