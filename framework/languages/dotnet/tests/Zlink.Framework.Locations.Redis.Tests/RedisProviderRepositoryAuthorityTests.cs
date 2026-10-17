using System.Security.Cryptography;
using System.Text;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.Locations.Redis.Tests;

[Collection(RedisTestCollection.Name)]
public sealed class RedisProviderRepositoryAuthorityTests(RedisTestFixture fixture)
{
    [SkippableFact]
    public async Task SeparateRepositoriesShareCreationAndRelocationAuthority()
    {
        Skip.IfNot(fixture.RedisAvailable, fixture.SkipReason);
        await using var sourceStore = fixture.CreateStore(out var keyPrefix);
        await using var targetStore = new ZLinkRedisLocationStore(
            new ZLinkRedisLocationOptions
            {
                ConnectionString = fixture.ConnectionString,
                KeyPrefix = keyPrefix,
            }
        );
        var source = new ZLinkProviderLocationRepository(sourceStore);
        var target = new ZLinkProviderLocationRepository(targetStore);
        var sourceOwner = await ClaimAsync(source, "source-owner");
        var targetOwner = await ClaimAsync(target, "target-owner");
        var sourceDescriptor = Descriptor("source", sourceOwner);
        var targetDescriptor = Descriptor("target", targetOwner);
        Assert.Equal(
            ZLinkLocationWriteStatus.Stored,
            (
                await source.UpdateMeshNodeAsync(
                    sourceDescriptor,
                    ZLinkLocationWriteIntent.NewClaim
                )
            ).Status
        );
        Assert.Equal(
            ZLinkLocationWriteStatus.Stored,
            (
                await target.UpdateMeshNodeAsync(
                    targetDescriptor,
                    ZLinkLocationWriteIntent.NewClaim
                )
            ).Status
        );
        var listed = await source.ListAllMeshNodesAsync("play");
        Assert.Equal(2, listed.Count);
        Assert.All(
            listed,
            candidate =>
                Assert.True(
                    Zlink.Framework.Runtime.Spots.ZLinkSpotRuntimeManager.IsEligibleCandidate(
                        candidate,
                        "player"
                    )
                )
        );

        var request = Reservation($"actor:redis:{Guid.NewGuid():N}", sourceDescriptor, sourceOwner);
        var reserved = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await target.ReserveAsync(request)
        );
        var created = Assert
            .IsType<ZLinkObjectCommitResult.Committed>(
                await source.CommitAsync(reserved.Reservation, new byte[] { 0x41 })
            )
            .Snapshot;
        Assert.Equal(
            created.StoreVersion,
            Assert
                .IsType<ZLinkAuthorityReadResult.Found>(
                    await target.ReadAuthorityAsync(request.Key)
                )
                .Snapshot.StoreVersion
        );

        var targetGeneration = created.AuthorityOwnerGeneration + 1;
        var moved = Assert
            .IsType<ZLinkAuthorityCompareExchangeResult.Stored>(
                await source.CompareExchangeAuthorityAsync(
                    request.Key,
                    created.StoreVersion,
                    new ZLinkAuthorityMutation.Put(
                        new byte[] { 0x42 },
                        ZLinkAuthorityGenerationTransition.NewOwner,
                        targetOwner,
                        created.Allocation with
                        {
                            Descriptor = new ZLinkMeshNodeDescriptorKey(
                                "play",
                                targetDescriptor.Rid
                            ),
                            DescriptorLifecycleGeneration = targetDescriptor.LifecycleGeneration,
                        },
                        targetGeneration
                    )
                )
            )
            .Snapshot;
        var observed = Assert
            .IsType<ZLinkAuthorityReadResult.Found>(await target.ReadAuthorityAsync(request.Key))
            .Snapshot;

