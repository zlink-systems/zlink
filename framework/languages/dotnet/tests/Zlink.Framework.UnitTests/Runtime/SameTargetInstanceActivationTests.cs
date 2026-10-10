using System.Security.Cryptography;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Configuration;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Messaging;
using Zlink.Framework.Runtime.Service;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class EntrySpotActorDispatchTests
{
    [Theory]
    [InlineData("disabled")]
    [InlineData("recreate")]
    [InlineData("unregistered")]
    public async Task EndedInstanceIntentUsesStoredTypeMeshAndTargetPolicy(string policy)
    {
        ReclaimedColdSpot.Initializations = 0;
        var node = new JoinedActivationNode();
        var blobs = new InMemoryRelocationStore();
        var (runtime, _) = await CreateStartedRuntimeAsync(
            node,
            includeActorFactory: false,
            includeInstanceSpotRoute: true,
            instanceSpotType: typeof(ReclaimedColdSpot),
            relocationStore: blobs
        );
        try
        {
            var store = RequireLocationStore(runtime);
            var descriptor = Assert.Single(
                await store.ListAllMeshNodesAsync("entry", CancellationToken.None),
                row => row.Rid == node.RoutingId
            );
            var oldOwner = Assert
                .IsType<ZLinkOwnerLeaseClaimResult.Claimed>(
                    await store.ClaimOwnerLeaseAsync(
                        "ended-instance-owner",
                        TimeSpan.FromMinutes(2)
                    )
                )
                .Token;
            var oldDescriptor = descriptor with
            {
                Rid = RoutingId.From("old-instance-node"),
                OwnerId = oldOwner.OwnerId,
                LeaseGeneration = oldOwner.LeaseGeneration,
                EntrySpotId = "old-instance-entry",
            };
            _ = await store.UpdateMeshNodeAsync(oldDescriptor, ZLinkLocationWriteIntent.NewClaim);
            const string spotId = "ended-instance";
            var key = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId);
            var ready = ZLinkInstanceSpotAuthorityPayloadCodec.Encode(
                new ZLinkInstanceSpotAuthorityPayload(
                    ZLinkInstanceSpotAuthorityState.Ready,
                    spotId,
                    "Tests.InstanceSpot",
                    "entry",
                    oldDescriptor.Rid,
                    oldDescriptor.LifecycleGeneration,
                    oldOwner.OwnerId,
                    checked((ulong)oldOwner.LeaseGeneration),
                    null
                )
            );
            var reserved = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
                await store.ReserveAsync(
                    new ZLinkObjectReservationRequest(
                        ZLinkPlacementObjectKind.InstanceSpot,
                        key,
                        "Tests.InstanceSpot",
                        "old-intent",
                        SHA256.HashData(ready),
                        ready.Length,
                        new ZLinkMeshNodeDescriptorKey("entry", oldDescriptor.Rid),
                        oldDescriptor.LifecycleGeneration,
                        oldOwner,
                        ready,
                        new ZLinkCapacityVector(
                            0,
                            1,
                            new ZLinkSpotTypeCapacityDelta(
                                ZLinkPlacementObjectKind.InstanceSpot,
                                "Tests.InstanceSpot",
                                1
                            )
                        )
                    )
                )
            );
            var before = Assert
                .IsType<ZLinkObjectCommitResult.Committed>(
                    await store.CommitAsync(reserved.Reservation, ready)
                )
                .Snapshot;
            await store.ReleaseOwnerLeaseAsync(oldOwner);
            var address = new InstanceSpotIntentAddress("wrong-mesh", "wrong-type", spotId);
            var canonical = await runtime.ResolveInstanceSpotIntentAsync(
                address,
                CancellationToken.None
            );
            Assert.Equal("entry", canonical.MeshName);
            Assert.Equal("Tests.InstanceSpot", canonical.InstanceSpotType);
            await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
                await runtime.ResolveInstanceSpotHandleAsync(canonical, CancellationToken.None)
            );
            Assert.Null(
                await runtime.ResolveInstanceSpotHandleAsync(
                    canonical,
                    CancellationToken.None,
                    instanceIntent: true
                )
            );
            var registration = runtime.Registration.SpotNodes["entry"];
            if (policy == "recreate")
                registration.InstanceSpotRelocations["Tests.InstanceSpot"] =
                    registration.InstanceSpotRelocations["Tests.InstanceSpot"] with
                    {
                        PolicyKind = ZLinkObjectRelocationRegistration.RecreatePolicy,
                    };
            if (policy == "unregistered")
                registration.InstanceSpotRelocations.Remove("Tests.InstanceSpot");
            var target = new ZLinkInstanceSpotActivationTarget(
                store,
                new ZLinkProviderRelocationRepository(blobs),
                runtime.GetSpotNodeRuntime("entry").Catalog,
                node,
                registration,
                new ZLinkLocationOwnerToken(descriptor.OwnerId, descriptor.LeaseGeneration)
            );
            InstanceSpotActivationOperation Operation(ulong id) =>
                new(
                    new InstanceSpotActivationTarget(
                        "entry",
                        node.RoutingId,
                        descriptor.LifecycleGeneration,
                        spotId,
                        "Tests.InstanceSpot",
                        descriptor.DescriptorRevision.ToString(
                            System.Globalization.CultureInfo.InvariantCulture
                        )
                    ),
                    node.RoutingId,
                    descriptor.LifecycleGeneration,
                    "",
                    new MeshOperationId(1575, id),
                    true,
                    id,
                    checked((ulong)DateTimeOffset.UtcNow.AddSeconds(5).ToUnixTimeMilliseconds())
                );
            var parts = ZLinkClientCallCodec.EncodeEnvelopeParts(
                ZLinkClientCallCodec.CreateEnvelope(
                    ZLinkMessageKind.Request,
                    "entry",
                    ZLinkMessageNameResolver.ResolveFromMessage(new ProbeRouteMessage("new"))
                ),
                new ProbeRouteMessage("new"),
                runtime.Registration.Codecs
            );
            var payload = parts
                .Select(static part => (ReadOnlyMemory<byte>)part.ToArray())
                .ToArray();
            foreach (var part in parts)
                part.Dispose();
            if (policy == "disabled")
            {
                var results = await Task.WhenAll(
                    target
                        .ActivateAsync(Operation(1), null, payload, CancellationToken.None)
                        .AsTask(),
                    target
                        .ActivateAsync(Operation(2), null, payload, CancellationToken.None)
                        .AsTask()
                );
                Assert.All(results, terminal => Assert.Equal(RequestResult.Ok, terminal.Result));
                Assert.Equal(1, ReclaimedColdSpot.Initializations);
                var after = Assert
                    .IsType<ZLinkAuthorityReadResult.Found>(await store.ReadAuthorityAsync(key))
                    .Snapshot;
                Assert.True(after.ObjectGeneration > before.ObjectGeneration);
                var old = Assert.Single(
                    await store.ListAllMeshNodesAsync("entry", CancellationToken.None),
                    row => row.Rid == oldDescriptor.Rid
                );
                Assert.Equal(0, old.Capacity.Spots.Active);
            }
            else
            {
                var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
                    await target.ActivateAsync(Operation(1), null, payload, CancellationToken.None)
                );
                Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, error.Kind);
                Assert.Equal(0, ReclaimedColdSpot.Initializations);
                Assert.Equal(
                    before.StoreVersion,
                    Assert
                        .IsType<ZLinkAuthorityReadResult.Found>(await store.ReadAuthorityAsync(key))
                        .Snapshot.StoreVersion
                );
            }
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    private sealed class ReclaimedColdSpot(IZLinkInstanceSpotContext context) : IZLinkInstanceSpot
    {
        internal static int Initializations;
        public IZLinkInstanceSpotContext Context { get; } = context;

        public void Configure() => Context.Handlers.AddPacket<ReclaimedColdRequestHandler>();

        public ValueTask OnInitializeAsync(CancellationToken cancellationToken)
        {
            Initializations++;
            return ValueTask.CompletedTask;
        }
    }

    private sealed class ReclaimedColdRequestHandler
        : IZLinkSpotRequestHandler<ReclaimedColdSpot, ProbeRouteMessage, ProbeReply>
    {
        public ValueTask<ProbeReply> HandleAsync(
            ReclaimedColdSpot spot,
            ProbeRouteMessage message,
            CancellationToken cancellationToken
        ) => ValueTask.FromResult(new ProbeReply("new"));
    }

    [Fact]
    public async Task Same_target_cold_event_then_request_joins_activation_in_arrival_order()
    {
        JoinedColdSpot.Entered = new(TaskCreationOptions.RunContinuationsAsynchronously);
        JoinedColdSpot.Release = new(TaskCreationOptions.RunContinuationsAsynchronously);
        var node = new JoinedActivationNode();
        var blobs = new InMemoryRelocationStore();
        var (runtime, _) = await CreateStartedRuntimeAsync(
            node,
            includeActorFactory: false,
            includeInstanceSpotRoute: true,
            instanceSpotType: typeof(JoinedColdSpot),
            relocationStore: blobs
        );
        try
        {
            var store = RequireLocationStore(runtime);
            var descriptor = Assert.Single(
                await store.ListAllMeshNodesAsync("entry", CancellationToken.None),
                row => row.Rid == node.RoutingId
            );
            var target = new ZLinkInstanceSpotActivationTarget(
                store,
                new ZLinkProviderRelocationRepository(blobs),
                runtime.GetSpotNodeRuntime("entry").Catalog,
                node,
                runtime.Registration.SpotNodes["entry"],
                new ZLinkLocationOwnerToken(descriptor.OwnerId, descriptor.LeaseGeneration)
            );
            InstanceSpotActivationOperation Operation(bool request, ulong id) =>
                new(
                    new InstanceSpotActivationTarget(
                        "entry",
                        node.RoutingId,
                        descriptor.LifecycleGeneration,
                        "joined-cold",
                        "Tests.InstanceSpot",
                        descriptor.DescriptorRevision.ToString(
                            System.Globalization.CultureInfo.InvariantCulture
                        )
                    ),
                    node.RoutingId,
                    descriptor.LifecycleGeneration,
                    "source-spot",
                    new MeshOperationId(1571, id),
                    request,
                    request ? id : 0,
                    checked((ulong)DateTimeOffset.UtcNow.AddSeconds(5).ToUnixTimeMilliseconds())
                );
            IReadOnlyList<ReadOnlyMemory<byte>> Payload(bool request)
            {
                object message = request ? new ProbeRouteMessage("query") : new JoinedColdEvent();
                var parts = ZLinkClientCallCodec.EncodeEnvelopeParts(
                    ZLinkClientCallCodec.CreateEnvelope(
                        request ? ZLinkMessageKind.Request : ZLinkMessageKind.Command,
                        "entry",
                        ZLinkMessageNameResolver.ResolveFromMessage(message)
                    ),
                    message,
                    runtime.Registration.Codecs
                );
                var result = parts.Select(part => (ReadOnlyMemory<byte>)part.ToArray()).ToArray();
                ZLinkMessageParts.DisposeAll(parts);
                return result;
            }
            var first = target
                .ActivateAsync(Operation(false, 1), null, Payload(false), CancellationToken.None)
                .AsTask();
            await JoinedColdSpot.Entered.Task.WaitAsync(TimeSpan.FromSeconds(5));
            var second = target
                .ActivateAsync(Operation(true, 2), null, Payload(true), CancellationToken.None)
                .AsTask();
            JoinedColdSpot.Release.TrySetResult();
            Assert.Equal(RequestResult.Ok, (await first).Result);
            var ready = Assert.IsType<ZLinkAuthorityReadResult.Found>(
                await store.ReadAuthorityAsync(
                    ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey("joined-cold")
                )
            );
            Assert.Equal(ZLinkPlacementAllocationState.Active, ready.Snapshot.Allocation.State);
            Assert.True(
                ZLinkInstanceSpotAuthorityPayloadCodec.TryDecode(
                    ready.Snapshot.Payload.Span,
                    out var authority
                )
            );
            Assert.Equal(node.RoutingId, authority.NodeRid);
            Assert.Equal(node.MeshStatus().LifecycleGeneration, authority.NodeGeneration);
            Assert.NotNull(
                await runtime
                    .GetSpotNodeRuntime("entry")
                    .Catalog.TryGetInstanceActivationAsync(
                        "joined-cold",
                        "Tests.InstanceSpot",
                        ready.Snapshot.ObjectGeneration
                    )
            );
            var terminal = await second;
            Assert.Equal(RequestResult.Ok, terminal.Result);
            Assert.Equal(
                "1",
                ZLinkClientCallCodec
                    .DecodeEnvelopeReplyAndDispose<ProbeReply>(
                        terminal.ReplyParts.Select(Message.From).ToArray(),
                        "empty",
                        "failed",
                        runtime.Registration.Codecs
                    )
                    .Value
            );
        }
        finally
        {
            JoinedColdSpot.Release.TrySetResult();
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    private sealed class JoinedColdSpot(IZLinkInstanceSpotContext context) : IZLinkInstanceSpot
    {
        internal static TaskCompletionSource Entered = null!;
        internal static TaskCompletionSource Release = null!;
        internal int Events;
        public IZLinkInstanceSpotContext Context { get; } = context;

        public void Configure()
        {
            Context.Handlers.AddPacket<JoinedColdEventHandler>();
            Context.Handlers.AddPacket<JoinedColdRequestHandler>();
        }

        public async ValueTask OnInitializeAsync(CancellationToken cancellationToken)
        {
            Entered.TrySetResult();
            await Release.Task.WaitAsync(cancellationToken);
        }
    }

    private sealed class JoinedActivationNode : CapturingSpotNode, IZLinkBackendSpotNode
    {
        public new IZLinkBackendSpot GetOrCreateReservedSpot(
            string targetSpotId,
            ulong objectGeneration,
            ulong authorityOwnerGeneration,
            out bool created
        )
        {
            var spot = (CapturingSpot)GetOrCreateSpot(targetSpotId, out created);
            spot.LifecycleGeneration = objectGeneration;
            return spot;
        }
    }

    private sealed record JoinedColdEvent;

    private sealed class JoinedColdEventHandler
        : IZLinkSpotPacketHandler<JoinedColdSpot, JoinedColdEvent>
    {
        public ValueTask HandleAsync(
            JoinedColdSpot spot,
            JoinedColdEvent message,
            CancellationToken cancellationToken
        )
        {
            ++spot.Events;
            return ValueTask.CompletedTask;
        }
    }

    private sealed class JoinedColdRequestHandler
        : IZLinkSpotRequestHandler<JoinedColdSpot, ProbeRouteMessage, ProbeReply>
    {
        public ValueTask<ProbeReply> HandleAsync(
            JoinedColdSpot spot,
            ProbeRouteMessage message,
            CancellationToken cancellationToken
        ) =>
            ValueTask.FromResult(
                new ProbeReply(
                    spot.Events.ToString(System.Globalization.CultureInfo.InvariantCulture)
                )
            );
    }
}
