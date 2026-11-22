using System.Text.RegularExpressions;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests;

public sealed class EntrySpotIdentityStoreTests
{
    private const string EntrySpotId = "play-entry-00000000-0000-4000-8000-000000000001";
    private static readonly TimeSpan LeaseTtl = TimeSpan.FromMinutes(1);

    [Fact]
    public void EntrySpotId_UsesTheDiagnosticPrefixAndLowercaseUuidV4()
    {
        var spotId = ZLinkSpotId.CreateEntrySpotId("play");

        Assert.Matches(
            new Regex(
                "^play-entry-[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$",
                RegexOptions.CultureInvariant
            ),
            spotId
        );
        var error = Assert.Throws<ZLinkFrameworkException>(() =>
            ZLinkSpotId.RequireCallerProvided(spotId, "spotId")
        );
        Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, error.Kind);
    }

    [Fact]
    public async Task DuplicateEntrySpotId_DoesNotBlockDescriptorOrSpotReserve()
    {
        var store = new ZLinkInMemoryLocationStore();
        var ownerA = await ClaimOwnerAsync(store, "owner-a");
        var ownerB = await ClaimOwnerAsync(store, "owner-b");

        var first = await store.UpdateMeshNodeAsync(
            Descriptor(ownerA, "node-a", EntrySpotId),
            ZLinkLocationWriteIntent.NewClaim
        );
        var second = await store.UpdateMeshNodeAsync(
            Descriptor(ownerB, "node-b", EntrySpotId),
            ZLinkLocationWriteIntent.NewClaim
        );
        Assert.Equal(ZLinkLocationWriteStatus.Stored, first.Status);
        Assert.Equal(ZLinkLocationWriteStatus.Stored, second.Status);
        Assert.Equal(2, (await store.ListMeshNodesAsync("play", default)).Items.Count);

        Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await store.ReserveAsync(
                SpotReservation(ownerB, "node-b", ZLinkPlacementObjectKind.UserSpot)
            )
        );
        Assert.IsType<ZLinkObjectReserveResult.TypeMismatch>(
            await store.ReserveAsync(
                SpotReservation(ownerA, "node-a", ZLinkPlacementObjectKind.InstanceSpot)
            )
        );
        Assert.IsType<ZLinkObjectReserveResult.Conflict>(
            await store.ReserveAsync(
                SpotReservation(ownerA, "node-a", ZLinkPlacementObjectKind.UserSpot)
            )
        );
    }

    [Fact]
    public async Task DescriptorRemove_RequiresExactOwnerAndLeavesOtherDescriptor()
    {
        var store = new ZLinkInMemoryLocationStore();
        var ownerA = await ClaimOwnerAsync(store, "owner-a");
        var ownerB = await ClaimOwnerAsync(store, "owner-b");
        var descriptor = Descriptor(ownerA, "node-a", EntrySpotId);
        var key = new ZLinkMeshNodeDescriptorKey(descriptor.MeshName, descriptor.Rid);
        Assert.Equal(
            ZLinkLocationWriteStatus.Stored,
            (await store.UpdateMeshNodeAsync(descriptor, ZLinkLocationWriteIntent.NewClaim)).Status
        );
        Assert.Equal(
            ZLinkLocationWriteStatus.Stored,
            (
                await store.UpdateMeshNodeAsync(
                    Descriptor(ownerB, "node-b", EntrySpotId),
                    ZLinkLocationWriteIntent.NewClaim
                )
            ).Status
        );
        Assert.Equal(
            ZLinkLocationWriteStatus.IgnoredStale,
            await store.RemoveMeshNodeAsync(
                key,
                new ZLinkLocationOwnerToken(ownerA.OwnerId, checked(ownerA.LeaseGeneration + 1))
            )
        );
        Assert.Equal(2, (await store.ListMeshNodesAsync("play", default)).Items.Count);
        Assert.Equal(ZLinkLocationWriteStatus.Stored, await store.RemoveMeshNodeAsync(key, ownerA));
        var remaining = Assert.Single((await store.ListMeshNodesAsync("play", default)).Items);
        Assert.Equal(RoutingId.From("node-b"), remaining.Rid);
        Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await store.ReserveAsync(
                SpotReservation(ownerB, "node-b", ZLinkPlacementObjectKind.InstanceSpot)
            )
        );
    }

    private static async ValueTask<ZLinkLocationOwnerToken> ClaimOwnerAsync(
        ZLinkInMemoryLocationStore store,
        string ownerId
    )
    {
        var claimed = Assert.IsType<ZLinkOwnerLeaseClaimResult.Claimed>(
            await store.ClaimOwnerLeaseAsync(ownerId, LeaseTtl)
        );
        var read = Assert.IsType<ZLinkOwnerLeaseReadResult.Found>(
            await store.ReadOwnerLeaseAsync(ownerId)
        );
        Assert.Equal(claimed.Token, read.Token);
        return claimed.Token;
    }

    private static ZLinkMeshNodeDescriptor Descriptor(
        ZLinkLocationOwnerToken owner,
        string nodeRid,
        string entrySpotId
    ) =>
        new(
            "play",
            RoutingId.From(nodeRid),
            LifecycleGeneration: 1,
            DescriptorRevision: 1,
            $"tcp://127.0.0.1:{(nodeRid == "node-a" ? 7001 : 7002)}",
            new Dictionary<string, int>(StringComparer.Ordinal) { ["play"] = 100 },
            SecurityIdentity: string.Empty,
            OwnerId: owner.OwnerId,
            LeaseGeneration: owner.LeaseGeneration,
            UpdatedAt: default
        )
        {
            ObjectRole = ZLinkMeshNodeObjectRole.Server,
            ObjectCapabilities =
            [
                new ZLinkObjectCapability(
                    ZLinkPlacementObjectKind.UserSpot,
                    "match",
                    ZLinkObjectMaintenancePolicyKind.Disabled,
                    false,
                    100
                ),
                new ZLinkObjectCapability(
                    ZLinkPlacementObjectKind.InstanceSpot,
                    "match",
                    ZLinkObjectMaintenancePolicyKind.Disabled,
                    false,
                    100
                ),
            ],
            Capacity = new ZLinkPlacementCapacity(
                new ZLinkPopulationCapacity(0, 0, 0),
                new ZLinkPopulationCapacity(0, 0, 100),
                [
                    new ZLinkSpotTypeCapacity(
                        ZLinkPlacementObjectKind.UserSpot,
                        "match",
                        0,
                        0,
                        100
                    ),
                    new ZLinkSpotTypeCapacity(
                        ZLinkPlacementObjectKind.InstanceSpot,
                        "match",
                        0,
                        0,
                        100
                    ),
                ]
            ),
            EntrySpotId = entrySpotId,
            State = ZLinkFrameworkRuntimeState.Serving,
        };

    private static ZLinkObjectReservationRequest SpotReservation(
        ZLinkLocationOwnerToken owner,
        string nodeRid,
        ZLinkPlacementObjectKind objectKind
    ) =>
        new(
            objectKind,
            ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(EntrySpotId),
            "match",
            "inline-v1:",
            new byte[32],
            0,
            new ZLinkMeshNodeDescriptorKey("play", RoutingId.From(nodeRid)),
            1,
            owner,
            new byte[] { 1 },
            new ZLinkCapacityVector(0, 1, new ZLinkSpotTypeCapacityDelta(objectKind, "match", 1))
        );
}