        Assert.Equal(targetOwner.OwnerId, moved.OwnerId);
        Assert.Equal(targetOwner.OwnerId, observed.OwnerId);
        Assert.Equal(created.ObjectGeneration, observed.ObjectGeneration);
        Assert.Equal(targetGeneration, observed.AuthorityOwnerGeneration);
        Assert.Equal(targetDescriptor.Rid, observed.Allocation.Descriptor.Rid);
    }

    [SkippableTheory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task EndedUserSpotReleasesEndedOwnerMembers(bool liveMember)
    {
        Skip.IfNot(fixture.RedisAvailable, fixture.SkipReason);
        await using var store = fixture.CreateStore(out _);
        var repository = new ZLinkProviderLocationRepository(store);
        var oldOwner = await ClaimAsync(repository, "spot-old-owner");
        var oldDescriptor = SpotDescriptor("source", oldOwner);
        Assert.Equal(
            ZLinkLocationWriteStatus.Stored,
            (
                await repository.UpdateMeshNodeAsync(
                    oldDescriptor,
                    ZLinkLocationWriteIntent.NewClaim
                )
            ).Status
        );
        var newOwner = await ClaimAsync(repository, "spot-new-owner");
        var newDescriptor = SpotDescriptor("target", newOwner);
        Assert.Equal(
            ZLinkLocationWriteStatus.Stored,
            (
                await repository.UpdateMeshNodeAsync(
                    newDescriptor,
                    ZLinkLocationWriteIntent.NewClaim
                )
            ).Status
        );
        var spotId = $"redis-spot-{Guid.NewGuid():N}";
        var spot = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await repository.ReserveAsync(SpotReservation(spotId, oldDescriptor, oldOwner))
        );
        Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await repository.CommitAsync(
                spot.Reservation,
                Zlink.Framework.Runtime.Spots.ZLinkUserSpotAuthorityPayloadCodec.Encode(
                    new Zlink.Framework.Runtime.Spots.ZLinkUserSpotAuthorityPayload(
                        Zlink.Framework.Runtime.Spots.ZLinkUserSpotAuthorityState.Ready,
                        "player",
                        spotId,
                        oldOwner.OwnerId,
                        checked((ulong)oldOwner.LeaseGeneration),
                        "play",
                        oldDescriptor.Rid,
                        1
                    )
                )
            )
        );
        var memberId = $"redis-member-{Guid.NewGuid():N}";
        var (memberDescriptor, memberOwner) = liveMember
            ? (newDescriptor, newOwner)
            : (oldDescriptor, oldOwner);
        var member = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await repository.ReserveAsync(Reservation(memberId, memberDescriptor, memberOwner))
        );
        Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await repository.CommitAsync(
                member.Reservation,
                Zlink.Framework.Runtime.Actors.ZLinkActorAuthorityPayloadCodec.Encode(
                    new Zlink.Framework.Runtime.Actors.ZLinkActorAuthorityPayload(
                        Zlink.Framework.Runtime.Actors.ZLinkActorAuthorityState.Ready,
                        "player",
                        memberId,
                        spotId,
                        spot.Reservation.ObjectGeneration,
                        ZLinkSpotKind.User,
                        memberOwner.OwnerId,
                        checked((ulong)memberOwner.LeaseGeneration),
                        "play",
                        memberDescriptor.Rid,
                        1
                    )
                )
            )
        );
        await repository.ReleaseOwnerLeaseAsync(oldOwner);
        var next = SpotReservation(spotId, newDescriptor, newOwner) with
        {
            ObjectRelocationPolicy = Zlink
                .Framework
                .Runtime
                .Configuration
                .ZLinkObjectRelocationRegistration
                .DisabledPolicy,
        };

        if (liveMember)
        {
            var error =
                await Assert.ThrowsAsync<Zlink.Framework.Contracts.Errors.ZLinkFrameworkException>(
                    async () =>
                        await repository.ReserveAsync(next)
                );
            Assert.Equal(
                Zlink.Framework.Contracts.Errors.ZLinkFrameworkErrorKind.Unavailable,
                error.Kind
            );
            Assert.Equal(
                spot.Reservation.ObjectGeneration,
                Assert
                    .IsType<ZLinkAuthorityReadResult.Found>(
                        await repository.ReadAuthorityAsync(next.Key)
                    )
                    .Snapshot.ObjectGeneration
            );
            Assert.IsType<ZLinkAuthorityReadResult.Found>(
                await repository.ReadAuthorityAsync(member.Reservation.Key)
            );
            return;
        }
        var recreated = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await repository.ReserveAsync(next)
        );
        Assert.True(recreated.Reservation.ObjectGeneration > spot.Reservation.ObjectGeneration);
        Assert.IsType<ZLinkAuthorityReadResult.Missing>(
            await repository.ReadAuthorityAsync(member.Reservation.Key)
        );
        var again = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await repository.ReserveAsync(Reservation(memberId, newDescriptor, newOwner))
        );
        Assert.True(again.Reservation.ObjectGeneration > member.Reservation.ObjectGeneration);
    }

    private static ZLinkMeshNodeDescriptor SpotDescriptor(
        string node,
        ZLinkLocationOwnerToken owner
    )
    {
        var descriptor = Descriptor(node, owner);
        return descriptor with
        {
            ObjectCapabilities =
            [
                .. descriptor.ObjectCapabilities.Select(capability =>
                    capability.ObjectKind == ZLinkPlacementObjectKind.UserSpot
                        ? capability with
                        {
                            Limit = 10,
                        }
                        : capability
                ),
            ],
            Capacity = descriptor.Capacity with
            {
                Spots = new ZLinkPopulationCapacity(0, 0, 10),
                SpotTypes =
                [
                    new ZLinkSpotTypeCapacity(
                        ZLinkPlacementObjectKind.UserSpot,
                        "player",
                        0,
                        0,
                        10
                    ),
                ],
            },
        };
    }

    private static ZLinkObjectReservationRequest SpotReservation(
        string spotId,
        ZLinkMeshNodeDescriptor descriptor,
        ZLinkLocationOwnerToken owner
    ) =>
        Reservation(spotId, descriptor, owner) with
        {
            ObjectKind = ZLinkPlacementObjectKind.UserSpot,
            Key = ZLinkAuthorityKeyCodec.EncodeSpot(spotId),
            Capacity = new ZLinkCapacityVector(
                0,
                1,
                new ZLinkSpotTypeCapacityDelta(ZLinkPlacementObjectKind.UserSpot, "player", 1)
            ),
        };

    private static async ValueTask<ZLinkLocationOwnerToken> ClaimAsync(
        IZLinkLocationRepository repository,
        string ownerId
    ) =>
        Assert
            .IsType<ZLinkOwnerLeaseClaimResult.Claimed>(
                await repository.ClaimOwnerLeaseAsync(ownerId, TimeSpan.FromMinutes(2))
            )
            .Token;

    private static ZLinkMeshNodeDescriptor Descriptor(string node, ZLinkLocationOwnerToken owner) =>
        new(
            "play",
            RoutingId.From(node),
            1,
            1,
            $"tcp://127.0.0.1:{(node == "source" ? 7201 : 7202)}",
            new Dictionary<string, int>(StringComparer.Ordinal) { ["play"] = 100 },
            string.Empty,
            owner.OwnerId,
            owner.LeaseGeneration,
            default
        )
        {
            ObjectRole = ZLinkMeshNodeObjectRole.Server,
            ObjectCapabilities =
            [
                new ZLinkObjectCapability(
                    ZLinkPlacementObjectKind.Actor,
                    "player",
                    ZLinkObjectMaintenancePolicyKind.Disabled,
                    false,
                    0
                ),
                new ZLinkObjectCapability(
                    ZLinkPlacementObjectKind.UserSpot,
                    "player",
                    ZLinkObjectMaintenancePolicyKind.Disabled,
                    false,
                    0
                ),
            ],
            Capacity = new ZLinkPlacementCapacity(
                new ZLinkPopulationCapacity(0, 0, 100),
                new ZLinkPopulationCapacity(0, 0, 0),
                [new ZLinkSpotTypeCapacity(ZLinkPlacementObjectKind.UserSpot, "player", 0, 0, 0)]
            ),
            EntrySpotId =
                $"play-entry-00000000-0000-4000-8000-{(node == "source" ? "000000000011" : "000000000012")}",
            State = ZLinkFrameworkRuntimeState.Serving,
        };

    private static ZLinkObjectReservationRequest Reservation(
        string actorId,
        ZLinkMeshNodeDescriptor descriptor,
        ZLinkLocationOwnerToken owner
    )
    {
        var intent = Encoding.UTF8.GetBytes($"create:{actorId}");
        return new ZLinkObjectReservationRequest(
            ZLinkPlacementObjectKind.Actor,
            ZLinkAuthorityKeyCodec.EncodeActor(actorId),
            "player",
            $"inline:{actorId}",
            SHA256.HashData(intent),
            intent.Length,
            new ZLinkMeshNodeDescriptorKey(descriptor.MeshName, descriptor.Rid),
            descriptor.LifecycleGeneration,
            owner,
            new byte[] { 0x11 },
            new ZLinkCapacityVector(1, 0, null)
        );
    }
}
