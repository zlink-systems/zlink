using System.Reflection;
using System.Runtime.ExceptionServices;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Runtime.Backend.Contracts;

namespace Zlink.Framework.UnitTests;

public sealed partial class StatefulServiceRuntimeTests
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task OutboundInboundOutboundHandover_RemovesEndpointRegistrationAndCompletesRemoteJoin(
        bool removeBeforeInbound
    )
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        var ownerContext = new CapturingMeshSocketContext(context);
        await using var owner = NewNode(ownerContext, "mesh-mid");
        owner.SetBind("tcp://127.0.0.1:0");
        var spot = (ZLinkManagedSpot)owner.GetOrCreateSpot("zone-nw", out _);
        owner.Start();
        var ownerEndpoint = Assert.IsType<string>(owner.Status().LocalEndpoint);
        ulong oldIntent;
        string endpoint;
        await using (var old = NewNode(context, "mesh-z-old"))
        {
            old.SetBind("tcp://127.0.0.1:0");
            old.Start();
            endpoint = Assert.IsType<string>(old.Status().LocalEndpoint);
            oldIntent = owner.ConnectPeer(endpoint, old.RoutingId);
            await WaitUntilAsync(() =>
                owner.Status().AdmittedPeerCount == 1 && old.Status().AdmittedPeerCount == 1
            );
        }
        await WaitUntilAsync(() =>
            owner.Status().AdmittedPeerCount == 0
            && owner
                .Peers()
                .Any(peer =>
                    peer.ConnectionIntentId == oldIntent && peer.State == MeshPeerState.Connecting
                )
        );
        if (removeBeforeInbound)
            Assert.True(owner.RemovePeerConnectionIfNotAdmitted(oldIntent));
        await using (var middle = NewNode(context, "mesh-a-middle"))
        {
            middle.SetBind(endpoint);
            middle.ConnectPeer(ownerEndpoint, owner.RoutingId);
            middle.Start();
            await WaitUntilAsync(() =>
                owner
                    .Peers()
                    .Any(peer =>
                        peer.RoutingId == middle.RoutingId && peer.State == MeshPeerState.Admitted
                    )
                && middle.Status().AdmittedPeerCount == 1
            );
            var inbound = Assert.Single(owner.Peers(), peer => peer.RoutingId == middle.RoutingId);
            if (!removeBeforeInbound)
                Assert.True(owner.RemovePeerConnectionIfNotAdmitted(oldIntent));
            Assert.DoesNotContain(owner.Peers(), peer => peer.ConnectionIntentId == oldIntent);
            // The public disconnect reports NotFound only when the native
            // endpoint registration is gone; no binding internals are inspected.
            var absent = Assert.Throws<ZlinkConnectException>(() =>
                ownerContext.Router!.Disconnect(endpoint)
            );
            Assert.Equal(ZlinkConnectException.ErrorCode.NotFound, absent.Result);
            var retained = Assert.Single(owner.Peers(), peer => peer.RoutingId == middle.RoutingId);
            Assert.Equal(MeshPeerState.Admitted, retained.State);
            Assert.Equal(inbound.ConnectionIntentId, retained.ConnectionIntentId);
            Assert.Equal(inbound.LifecycleGeneration, retained.LifecycleGeneration);
            Assert.Equal(inbound.DescriptorRevision, retained.DescriptorRevision);
            Assert.Equal(inbound.LastChangedMs, retained.LastChangedMs);
            middle.PublishDraining();
            await WaitUntilAsync(() =>
                owner
                    .Peers()
                    .Any(peer =>
                        peer.RoutingId == middle.RoutingId && peer.State == MeshPeerState.Draining
                    )
            );
        }
        await WaitUntilAsync(() => owner.Status().AdmittedPeerCount == 0);
        var scheduler = new GatedTaskScheduler();
        await using var replacement = new ZLinkManagedMeshNode(
            context,
            "mesh",
            routedSubmitScheduler: scheduler
        );
        replacement.SetRoutingId(RoutingId.From("mesh-z-replacement"));
        replacement.SetBind(endpoint);
        replacement.Start();
        try
        {
            var replacementIntent = owner.ConnectPeer(endpoint, replacement.RoutingId);
            // MeshNode §7.1: the replacement records the owner as admitted when it
            // accepts the Hello, then submits its Admit; the owner admits on receipt.
            await WaitUntilAsync(() => replacement.Status().AdmittedPeerCount == 1);
            Assert.Equal(MeshPeerState.Admitted, Assert.Single(replacement.Peers()).State);
            await WaitUntilAsync(() => owner.Status().AdmittedPeerCount == 1);
            replacement.ObserveSpotAuthority(
                owner.RoutingId,
                spot.SpotId,
                spot.LifecycleGeneration,
                owner.Status().LifecycleGeneration,
                spot.AuthorityOwnerGeneration,
                7
            );
            var operation = replacement.AllocateOperationId();
            Assert.Equal(
                SubmitResult.Ok,
                replacement.TryRequestCanonicalActorJoin(
                    new ZLinkBackendCanonicalActorJoinRequest(
                        new ZLinkBackendActorRef(replacement.RoutingId, "a1", 11),
                        replacement.Status().LifecycleGeneration,
                        13,
                        7,
                        false,
                        owner.RoutingId,
                        spot.SpotId,
                        spot.LifecycleGeneration,
                        owner.Status().LifecycleGeneration,
                        spot.AuthorityOwnerGeneration,
                        7,
                        "ZLinkFrameworkActorJoinRequest",
                        "application/json",
                        "{}"u8.ToArray()
                    ),
                    operation,
                    TimeSpan.FromSeconds(2)
                )
            );
            // A reverse record from the owner reaches the replacement while the join
            // is outstanding (the owner replies to it only below). The admitted peer
            // authenticates that ingress; no binding internals are used.
            using (
                var header = Message.From(
                    ZLinkServiceWireCodec.EncodeApplication(
                        ServiceWireConstants.Command.NodeSend,
                        0,
                        null,
                        false
                    )
                )
            )
            using (
                var payload = Message.From(
                    ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipart(
                        new ReadOnlyMemory<byte>[] { "reverse-before-ready"u8.ToArray() }
                    )
                )
            )
                ownerContext
                    .Router!.Send(replacement.RoutingId)
                    .Message(header)
                    .Message(payload)
                    .Submit();
            MeshReceiveRecord? reverse = null;
            await WaitUntilAsync(() =>
            {
                foreach (var record in DrainRecords(replacement))
                {
                    Assert.NotEqual(operation, record.OperationId);
                    if (record.Kind == MeshRecordKind.NodeSend)
                        reverse = record;
                }
                return reverse is not null;
            });
            Assert.Equal(1U, replacement.Status().AdmittedPeerCount);
            scheduler.Release();

            var joins = 0;
            int? completion = null;
            await WaitUntilAsync(() =>
            {
                foreach (var record in DrainRecords(owner))
                {
                    if (record.OperationKind != MeshOperationKind.ActorJoin)
                        continue;
                    joins++;
                    Assert.Equal(
                        SubmitResult.Ok,
                        record.ReplyJoin(ActorJoinResult.Accepted, Array.Empty<Message>())
                    );
                }
                foreach (var record in DrainRecords(replacement))
                {
                    if (record.OperationId != operation)
                        continue;
                    completion = record.TerminalResult;
                }
                return completion is not null;
            });
            Assert.Equal((int)RequestResult.Ok, completion);
            Assert.Equal(1, joins);
            var outbound = Assert.Single(
                owner.Peers(),
                peer => peer.RoutingId == replacement.RoutingId
            );
            Assert.Equal(replacementIntent, outbound.ConnectionIntentId);
            Assert.Equal(MeshPeerState.Admitted, outbound.State);
            Assert.Equal(MeshPeerState.Admitted, Assert.Single(replacement.Peers()).State);
        }
        finally
        {
            scheduler.Release();
        }
    }

    [Fact]
    public async Task ReplacingUnknownRidIntent_PreservesOutboundConnection()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var owner = NewNode(context, "mesh-mid");
        var scheduler = new GatedTaskScheduler();
        await using var replacement = new ZLinkManagedMeshNode(
            context,
            "mesh",
            routedSubmitScheduler: scheduler
        );
        replacement.SetRoutingId(RoutingId.From("mesh-z-replacement"));
        replacement.SetBind("tcp://127.0.0.1:0");
        replacement.Start();
        var endpoint = Assert.IsType<string>(replacement.Status().LocalEndpoint);
        owner.SetBind("tcp://127.0.0.1:0");
        owner.Start();
        var staleIntent = owner.ConnectPeer(endpoint);
        var replacementIntent = owner.ConnectPeer(endpoint, replacement.RoutingId);
        Assert.NotEqual(staleIntent, replacementIntent);
        try
        {
            await scheduler.Queued.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.True(owner.RemovePeerConnectionIfNotAdmitted(staleIntent));
            Assert.Equal(replacementIntent, Assert.Single(owner.Peers()).ConnectionIntentId);
            scheduler.Release();
            await WaitUntilAsync(() =>
                owner.Status().AdmittedPeerCount == 1 && replacement.Status().AdmittedPeerCount == 1
            );
            Assert.Equal(replacementIntent, Assert.Single(owner.Peers()).ConnectionIntentId);
            Assert.False(owner.RemovePeerConnectionIfNotAdmitted(replacementIntent));
        }
        finally
        {
            scheduler.Release();
        }
    }

    private sealed class CapturingMeshSocketContext(IContext inner) : IContext
    {
        public IRouterSocket? Router { get; private set; }
        public ZlinkConnectException? DisconnectFailure { get; set; }
        public int DisconnectRidCalls { get; private set; }
        public IContextOptions Options => inner.Options;

        public IRouterSocket CreateRouterSocket()
        {
            var router = inner.CreateRouterSocket();
            Router = router;
            if (DisconnectFailure is null)
                return router;

            var proxy = DispatchProxy.Create<IRouterSocket, CapturingRouterSocketProxy>();
            ((CapturingRouterSocketProxy)(object)proxy).Initialize(
                router,
                () =>
                {
                    DisconnectRidCalls++;
                    if (DisconnectFailure is not { } failure)
                        return;
                    DisconnectFailure = null;
                    throw failure;
                }
            );
            return proxy;
        }

        public void DisposeCapturedRouter() => Router?.Dispose();

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

        public class CapturingRouterSocketProxy : DispatchProxy
        {
            private IRouterSocket _inner = null!;
            private Action? _beforeDisconnectRid;
            internal IRouterSocket Inner => _inner;

            public void Initialize(IRouterSocket inner, Action beforeDisconnectRid)
            {
                _inner = inner;
                _beforeDisconnectRid = beforeDisconnectRid;
            }

            protected override object? Invoke(MethodInfo? targetMethod, object?[]? args)
            {
                ArgumentNullException.ThrowIfNull(targetMethod);
                if (targetMethod.Name == nameof(IRouterSocket.DisconnectRid))
                    _beforeDisconnectRid?.Invoke();
                try
                {
                    return targetMethod.Invoke(_inner, args);
                }
                catch (TargetInvocationException error) when (error.InnerException is not null)
                {
                    ExceptionDispatchInfo.Capture(error.InnerException).Throw();
                    throw;
                }
            }
        }
    }
}
