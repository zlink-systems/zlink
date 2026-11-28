using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class SpotRelocationInventoryTests
{
    [Theory]
    [InlineData("intact")]
    [InlineData("second-attempt")]
    [InlineData("other-target")]
    [InlineData("missing")]
    [InlineData("damaged")]
    [InlineData("order")]
    public async Task StageReadsSealedInventoryBeforeRestore(string inventoryState)
    {
        var bytes = new ZLinkInMemoryProviderLocationStore();
        var store = new InventoryAuthorityStore(bytes);
        var fence = new ZLinkAggregateFence(Guid.NewGuid(), 1);
        var sourceNode = RoutingId.From("source");
        var coordinator = new ZLinkAuthorityRelocation.CoordinatorFence(
            sourceNode.ToHex(),
            1,
            "source-owner",
            1,
            "original-spot"
        );
        var spotKey = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey("room");
        var actorKey = ZLinkActorAuthorityPayloadCodec.AuthorityKey("actor");
        var actorPayload = ZLinkActorAuthorityPayloadCodec.Encode(
            new ZLinkActorAuthorityPayload(
                ZLinkActorAuthorityState.Ready,
                "player",
                "actor",
                "room",
                1,
                ZLinkSpotKind.User,
                "source-owner",
                1,
                "mesh",
                sourceNode,
                1
            )
        );
        byte[] spotPayload = [90, 23, 10];
        ZLinkAggregateParticipant[] sealedEntries =
        [
            new(
                spotKey,
                "original-spot",
                ZLinkAuthorityGenerationTransition.NewOwner,
                spotPayload,
                "[]"u8.ToArray()
            ),
            new(
                actorKey,
                "original-actor",
                ZLinkAuthorityGenerationTransition.NewOwner,
                actorPayload,
                "[]"u8.ToArray()
            ),
        ];
        var digest = await ZLinkAggregateInventory.SealAsync(
            bytes,
            "",
            fence,
            sealedEntries,
            default
        );
        store.Snapshots[spotKey] = Snapshot(ZLinkPlacementObjectKind.UserSpot, "room-type");
        store.Snapshots[actorKey] = Snapshot(ZLinkPlacementObjectKind.Actor, "player");
        var payloadKey = ZLinkAggregateInventory.ParticipantKey("", fence, 0, membership: false);
        if (inventoryState == "missing")
            await bytes.WriteAsync(
                new ZLinkStoreWriteRequest([], [new ZLinkStoreMutation.Delete(payloadKey)])
            );
        if (inventoryState == "damaged")
            await bytes.WriteAsync(
                new ZLinkStoreWriteRequest(
                    [],
                    [new ZLinkStoreMutation.Put(payloadKey, new byte[] { 0 }, null)]
                )
            );

        var request = new ZLinkCanonicalSpotStageContext(
            fence.AggregateId,
            fence.AggregateGeneration,
            inventoryState is "second-attempt" or "other-target" ? 2UL : 1UL,
            "mesh",
            sourceNode.ToHex(),
            1,
            "source-owner",
            1,
            RoutingId.From("target").ToHex(),
            1,
            "target-owner",
            1,
            "room",
            "room-type",
            false,
            42,
            coordinator.ExpectedStoreVersion
        );
        var actorState = State(1, 11);
        var spotState = State(2, 22);
        var envelope = new ZLinkRelocationEnvelope(
            fence.AggregateId,
            fence.AggregateGeneration,
            new byte[32],
            inventoryState == "order" ? [spotState, actorState] : [actorState, spotState]
        );
        if (inventoryState is "intact" or "second-attempt")
        {
            var (bound, participants, _) = await StageAsync();
            Assert.Equal(
                new[] { actorKey, spotKey },
                bound.Participants.Select(state => state.AuthorityKey)
            );
            Assert.Equal(new byte[] { 11 }, bound.Participants[0].ApplicationState.ToArray());
            Assert.Equal(new byte[] { 22 }, bound.Participants[1].ApplicationState.ToArray());
            Assert.Equal(
                digest,
                Convert.ToHexString(bound.InventoryDigest.Span).ToLowerInvariant()
            );
            Assert.Equal(actorPayload, participants[0].ApplicationAuthorityPayload.ToArray());
            Assert.Equal(spotPayload, participants[1].ApplicationAuthorityPayload.ToArray());
            Assert.Equal("original-spot", participants[1].ExpectedStoreVersion);
            Assert.Equal("captured", participants[1].CompareExchangeStoreVersion);
            Assert.Single(
                bound.Participants,
                participant => participant.ObjectKind == ZLinkPlacementObjectKind.Actor
            );
        }
        else if (inventoryState == "other-target")
        {
            var conflict = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                StageAsync().AsTask()
            );
            Assert.Equal(ZLinkFrameworkErrorKind.AlreadyExists, conflict.Kind);
        }
        else
        {
            await Assert.ThrowsAsync<ZLinkRelocationDataLostException>(() => StageAsync().AsTask());
        }

        async ValueTask<(
            ZLinkRelocationEnvelope,
            IReadOnlyList<ZLinkAggregateRelocationParticipant>,
            ZLinkAuthorityRelocation.TargetFence?
        )> StageAsync()
        {
            return await ZLinkSpotRetireTargetRuntime.BindCanonicalAuthorityInventoryAsync(
                store,
                envelope,
                request,
                default
            );
        }

        ZLinkAuthoritySnapshot Snapshot(ZLinkPlacementObjectKind kind, string stableType) =>
            new(
                kind == ZLinkPlacementObjectKind.Actor ? "original-actor" : "captured",
                Array.Empty<byte>(),
                1,
                1,
                "source-owner",
                1,
                new ZLinkPlacementAllocation(
                    ZLinkPlacementAllocationState.Active,
                    kind,
                    stableType,
                    new ZLinkMeshNodeDescriptorKey("mesh", sourceNode),
                    1,
                    new ZLinkCapacityVector(0, 0, null)
                ),
                null,
                DateTimeOffset.UtcNow
            )
            {
                Relocation =
                    kind == ZLinkPlacementObjectKind.Actor
                        ? null
                        : new ZLinkAuthorityRelocation(
                            fence.AggregateId,
                            inventoryState is "second-attempt" or "other-target"
                                ? ServiceWireCodec.RelocationPhase.Prepared
                                : ServiceWireCodec.RelocationPhase.Captured,
                            coordinator.Identity,
                            inventoryState is "second-attempt" or "other-target"
                                ? new(
                                    RoutingId
                                        .From(inventoryState == "other-target" ? "other" : "target")
                                        .ToHex(),
                                    1,
                                    "target-owner",
                                    1,
                                    1
                                )
                                : null,
                            coordinator,
                            digest,
                            1
                        ),
            };

        ZLinkRelocationParticipantEnvelope State(ulong id, byte value) =>
            new(spotKey, ZLinkPlacementObjectKind.UserSpot, 1, 1, new byte[] { value }, [], [])
            {
                CanonicalParticipantId = id,
            };
    }

    private sealed class InventoryAuthorityStore(IZLinkLocationStore records)
        : ZLinkLocationStoreTestDouble
    {
        internal Dictionary<ZLinkAuthorityKey, ZLinkAuthoritySnapshot> Snapshots { get; } = new();

        public override ValueTask<ZLinkAuthorityReadResult> ReadAuthorityAsync(
            ZLinkAuthorityKey key,
            CancellationToken cancellationToken = default
        ) =>
            ValueTask.FromResult<ZLinkAuthorityReadResult>(
                Snapshots.TryGetValue(key, out var snapshot)
                    ? new ZLinkAuthorityReadResult.Found(snapshot)
                    : new ZLinkAuthorityReadResult.Missing(DateTimeOffset.UtcNow)
            );

        public override ValueTask<
            IReadOnlyList<ZLinkAggregateInventory.Participant>
        > ReadAggregateInventoryAsync(
            ZLinkAggregateFence fence,
            string digest,
            CancellationToken cancellationToken
        ) => ZLinkAggregateInventory.ReadAsync(records, "", fence, digest, cancellationToken);
    }
}
