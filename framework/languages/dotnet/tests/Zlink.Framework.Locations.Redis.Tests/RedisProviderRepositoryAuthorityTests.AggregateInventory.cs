using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.Locations.Redis.Tests;

public sealed partial class RedisProviderRepositoryAuthorityTests
{
    [SkippableFact]
    public async Task DirectJoinInventoryRoundTripsAtGenerationOne()
    {
        Skip.IfNot(fixture.RedisAvailable, fixture.SkipReason);
        await using var provider = fixture.CreateStore();
        var writer = new ZLinkProviderLocationRepository(provider);
        var reader = new ZLinkProviderLocationRepository(provider);
        var id = Guid.NewGuid();
        var fence = ZLinkAggregateInventory.RelocationFence(id);
        var key = ZLinkActorAuthorityPayloadCodec.AuthorityKey("direct-join");
        var payload = new byte[] { 0, 127, 255 };
        var participant = ZLinkDirectRelocationInventory.Participant(key, "original", payload);
        var digest = await writer.SealAggregateInventoryAsync(fence, [participant], default);

        Assert.Equal(1UL, fence.AggregateGeneration);
        Assert.IsType<ZLinkStoreReadResult.Found>(
            await provider.ReadAsync(new($"zlink:v11:aggregate-inventory:{id:D}:1:root"))
        );
        Assert.Equal(
            digest,
            await writer.SealAggregateInventoryAsync(fence, [participant], default)
        );
        var stored = Assert.Single(
            await reader.ReadAggregateInventoryAsync(fence, digest, default)
        );
        Assert.Equal(key.Value, stored.Entry.AuthorityKey);
        Assert.Equal("original", stored.Entry.ExpectedStoreVersion);
        Assert.Equal(payload, stored.Payload.ToArray());
    }

