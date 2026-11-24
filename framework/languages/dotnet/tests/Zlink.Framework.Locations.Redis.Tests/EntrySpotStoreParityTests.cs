using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.Locations.Redis.Tests;

[Collection(RedisTestCollection.Name)]
public sealed class EntrySpotStoreParityTests(RedisTestFixture fixture)
{
    private const string EntrySpotId = "play-entry-00000000-0000-4000-8000-000000000001";
    private const string MeshName = "play";
    private const string StableType = "room";

    [SkippableTheory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task DescriptorWritesAndSpotReserveAgreeAcrossStores(bool instanceFirst)
    {
        Skip.IfNot(fixture.RedisAvailable, fixture.SkipReason);
        await using var provider = fixture.CreateStore(out _);
        var kind = instanceFirst
            ? ZLinkPlacementObjectKind.InstanceSpot
            : ZLinkPlacementObjectKind.UserSpot;
        var redis = await WriteSequenceAsync(new ZLinkProviderLocationRepository(provider), kind);
        var memory = await WriteSequenceAsync(new ZLinkInMemoryLocationStore(), kind);

        Assert.Equal(
            (
                ZLinkLocationWriteStatus.Stored,
                ZLinkLocationWriteStatus.Stored,
                typeof(ZLinkObjectReserveResult.Reserved),
                ZLinkLocationWriteStatus.Stored,
                typeof(ZLinkObjectReserveResult.TypeMismatch)
            ),
            redis
        );
        Assert.Equal(redis, memory);
    }

    private static async Task<(
        ZLinkLocationWriteStatus First,
        ZLinkLocationWriteStatus Second,
        Type Reserve,
        ZLinkLocationWriteStatus Third,
        Type CompetingReserve
    )> WriteSequenceAsync(IZLinkLocationRepository store, ZLinkPlacementObjectKind kind)
    {
        var firstOwner = await ClaimAsync(store, "node-a");
        var first = await PublishAsync(store, firstOwner);
        var second = await PublishAsync(store, await ClaimAsync(store, "node-b"));
        var reserved = await store.ReserveAsync(Reservation(firstOwner, kind));
        var third = await PublishAsync(store, await ClaimAsync(store, "node-c"));
        var competingKind =
            kind == ZLinkPlacementObjectKind.UserSpot
                ? ZLinkPlacementObjectKind.InstanceSpot
                : ZLinkPlacementObjectKind.UserSpot;
        var competing = await store.ReserveAsync(Reservation(firstOwner, competingKind));
        return (first, second, reserved.GetType(), third, competing.GetType());
    }

    private static async Task<ZLinkLocationOwnerToken> ClaimAsync(
        IZLinkLocationRepository store,
        string node
    ) =>
        Assert
            .IsType<ZLinkOwnerLeaseClaimResult.Claimed>(
                await store.ClaimOwnerLeaseAsync(node, TimeSpan.FromMinutes(1))
            )
            .Token;

    private static async Task<ZLinkLocationWriteStatus> PublishAsync(
        IZLinkLocationRepository store,
        ZLinkLocationOwnerToken owner
    )
    {
        var kinds = new[]
        {
            ZLinkPlacementObjectKind.UserSpot,
            ZLinkPlacementObjectKind.InstanceSpot,
        };
        var descriptor = new ZLinkMeshNodeDescriptor(
            MeshName,
            RoutingId.From(owner.OwnerId),
            LifecycleGeneration: 1,
            DescriptorRevision: 1,
            "tcp://127.0.0.1:7000",
            new Dictionary<string, int>(),
            SecurityIdentity: owner.OwnerId,
            OwnerId: owner.OwnerId,
            LeaseGeneration: owner.LeaseGeneration,
            UpdatedAt: default
        )
        {
            ObjectRole = ZLinkMeshNodeObjectRole.Server,
            ObjectCapabilities = kinds
                .Select(kind => new ZLinkObjectCapability(
                    kind,
                    StableType,
                    ZLinkObjectMaintenancePolicyKind.Disabled,
                    false,
                    8
                ))
                .ToArray(),
            Capacity = new ZLinkPlacementCapacity(
                new ZLinkPopulationCapacity(0, 0, 0),
                new ZLinkPopulationCapacity(0, 0, 8),
                kinds.Select(kind => new ZLinkSpotTypeCapacity(kind, StableType, 0, 0, 8)).ToArray()
            ),
            EntrySpotId = EntrySpotId,
            State = ZLinkFrameworkRuntimeState.Serving,
        };
        return (
            await store.UpdateMeshNodeAsync(descriptor, ZLinkLocationWriteIntent.NewClaim)
        ).Status;
    }

    private static ZLinkObjectReservationRequest Reservation(
        ZLinkLocationOwnerToken owner,
        ZLinkPlacementObjectKind kind
    ) =>
        new(
            kind,
            ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(EntrySpotId),
            StableType,
            "inline-v1:",
            new byte[32],
            0,
            new ZLinkMeshNodeDescriptorKey(MeshName, RoutingId.From(owner.OwnerId)),
            1,
            owner,
            new byte[] { 1 },
            new ZLinkCapacityVector(0, 1, new ZLinkSpotTypeCapacityDelta(kind, StableType, 1))
        );
}
