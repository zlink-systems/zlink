using Systems.Zlink.Framework.Runtime.Protocol;
using System.Security.Cryptography;
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
            "root-1",
            17,
            0);

        Assert.True(ZLinkInstanceSpotAuthorityPayloadCodec.TryDecode(
            ZLinkInstanceSpotAuthorityPayloadCodec.Encode(expected),
            out var restored));
        Assert.Equal(expected, restored);
    }

    [Fact]
    public async Task ReadyCommitCrashBeforeQueueRestoreRetainsAcceptedRoot()
    {
        var (store, reservation, _) = await ReserveCreatingAsync("ready-crash");
        var reserved = Assert.IsType<ZLinkAuthorityReadResult.Found>(
            await store.ReadAuthorityAsync(reservation.Key));
        Assert.True(ZLinkInstanceSpotAuthorityPayloadCodec.TryDecode(
            reserved.Snapshot.Payload.Span,
            out var creating));
        await store.CommitAsync(
            reservation,
            ZLinkInstanceSpotAuthorityPayloadCodec.Encode(
                creating with { State = ZLinkInstanceSpotAuthorityState.Ready }));

        var afterCrash = Assert.IsType<ZLinkAuthorityReadResult.Found>(
            await store.ReadAuthorityAsync(reservation.Key));
        Assert.True(ZLinkInstanceSpotAuthorityPayloadCodec.TryDecode(
            afterCrash.Snapshot.Payload.Span,
            out var restored));
        Assert.Equal(ZLinkInstanceSpotAuthorityState.Ready, restored.State);
        Assert.Equal(0UL, restored.ReplayCursor);
        Assert.Equal("activation-root", restored.RecoveryReference);
    }

    [Fact]
    public async Task ReserveCrashCanReconstructExactReservationAndCommitReady()
    {
        var (store, reservation, _) = await ReserveCreatingAsync("reserve-crash");
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
                    creating with { State = ZLinkInstanceSpotAuthorityState.Ready })));

        Assert.Equal(ZLinkPlacementAllocationState.Active, committed.Snapshot.Allocation.State);
        Assert.Equal(reservation.ObjectGeneration, committed.Snapshot.ObjectGeneration);
    }

    [Fact]
    public async Task TerminalPublicationCrashRetainsReplayableTerminalPointer()
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
                    creating with { State = ZLinkInstanceSpotAuthorityState.Ready })));
        var terminalRoot = ZLinkInstanceSpotActivationEnvelopeCodec.EncodeTerminal(
            activationEnvelope,
            new InstanceSpotActivationTerminal(
                RequestResult.Ok,
                ServiceWireConstants.FrameworkErrorCode.None,
                [new byte[] { 41 }]));
        var terminalAuthority = creating with
        {
            State = ZLinkInstanceSpotAuthorityState.Ready,
            RecoveryReference = "terminal-root",
            RecoveryChecksum = ZLinkCrc32C.Compute(terminalRoot),
            ReplayCursor = 1
        };

        Assert.IsType<ZLinkAuthorityCompareExchangeResult.Stored>(
            await store.CompareExchangeAuthorityAsync(
                reservation.Key,
                ready.Snapshot.StoreVersion,
                new ZLinkAuthorityMutation.Put(
                    ZLinkInstanceSpotAuthorityPayloadCodec.Encode(terminalAuthority),
                    ZLinkAuthorityGenerationTransition.Preserve,
                    null,
                    null)));
        var afterCrash = Assert.IsType<ZLinkAuthorityReadResult.Found>(
            await store.ReadAuthorityAsync(reservation.Key));
        Assert.True(ZLinkInstanceSpotAuthorityPayloadCodec.TryDecode(
            afterCrash.Snapshot.Payload.Span,
            out var retained));
        Assert.Equal(1UL, retained.ReplayCursor);
        Assert.True(ZLinkInstanceSpotActivationEnvelopeCodec.TryDecodeTerminal(
            terminalRoot,
            out _,
            out var terminal));
        Assert.Equal(41, terminal.ReplyParts[0].Span[0]);
    }

    [Fact]
    public void TerminalRootRetainsOriginalOperationAndReply()
    {
        var operation = Operation(new MeshOperationId(11, 17));
        var activation = ZLinkInstanceSpotActivationEnvelopeCodec.Encode(
            operation,
            RequestSource(operation),
            new byte[] { 1, 2 },
            [new byte[] { 3, 4 }]);
        var expected = new InstanceSpotActivationTerminal(
            RequestResult.Ok,
            ServiceWireConstants.FrameworkErrorCode.None,
            [new byte[] { 5, 6 }]);

        var encoded = ZLinkInstanceSpotActivationEnvelopeCodec.EncodeTerminal(
            activation,
            expected);

        Assert.True(ZLinkInstanceSpotActivationEnvelopeCodec.TryDecodeTerminal(
            encoded,
            out var original,
            out var terminal));
        Assert.Equal(operation.OperationId, original.OperationId);
        Assert.Equal(operation.SourceNodeRid, original.SourceNodeRid);
        Assert.Equal(RequestSource(operation), original.RequestSource);
        Assert.Equal(expected.Result, terminal.Result);
        Assert.Equal(expected.ReplyParts[0].ToArray(), terminal.ReplyParts[0].ToArray());
    }

    [Fact]
    public void DifferentOperationCannotMistakeRetainedTerminalForItsOwn()
    {
        var operation = Operation(new MeshOperationId(23, 29));
        var encoded = ZLinkInstanceSpotActivationEnvelopeCodec.EncodeTerminal(
            ZLinkInstanceSpotActivationEnvelopeCodec.Encode(
                operation,
                RequestSource(operation),
                null,
                [new byte[] { 7 }]),
            new InstanceSpotActivationTerminal(
                RequestResult.Ok,
                ServiceWireConstants.FrameworkErrorCode.None,
                []));

        Assert.True(ZLinkInstanceSpotActivationEnvelopeCodec.TryDecodeTerminal(
            encoded,
            out var original,
            out _));
        Assert.NotEqual(new MeshOperationId(31, 37), original.OperationId);
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
            "activation-root",
            ZLinkCrc32C.Compute(envelope),
            0);
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
}
