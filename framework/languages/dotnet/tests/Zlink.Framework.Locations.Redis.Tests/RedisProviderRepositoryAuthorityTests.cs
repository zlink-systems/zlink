using System.Security.Cryptography;
using System.Text;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.Locations.Redis.Tests;

[Collection(RedisTestCollection.Name)]
public sealed class RedisProviderRepositoryAuthorityTests(
    RedisTestFixture fixture)
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
                KeyPrefix = keyPrefix
            });
        var source = new ZLinkProviderLocationRepository(sourceStore);
        var target = new ZLinkProviderLocationRepository(targetStore);
        var sourceOwner = await ClaimAsync(source, "source-owner");
        var targetOwner = await ClaimAsync(target, "target-owner");
        var sourceDescriptor = Descriptor("source", sourceOwner);
        var targetDescriptor = Descriptor("target", targetOwner);
        Assert.Equal(
            ZLinkLocationWriteStatus.Stored,
            (await source.UpdateMeshNodeAsync(
                sourceDescriptor,
                ZLinkLocationWriteIntent.NewClaim)).Status);
        Assert.Equal(
            ZLinkLocationWriteStatus.Stored,
            (await target.UpdateMeshNodeAsync(
                targetDescriptor,
                ZLinkLocationWriteIntent.NewClaim)).Status);
        var listed = await source.ListAllMeshNodesAsync("play");
        Assert.Equal(2, listed.Count);
        Assert.All(
            listed,
            candidate => Assert.True(
                Zlink.Framework.Runtime.Spots.ZLinkSpotRuntimeManager
                    .IsEligibleCandidate(candidate, "player")));

        var request = Reservation(
            $"actor:redis:{Guid.NewGuid():N}",
            sourceDescriptor,
            sourceOwner);
        var reserved = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await target.ReserveAsync(request));
        var created = Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await source.CommitAsync(
                reserved.Reservation,
                new byte[] { 0x41 })).Snapshot;
        Assert.Equal(
            created.StoreVersion,
            Assert.IsType<ZLinkAuthorityReadResult.Found>(
                await target.ReadAuthorityAsync(request.Key))
                .Snapshot.StoreVersion);

        var relocation = new ZLinkRelocationCapacityReservationRequest(
            Guid.NewGuid(),
            request.Key,
            created.StoreVersion,
            ZLinkPlacementObjectKind.Actor,
            "player",
            new ZLinkMeshNodeDescriptorKey(
                "play",
                sourceDescriptor.Rid),
            sourceDescriptor.LifecycleGeneration,
            sourceOwner,
            new ZLinkMeshNodeDescriptorKey(
                "play",
                targetDescriptor.Rid),
            targetDescriptor.LifecycleGeneration,
            targetOwner,
            new ZLinkCapacityVector(1, 0, null));
        var capacity = Assert.IsType<
            ZLinkRelocationCapacityReserveResult.Reserved>(
            await target.ReserveRelocationCapacityAsync(relocation));
        var moved = Assert.IsType<ZLinkAuthorityCompareExchangeResult.Stored>(
            await source.CompareExchangeAuthorityAsync(
                request.Key,
                created.StoreVersion,
                new ZLinkAuthorityMutation.Put(
                    new byte[] { 0x42 },
                    ZLinkAuthorityGenerationTransition.NewOwner,
                    targetOwner,
                    capacity.Fence))).Snapshot;
        var observed = Assert.IsType<ZLinkAuthorityReadResult.Found>(
            await target.ReadAuthorityAsync(request.Key)).Snapshot;

        Assert.Equal(targetOwner.OwnerId, moved.OwnerId);
        Assert.Equal(targetOwner.OwnerId, observed.OwnerId);
        Assert.Equal(created.ObjectGeneration, observed.ObjectGeneration);
        Assert.True(
            observed.AuthorityOwnerGeneration
            > created.AuthorityOwnerGeneration);
        Assert.Equal(targetDescriptor.Rid, observed.Allocation.Descriptor.Rid);
    }

    private static async ValueTask<ZLinkLocationOwnerToken> ClaimAsync(
        IZLinkLocationRepository repository,
        string ownerId) =>
        Assert.IsType<ZLinkOwnerLeaseClaimResult.Claimed>(
            await repository.ClaimOwnerLeaseAsync(
                ownerId,
                TimeSpan.FromMinutes(2))).Token;

    private static ZLinkMeshNodeDescriptor Descriptor(
        string node,
        ZLinkLocationOwnerToken owner) =>
        new(
            "play",
            RoutingId.From(node),
            1,
            1,
            $"tcp://127.0.0.1:{(node == "source" ? 7201 : 7202)}",
            new Dictionary<string, int>(StringComparer.Ordinal)
            {
                ["play"] = 100
            },
            string.Empty,
            owner.OwnerId,
            owner.LeaseGeneration,
            default)
        {
            ObjectRole = ZLinkMeshNodeObjectRole.Server,
            ObjectCapabilities =
            [
                new ZLinkObjectCapability(
                    ZLinkPlacementObjectKind.Actor,
                    "player",
                    ZLinkObjectMaintenancePolicyKind.Disabled,
                    false,
                    0),
                new ZLinkObjectCapability(
                    ZLinkPlacementObjectKind.UserSpot,
                    "player",
                    ZLinkObjectMaintenancePolicyKind.Disabled,
                    false,
                    0)
            ],
            Capacity = new ZLinkPlacementCapacity(
                new ZLinkPopulationCapacity(0, 0, 100),
                new ZLinkPopulationCapacity(0, 0, 0),
                [
                    new ZLinkSpotTypeCapacity(
                        ZLinkPlacementObjectKind.UserSpot,
                        "player",
                        0,
                        0,
                        0)
                ]),
            EntrySpotId =
                $"play-entry-00000000-0000-4000-8000-{(node == "source" ? "000000000011" : "000000000012")}",
            State = ZLinkFrameworkRuntimeState.Serving
        };

    private static ZLinkObjectReservationRequest Reservation(
        string actorId,
        ZLinkMeshNodeDescriptor descriptor,
        ZLinkLocationOwnerToken owner)
    {
        var intent = Encoding.UTF8.GetBytes($"create:{actorId}");
        return new ZLinkObjectReservationRequest(
            ZLinkPlacementObjectKind.Actor,
            new ZLinkAuthorityKey(actorId),
            "player",
            $"inline:{actorId}",
            SHA256.HashData(intent),
            intent.Length,
            new ZLinkMeshNodeDescriptorKey(
                descriptor.MeshName,
                descriptor.Rid),
            descriptor.LifecycleGeneration,
            owner,
            new byte[] { 0x11 },
            new ZLinkCapacityVector(1, 0, null));
    }
}