    [SkippableTheory]
    [InlineData("unchanged")]
    [InlineData("before-restore")]
    [InlineData("after-restore")]
    public async Task SpotWideInventoryUsesCapturedPrimaryAndSealedMemberVersion(string change)
    {
        Skip.IfNot(fixture.RedisAvailable, fixture.SkipReason);
        await using var provider = fixture.CreateStore();
        var writer = new ZLinkProviderLocationRepository(provider);
        var reader = new ZLinkProviderLocationRepository(provider);
        var owner = await ClaimAsync(writer, "source-owner");
        var descriptor = SpotDescriptor("source", owner);
        var targetOwner = await ClaimAsync(writer, "target-owner");
        var targetDescriptor = SpotDescriptor("target", targetOwner);
        await writer.UpdateMeshNodeAsync(targetDescriptor, ZLinkLocationWriteIntent.NewClaim);
        await writer.UpdateMeshNodeAsync(descriptor, ZLinkLocationWriteIntent.NewClaim);
        const string spotId = "matrix-room";
        const string actorId = "matrix-actor";
        var spotRequest = SpotReservation(spotId, descriptor, owner);
        var spot = await CreateAsync(
            spotRequest,
            ZLinkUserSpotAuthorityPayloadCodec.Encode(
                new ZLinkUserSpotAuthorityPayload(
                    ZLinkUserSpotAuthorityState.Ready,
                    "player",
                    spotId,
                    owner.OwnerId,
                    (ulong)owner.LeaseGeneration,
                    "play",
                    descriptor.Rid,
                    1
                )
            )
        );
        var actorRequest = Reservation(actorId, descriptor, owner);
        var actor = await CreateAsync(
            actorRequest,
            ZLinkActorAuthorityPayloadCodec.Encode(
                new ZLinkActorAuthorityPayload(
                    ZLinkActorAuthorityState.Ready,
                    "player",
                    actorId,
                    spotId,
                    spot.ObjectGeneration,
                    ZLinkSpotKind.User,
                    owner.OwnerId,
                    (ulong)owner.LeaseGeneration,
                    "play",
                    descriptor.Rid,
                    1
                )
            )
        );
        var id = Guid.NewGuid();
        var fence = new ZLinkAggregateFence(id, 1);
        var participants = new[]
        {
            Participant(spotRequest.Key, ZLinkPlacementObjectKind.UserSpot, spot),
            Participant(actorRequest.Key, ZLinkPlacementObjectKind.Actor, actor),
        };
        var digest = await writer.SealAggregateInventoryAsync(
            fence,
            ZLinkAggregateInventoryDigest.Authorities(participants),
            default
        );
        var captured = await ZLinkSpotRelocationSourceInventory.CaptureAsync(
            writer,
            fence,
            spotRequest.Key,
            spot,
            digest,
            1,
            default
        );
        Assert.NotEqual(spot.StoreVersion, captured.StoreVersion);
        var currentActor = Assert
            .IsType<ZLinkAuthorityReadResult.Found>(
                await reader.ReadAuthorityAsync(actorRequest.Key)
            )
            .Snapshot;
        Assert.Equal(actor.StoreVersion, currentActor.StoreVersion);
        Assert.Null(currentActor.Relocation);
        var currentSpot = Assert
            .IsType<ZLinkAuthorityReadResult.Found>(
                await reader.ReadAuthorityAsync(spotRequest.Key)
            )
            .Snapshot;
        Assert.Equal(ServiceWireCodec.RelocationPhase.Captured, currentSpot.Relocation?.Phase);
        Assert.Equal(digest, currentSpot.Relocation?.SourceInventorySha256);

        if (change == "before-restore")
            await ChangeMemberVersionAsync();

        var request = new ZLinkCanonicalSpotStageContext(
            id,
            1,
            1,
            "play",
            descriptor.Rid.ToHex(),
            1,
            owner.OwnerId,
            (ulong)owner.LeaseGeneration,
            targetDescriptor.Rid.ToHex(),
            targetDescriptor.LifecycleGeneration,
            targetOwner.OwnerId,
            (ulong)targetOwner.LeaseGeneration,
            spotId,
            "player",
            false,
            0,
            spot.StoreVersion
        );
        var envelope = new ZLinkRelocationEnvelope(
            id,
            1,
            new byte[32],
            [
                participants[1].Envelope with
                {
                    CanonicalParticipantId = 1,
                },
                participants[0].Envelope with
                {
                    CanonicalParticipantId = 2,
                },
            ]
        );
        var restore = ZLinkSpotRetireTargetRuntime
            .BindCanonicalAuthorityInventoryAsync(reader, envelope, request, default)
            .AsTask();
        if (change == "before-restore")
        {
            await Assert.ThrowsAsync<ZLinkRelocationDataLostException>(() => restore);
            return;
        }

        var result = await restore;
        Assert.Equal(actorRequest.Key, result.PublicationParticipants[0].Envelope.AuthorityKey);
        Assert.Equal(
            actor.StoreVersion,
            result.PublicationParticipants[0].CompareExchangeStoreVersion
        );
        Assert.Equal(
            currentSpot.StoreVersion,
            result.PublicationParticipants[1].CompareExchangeStoreVersion
        );
        if (change == "after-restore")
            await ChangeMemberVersionAsync();
        var attempt = new ZLinkAuthorityRelocation.TargetFence(
            targetDescriptor.Rid.ToHex(),
            targetDescriptor.LifecycleGeneration,
            targetOwner.OwnerId,
            (ulong)targetOwner.LeaseGeneration,
            1
        );
        var aggregate = new ZLinkAggregatePrepareRequest(
            id,
            fence.AggregateGeneration,
            ZLinkAggregateInventoryDigest.Authorities(result.PublicationParticipants),
            result.Envelope.InventoryDigest,
            new ZLinkMeshNodeDescriptorKey("play", targetDescriptor.Rid),
            targetDescriptor.LifecycleGeneration,
            new ZLinkCapacityVector(
                1,
                1,
                new ZLinkSpotTypeCapacityDelta(ZLinkPlacementObjectKind.UserSpot, "player", 1)
            ),
            targetOwner
        )
        {
            RepresentativeKey = spotRequest.Key,
            Relocation = captured.Relocation! with { Target = attempt },
        };
        var prepare = await reader.PrepareAggregateAsync(aggregate);
        if (change == "after-restore")
        {
            Assert.IsType<ZLinkAggregatePrepareResult.Conflict>(prepare);
            return;
        }

        Assert.IsType<ZLinkAggregatePrepareResult.Prepared>(prepare);
        var memberKey = ZLinkProviderLocationRepository.AuthorityMetaKey(actorRequest.Key);
        var memberBefore = Assert
            .IsType<ZLinkStoreReadResult.Found>(await provider.ReadAsync(memberKey))
            .Value;
        var retry = await ZLinkSpotRetireTargetRuntime.BindCanonicalAuthorityInventoryAsync(
            reader,
            envelope,
            request with
            {
                TargetAttemptGeneration = 2,
            },
            default
        );
        Assert.Equal(attempt, retry.PreparedTarget);
        Assert.IsType<ZLinkAggregatePrepareResult.AlreadyPrepared>(
            await reader.PrepareAggregateAsync(
                aggregate with
                {
                    Participants = ZLinkAggregateInventoryDigest.Authorities(
                        retry.PublicationParticipants
                    ),
                    Relocation = aggregate.Relocation! with
                    {
                        Target = attempt with { AttemptGeneration = 2 },
                    },
                }
            )
        );
        Assert.Equal(
            memberBefore.Version,
            Assert
                .IsType<ZLinkStoreReadResult.Found>(await provider.ReadAsync(memberKey))
                .Value.Version
        );
        Assert.Equal(
            ZLinkAggregateCommitResult.Stale,
            await reader.CommitAggregateAttemptAsync(fence, 1, default)
        );
        Assert.Equal(
            ZLinkAggregateCommitResult.Committed,
            await reader.CommitAggregateAttemptAsync(fence, 2, default)
        );
        var moved = Assert
            .IsType<ZLinkAuthorityReadResult.Found>(
                await writer.ReadAuthorityAsync(actorRequest.Key)
            )
            .Snapshot;
        Assert.Equal(targetOwner.OwnerId, moved.OwnerId);
        Assert.True(
            ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(
                moved.Payload.Span,
                out var movedActor
            )
        );
        Assert.Equal(targetDescriptor.Rid, movedActor.NodeRid);
        Assert.Equal(targetOwner.OwnerId, movedActor.OwnerId);
        Assert.Equal(spotId, movedActor.CurrentSpotId);
        Assert.Equal(spot.ObjectGeneration, movedActor.CurrentSpotGeneration);
        var inventory = await reader.ReadAggregateInventoryAsync(fence, digest, default);
        Assert.Equal(
            actor.Payload.ToArray(),
            inventory
                .Single(entry => entry.Entry.AuthorityKey == actorRequest.Key.Value)
                .Payload.ToArray()
        );

        async ValueTask ChangeMemberVersionAsync()
        {
            var changed = Assert.IsType<ZLinkAuthorityCompareExchangeResult.Stored>(
                await writer.CompareExchangeAuthorityAsync(
                    actorRequest.Key,
                    actor.StoreVersion,
                    new ZLinkAuthorityMutation.Put(
                        actor.Payload,
                        ZLinkAuthorityGenerationTransition.Preserve,
                        null,
                        null
                    )
                )
            );
            Assert.NotEqual(actor.StoreVersion, changed.Snapshot.StoreVersion);
        }

        async ValueTask<ZLinkAuthoritySnapshot> CreateAsync(
            ZLinkObjectReservationRequest reservation,
            ReadOnlyMemory<byte> payload
        )
        {
            var reserved = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
                await writer.ReserveAsync(reservation)
            );
            return Assert
                .IsType<ZLinkObjectCommitResult.Committed>(
                    await writer.CommitAsync(reserved.Reservation, payload)
                )
                .Snapshot;
        }

        static ZLinkAggregateRelocationParticipant Participant(
            ZLinkAuthorityKey key,
            ZLinkPlacementObjectKind kind,
            ZLinkAuthoritySnapshot authority
        ) =>
            new(
                new ZLinkRelocationParticipantEnvelope(
                    key,
                    kind,
                    authority.ObjectGeneration,
                    authority.AuthorityOwnerGeneration,
                    default,
                    [],
                    []
                ),
                authority.StoreVersion,
                ZLinkAuthorityGenerationTransition.NewOwner,
                authority.Payload,
                ZLinkAggregateInventory.NoMembershipMutation
            );
    }
}
