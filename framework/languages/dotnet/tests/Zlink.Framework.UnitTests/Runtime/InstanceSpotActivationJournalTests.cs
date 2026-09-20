using System.Buffers.Binary;
using System.Reflection;
using Systems.Zlink.Framework.Runtime.Protocol;
using System.Security.Cryptography;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class InstanceSpotActivationJournalTests
{
    [Fact]
    public void ReservedCrashRetainsCreatingFenceAndActivationRoot()
    {
        var expected = new ZLinkInstanceSpotAuthorityPayload(
            ZLinkInstanceSpotAuthorityState.Creating,
            "spot",
            "sample",
            "mesh",
            RoutingId.From("target"),
            3,
            "owner",
            5,
            null);

        var encoded = ZLinkInstanceSpotAuthorityPayloadCodec.Encode(expected);
        Assert.Equal("ZLAU", System.Text.Encoding.ASCII.GetString(encoded, 0, 4));
        Assert.True(ZLinkInstanceSpotAuthorityPayloadCodec.TryDecode(
            encoded,
            out var restored));
        Assert.Equal(expected, restored);
    }

    [Fact]
    public void ReadyAuthorityRoundTripsCanonicalActivationRecoveryPointer()
    {
        var recovery = new ZLinkInstanceSpotActivationRecoveryPointer(
            "activation-root",
            Enumerable.Range(0, 32).Select(static value => (byte)value).ToArray(),
            257,
            7,
            3);
        var expected = new ZLinkInstanceSpotAuthorityPayload(
            ZLinkInstanceSpotAuthorityState.Ready,
            "spot",
            "sample",
            "mesh",
            RoutingId.From("target"),
            3,
            "owner",
            5,
            recovery);

        var encoded = ZLinkInstanceSpotAuthorityPayloadCodec.Encode(expected);
        var nodeEncoded = Convert.FromHexString(
            "5a4c4155010000000000880002001203000f02000c0673616d706c650473706f74"
            + "056f776e65720000000000000005046d657368067461726765740000000000000003"
            + "00000000000100000046000f61637469766174696f6e2d726f6f7420000102030405"
            + "060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f0000010100000000"
            + "000000070000000000000003bfd1b955");

        Assert.Equal(nodeEncoded, encoded);
        Assert.True(ZLinkInstanceSpotAuthorityPayloadCodec.TryDecode(encoded, out var restored));
        Assert.Equal(recovery.Reference, restored.ActivationRecovery?.Reference);
        Assert.Equal(recovery.Sha256, restored.ActivationRecovery?.Sha256);
        Assert.Equal(recovery.EncodedSize, restored.ActivationRecovery?.EncodedSize);
        Assert.Equal(recovery.InboxSequence, restored.ActivationRecovery?.InboxSequence);
        Assert.Equal(recovery.ReplayCursor, restored.ActivationRecovery?.ReplayCursor);
        Assert.Equal(encoded, ZLinkInstanceSpotAuthorityPayloadCodec.Encode(restored));
    }

    [Fact]
    public void DecoderRejectsReplayCursorPastInboxSequence()
    {
        var encoded = Convert.FromHexString(
            "5a4c4155010000000000880002001203000f02000c0673616d706c650473706f74"
            + "056f776e65720000000000000005046d657368067461726765740000000000000003"
            + "00000000000100000046000f61637469766174696f6e2d726f6f7420000102030405"
            + "060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f0000010100000000"
            + "000000070000000000000003bfd1b955");
        encoded[^5] = 8;
        BinaryPrimitives.WriteUInt32BigEndian(
            encoded.AsSpan(encoded.Length - 4),
            ZLinkCrc32C.Compute(encoded.AsSpan(0, encoded.Length - 4)));

        Assert.False(ZLinkInstanceSpotAuthorityPayloadCodec.TryDecode(encoded, out _));
    }

    [Fact]
    public async Task ReadyCommitCrashBeforeQueueRestoreRetainsAcceptedRoot()
    {
        var (store, reservation, activationEnvelope) =
            await ReserveCreatingAsync("ready-crash");
        var reserved = Assert.IsType<ZLinkAuthorityReadResult.Found>(
            await store.ReadAuthorityAsync(reservation.Key));
        Assert.True(ZLinkInstanceSpotAuthorityPayloadCodec.TryDecode(
            reserved.Snapshot.Payload.Span,
            out var creating));
        await store.CommitAsync(
            reservation,
            ZLinkInstanceSpotAuthorityPayloadCodec.Encode(
                creating with
                {
                    State = ZLinkInstanceSpotAuthorityState.Ready,
                    ActivationRecovery = Recovery(activationEnvelope)
                }));

        var afterCrash = Assert.IsType<ZLinkAuthorityReadResult.Found>(
            await store.ReadAuthorityAsync(reservation.Key));
        Assert.True(ZLinkInstanceSpotAuthorityPayloadCodec.TryDecode(
            afterCrash.Snapshot.Payload.Span,
            out var restored));
        Assert.Equal(ZLinkInstanceSpotAuthorityState.Ready, restored.State);
        Assert.Equal(0UL, restored.ActivationRecovery?.ReplayCursor);
        Assert.Equal("activation-root", restored.ActivationRecovery?.Reference);
    }

    [Fact]
    public async Task ReserveCrashCanReconstructExactReservationAndCommitReady()
    {
        var (store, reservation, activationEnvelope) =
            await ReserveCreatingAsync("reserve-crash");
        var found = Assert.IsType<ZLinkAuthorityReadResult.Found>(
            await store.ReadAuthorityAsync(reservation.Key));
        var pending = Assert.IsType<ZLinkReservedObjectCreation>(
            found.Snapshot.ReservedCreation);
        var reconstructed = new ZLinkObjectReservation(
            reservation.Key,
            found.Snapshot.StoreVersion,
            found.Snapshot.ObjectGeneration,
            found.Snapshot.AuthorityOwnerGeneration,
            pending.ReservationId,
            found.Snapshot.Allocation.Descriptor,
            found.Snapshot.Allocation.DescriptorLifecycleGeneration,
            new ZLinkLocationOwnerToken(
                found.Snapshot.OwnerId,
                found.Snapshot.OwnerLeaseGeneration));
        Assert.True(ZLinkInstanceSpotAuthorityPayloadCodec.TryDecode(
            found.Snapshot.Payload.Span,
            out var creating));

        var committed = Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await store.CommitAsync(
                reconstructed,
                ZLinkInstanceSpotAuthorityPayloadCodec.Encode(
                    creating with
                    {
                        State = ZLinkInstanceSpotAuthorityState.Ready,
                        ActivationRecovery = Recovery(activationEnvelope)
                    })));

        Assert.Equal(ZLinkPlacementAllocationState.Active, committed.Snapshot.Allocation.State);
        Assert.Equal(reservation.ObjectGeneration, committed.Snapshot.ObjectGeneration);
    }

    [Fact]
    public async Task CompletionCursorCrashRetainsImmutableRecoveryRoot()
    {
        var (store, reservation, activationEnvelope) =
            await ReserveCreatingAsync("terminal-crash");
        var reserved = Assert.IsType<ZLinkAuthorityReadResult.Found>(
            await store.ReadAuthorityAsync(reservation.Key));
        Assert.True(ZLinkInstanceSpotAuthorityPayloadCodec.TryDecode(
            reserved.Snapshot.Payload.Span,
            out var creating));
        var ready = Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await store.CommitAsync(
                reservation,
                ZLinkInstanceSpotAuthorityPayloadCodec.Encode(
                    creating with
                    {
                        State = ZLinkInstanceSpotAuthorityState.Ready,
                        ActivationRecovery = Recovery(activationEnvelope)
                    })));
        var completedAuthority = creating with
        {
            State = ZLinkInstanceSpotAuthorityState.Ready,
            ActivationRecovery = Recovery(activationEnvelope) with { ReplayCursor = 1 }
        };

        Assert.IsType<ZLinkAuthorityCompareExchangeResult.Stored>(
            await store.CompareExchangeAuthorityAsync(
                reservation.Key,
                ready.Snapshot.StoreVersion,
                new ZLinkAuthorityMutation.Put(
                    ZLinkInstanceSpotAuthorityPayloadCodec.Encode(completedAuthority),
                    ZLinkAuthorityGenerationTransition.Preserve,
                    null,
                    null)));
        var afterCrash = Assert.IsType<ZLinkAuthorityReadResult.Found>(
            await store.ReadAuthorityAsync(reservation.Key));
        Assert.True(ZLinkInstanceSpotAuthorityPayloadCodec.TryDecode(
            afterCrash.Snapshot.Payload.Span,
            out var retained));
        Assert.Equal("activation-root", retained.ActivationRecovery?.Reference);
        Assert.Equal(1UL, retained.ActivationRecovery?.ReplayCursor);
        Assert.Equal(1UL, retained.ActivationRecovery?.InboxSequence);
    }

    [Fact]
    public async Task RestartReleasesDurablyCompletedRecoveryPointer()
    {
        var (store, reservation, activationEnvelope) =
            await ReserveCreatingAsync("completed-restart");
        var reserved = Assert.IsType<ZLinkAuthorityReadResult.Found>(
            await store.ReadAuthorityAsync(reservation.Key));
        Assert.True(ZLinkInstanceSpotAuthorityPayloadCodec.TryDecode(
            reserved.Snapshot.Payload.Span,
            out var creating));
        var completed = creating with
        {
            State = ZLinkInstanceSpotAuthorityState.Ready,
            ActivationRecovery = Recovery(activationEnvelope) with { ReplayCursor = 1 }
        };
        Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await store.CommitAsync(
                reservation,
                ZLinkInstanceSpotAuthorityPayloadCodec.Encode(completed)));
        var node = RecoverySpotNode.Create(creating.NodeRid, creating.NodeGeneration);
        var target = new ZLinkInstanceSpotActivationTarget(
            store,
            new InMemoryRelocationStore(),
            null!,
            node,
            null!,
            new ZLinkLocationOwnerToken(
                creating.OwnerId,
                checked((long)creating.OwnerLeaseGeneration)));

        await target.RecoverAsync(CancellationToken.None);

        var recovered = Assert.IsType<ZLinkAuthorityReadResult.Found>(
            await store.ReadAuthorityAsync(reservation.Key));
        Assert.True(ZLinkInstanceSpotAuthorityPayloadCodec.TryDecode(
            recovered.Snapshot.Payload.Span,
            out var cleared));
        Assert.Null(cleared.ActivationRecovery);
    }

    [Fact]
    public void ExactSpotIdFluentSurfaceExposesInstanceIntentAndMeshSelection()
    {
        Assert.Equal(
            typeof(IZLinkSpotSendCall),
            typeof(IZLinkSpotClient)
                .GetMethods()
                .Single(method =>
                    method.Name == nameof(IZLinkSpotClient.SendToSpot)
                    && method.GetParameters()[0].ParameterType == typeof(string))
                .ReturnType);
        Assert.NotNull(typeof(IZLinkSpotSendCall).GetMethod(
            nameof(IZLinkSpotSendCall.InstanceSpot),
            Type.EmptyTypes));
        Assert.NotNull(typeof(IZLinkSpotRequestCall).GetMethod(
            nameof(IZLinkSpotRequestCall.InMesh),
            [typeof(string)]));
    }

    [Fact]
    public async Task ConcurrentLoserJoinsRunningOperationInsteadOfSubmittingAgain()
    {
        var gate = new ZLinkInstanceSpotOperationGate();
        var entered = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var submissions = 0;
        async Task<InstanceSpotActivationTerminal> Execute()
        {
            Interlocked.Increment(ref submissions);
            entered.TrySetResult();
            await release.Task;
            return new InstanceSpotActivationTerminal(
                RequestResult.Ok,
                ServiceWireConstants.FrameworkErrorCode.None,
                [new byte[] { 9 }]);
        }

        var first = gate.RunAsync("spot\0operation", Execute);
        await entered.Task;
        var second = gate.RunAsync("spot\0operation", Execute);
        release.TrySetResult();

        var results = await Task.WhenAll(first, second);
        Assert.Equal(1, submissions);
        Assert.Equal(results[0].ReplyParts[0].ToArray(), results[1].ReplyParts[0].ToArray());
    }

    private static InstanceSpotActivationOperation Operation(MeshOperationId id) =>
        new(
            new InstanceSpotActivationTarget(
                "mesh",
                RoutingId.From("target"),
                3,
                "spot",
                "sample",
                "descriptor"),
            RoutingId.From("source"),
            5,
            "entry",
            id,
            true,
            7,
            4_102_444_800_000);

    private static ZLinkServiceWireCodec.RequestSourceFence RequestSource(
        InstanceSpotActivationOperation operation) =>
        new(
            "source-owner",
            11,
            operation.SourceNodeRid,
            operation.SourceNodeGeneration);

    private static async Task<(
        ZLinkInMemoryLocationStore Store,
        ZLinkObjectReservation Reservation,
        byte[] ActivationEnvelope)> ReserveCreatingAsync(string spotId)
    {
        var store = new ZLinkInMemoryLocationStore();
        var owner = Assert.IsType<ZLinkOwnerLeaseClaimResult.Claimed>(
            await store.ClaimOwnerLeaseAsync(
                $"owner-{spotId}",
                TimeSpan.FromMinutes(1)));
        var nodeRid = RoutingId.From($"node-{spotId}");
        var descriptor = new ZLinkMeshNodeDescriptor(
            "mesh",
            nodeRid,
            3,
            1,
            $"inproc://{spotId}",
            new Dictionary<string, int> { ["mesh"] = 100 },
            string.Empty,
            owner.Token.OwnerId,
            owner.Token.LeaseGeneration,
            DateTimeOffset.UtcNow)
        {
            ObjectRole = ZLinkMeshNodeObjectRole.Server,
            EntrySpotId = $"entry-{spotId}",
            State = ZLinkFrameworkRuntimeState.Serving,
            ObjectCapabilities =
            [
                new ZLinkObjectCapability(
                    ZLinkPlacementObjectKind.InstanceSpot,
                    "sample",
                    ZLinkObjectMaintenancePolicyKind.Disabled,
                    false,
                    0)
            ],
            Capacity = new ZLinkPlacementCapacity(
                new ZLinkPopulationCapacity(0, 0, 0),
                new ZLinkPopulationCapacity(0, 0, 0),
                [
                    new ZLinkSpotTypeCapacity(
                        ZLinkPlacementObjectKind.InstanceSpot,
                        "sample",
                        0,
                        0,
                        0)
                ])
        };
        Assert.Equal(
            ZLinkLocationWriteStatus.Stored,
            (await store.UpdateMeshNodeAsync(
                descriptor,
                ZLinkLocationWriteIntent.NewClaim)).Status);
        var operation = Operation(new MeshOperationId(101, 103));
        var envelope = ZLinkInstanceSpotActivationEnvelopeCodec.Encode(
            operation,
            RequestSource(operation),
            null,
            [new byte[] { 1 }]);
        var key = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId);
        var creating = new ZLinkInstanceSpotAuthorityPayload(
            ZLinkInstanceSpotAuthorityState.Creating,
            spotId,
            "sample",
            "mesh",
            nodeRid,
            3,
            owner.Token.OwnerId,
            checked((ulong)owner.Token.LeaseGeneration),
            null);
        var reserved = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await store.ReserveAsync(
                new ZLinkObjectReservationRequest(
                    ZLinkPlacementObjectKind.InstanceSpot,
                    key,
                    "sample",
                    "activation-root",
                    SHA256.HashData(envelope),
                    envelope.Length,
                    new ZLinkMeshNodeDescriptorKey("mesh", nodeRid),
                    3,
                    owner.Token,
                    ZLinkInstanceSpotAuthorityPayloadCodec.Encode(creating),
                    new ZLinkCapacityVector(
                        0,
                        1,
                        new ZLinkSpotTypeCapacityDelta(
                            ZLinkPlacementObjectKind.InstanceSpot,
                            "sample",
                            1)))));
        return (store, reserved.Reservation, envelope);
    }

    private static ZLinkInstanceSpotActivationRecoveryPointer Recovery(
        ReadOnlySpan<byte> envelope) =>
        new(
            "activation-root",
            SHA256.HashData(envelope),
            checked((uint)envelope.Length),
            1,
            0);

    private class RecoverySpotNode : DispatchProxy
    {
        private RoutingId routingId;
        private ulong generation;

        internal static IZLinkBackendSpotNode Create(
            RoutingId routingId,
            ulong generation)
        {
            var proxy = Create<IZLinkBackendSpotNode, RecoverySpotNode>();
            var state = (RecoverySpotNode)(object)proxy;
            state.routingId = routingId;
            state.generation = generation;
            return proxy;
        }

        protected override object? Invoke(MethodInfo? targetMethod, object?[]? args) =>
            targetMethod?.Name switch
            {
                "get_RoutingId" => routingId,
                nameof(IZLinkBackendSpotNode.MeshStatus) => new MeshNodeStatus(
                    MeshNodeState.Ready,
                    routingId,
                    "mesh",
                    string.Empty,
                    generation,
                    1,
                    0,
                    0,
                    0,
                    0,
                    0,
                    0,
                    0,
                    0,
                    0),
                _ => throw new NotSupportedException(targetMethod?.Name)
            };
    }
}
