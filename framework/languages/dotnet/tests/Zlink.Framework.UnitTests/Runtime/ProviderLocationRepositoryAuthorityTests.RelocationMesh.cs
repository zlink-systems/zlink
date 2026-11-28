using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests;

public sealed partial class ProviderLocationRepositoryAuthorityTests
{
    [Theory]
    [InlineData(false, false)]
    [InlineData(false, true)]
    [InlineData(true, false)]
    public async Task SpotAggregatePublishesTargetMesh(bool instanceSpot, bool wrappedActor)
    {
        const string spotId = "relocated-spot";
        const string actorId = "relocated-actor";
        var repository = new ZLinkProviderLocationRepository(
            new ZLinkInMemoryProviderLocationStore()
        );
        var sourceOwner = await ClaimAsync(repository, "source");
        var targetOwner = await ClaimAsync(repository, "target");
        var spotKind = instanceSpot
            ? ZLinkPlacementObjectKind.InstanceSpot
            : ZLinkPlacementObjectKind.UserSpot;
        var source = RelocationMeshDescriptor("source", sourceOwner, spotKind);
        var target = RelocationMeshDescriptor("target", targetOwner, spotKind);
        await PublishAsync(repository, source, ZLinkLocationWriteIntent.NewClaim);
        await PublishAsync(repository, target, ZLinkLocationWriteIntent.NewClaim);

        var spotReservation = Reservation(spotId, source, sourceOwner, spotKind);
        var spotPayload = instanceSpot
            ? ZLinkInstanceSpotAuthorityPayloadCodec.Encode(
                new(
                    ZLinkInstanceSpotAuthorityState.Ready,
                    spotId,
                    "player",
                    source.MeshName,
                    source.Rid,
                    source.LifecycleGeneration,
                    sourceOwner.OwnerId,
                    (ulong)sourceOwner.LeaseGeneration,
                    null
                )
            )
            : ZLinkUserSpotAuthorityPayloadCodec.Encode(
                new(
                    ZLinkUserSpotAuthorityState.Ready,
                    "player",
                    spotId,
                    sourceOwner.OwnerId,
                    (ulong)sourceOwner.LeaseGeneration,
                    source.MeshName,
                    source.Rid,
                    source.LifecycleGeneration
                )
            );
        var spot = await CommitRelocationMeshSourceAsync(repository, spotReservation, spotPayload);
        var fence = ZLinkAggregateInventory.RelocationFence(Guid.NewGuid());
        var participants = new List<ZLinkAggregateParticipant>
        {
            ZLinkDirectRelocationInventory.Participant(
                spotReservation.Key,
                spot.StoreVersion,
                spotPayload
            ),
        };
        var actorKey = ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorId);
        if (!instanceSpot)
        {
            var sourceActor = StandaloneActorRelocationPrecommitTests.Authority(
                actorId,
                source,
                sourceOwner
            ) with
            {
                StableType = "player",
                CurrentSpotId = spotId,
                CurrentSpotGeneration = spot.ObjectGeneration,
                CurrentSpotKind = ZLinkSpotKind.User,
            };
            var actorPayload = ZLinkActorAuthorityPayloadCodec.Encode(sourceActor);
            var actor = await CommitRelocationMeshSourceAsync(
                repository,
                Reservation(actorId, source, sourceOwner),
                actorPayload
            );
            participants.Add(
                ZLinkDirectRelocationInventory.Participant(
                    actorKey,
                    actor.StoreVersion,
                    wrappedActor
                        ? ZLinkActorRelocationAuthorityPayloadCodec.Encode(
                            new(
                                fence.AggregateId,
                                ZLinkActorRelocationAuthorityPhase.Activated,
                                default,
                                actorPayload
                            )
                        )
                        : actorPayload
                )
            );
        }
        var digest = await repository.SealAggregateInventoryAsync(fence, participants, default);
        var captured = await ZLinkSpotRelocationSourceInventory.CaptureAsync(
            repository,
            fence,
            spotReservation.Key,
            spot,
            digest,
            1,
            default
        );
        var targetDescriptor = new ZLinkMeshNodeDescriptorKey(target.MeshName, target.Rid);
        participants[0] = participants[0] with { CapturedStoreVersion = captured.StoreVersion };
        var request = new ZLinkAggregatePrepareRequest(
            fence.AggregateId,
            fence.AggregateGeneration,
            participants,
            Convert.FromHexString(digest),
            targetDescriptor,
            target.LifecycleGeneration,
            new(participants.Count - 1, 1, new(spotKind, "player", 1)),
            targetOwner
        )
        {
            RepresentativeKey = spotReservation.Key,
            Relocation = captured.Relocation! with
            {
                Target = new(
                    target.Rid.ToHex(),
                    target.LifecycleGeneration,
                    targetOwner.OwnerId,
                    (ulong)targetOwner.LeaseGeneration,
                    1
                ),
            },
        };
        Assert.IsType<ZLinkAggregatePrepareResult.Prepared>(
            await repository.PrepareAggregateAsync(request)
        );
        Assert.Equal(
            ZLinkAggregateCommitResult.Committed,
            await repository.CommitAggregateAttemptAsync(fence, 1, default)
        );

        var movedSpot = Assert
            .IsType<ZLinkAuthorityReadResult.Found>(
                await repository.ReadAuthorityAsync(spotReservation.Key)
            )
            .Snapshot;
        Assert.Equal(targetDescriptor, movedSpot.Allocation.Descriptor);
        if (instanceSpot)
        {
            Assert.True(
                ZLinkInstanceSpotAuthorityPayloadCodec.TryDecode(
                    movedSpot.Payload.Span,
                    out var instance
                )
            );
            Assert.Equal(target.MeshName, instance.MeshName);
            return;
        }

        var movedActor = Assert
            .IsType<ZLinkAuthorityReadResult.Found>(await repository.ReadAuthorityAsync(actorKey))
            .Snapshot;
        Assert.Equal(targetDescriptor, movedActor.Allocation.Descriptor);
        Assert.True(
            ZLinkUserSpotAuthorityPayloadCodec.TryDecode(movedSpot.Payload.Span, out var user)
        );
        Assert.True(
            ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(
                movedActor.Payload.Span,
                out var actorAuthority
            )
        );
        Assert.Equal(
            (target.MeshName, target.MeshName),
            (SpotMesh: user.MeshName, ActorMesh: actorAuthority.MeshName)
        );
    }

    private static ZLinkMeshNodeDescriptor RelocationMeshDescriptor(
        string name,
        ZLinkLocationOwnerToken owner,
        ZLinkPlacementObjectKind spotKind
    )
    {
        var descriptor = Descriptor(name, owner);
        return descriptor with
        {
            MeshName = $"{name}-mesh",
            ObjectCapabilities =
            [
                descriptor.ObjectCapabilities[0],
                descriptor.ObjectCapabilities[1] with
                {
                    ObjectKind = spotKind,
                },
            ],
            Capacity = descriptor.Capacity with
            {
                SpotTypes = [new(spotKind, "player", 0, 0, 100)],
            },
        };
    }

    private static async Task<ZLinkAuthoritySnapshot> CommitRelocationMeshSourceAsync(
        ZLinkProviderLocationRepository repository,
        ZLinkObjectReservationRequest request,
        byte[] payload
    )
    {
        var reserved = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await repository.ReserveAsync(request)
        );
        return Assert
            .IsType<ZLinkObjectCommitResult.Committed>(
                await repository.CommitAsync(reserved.Reservation, payload)
            )
            .Snapshot;
    }
}
