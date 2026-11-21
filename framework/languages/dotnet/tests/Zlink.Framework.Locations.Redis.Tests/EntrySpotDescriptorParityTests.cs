using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.Locations.Redis.Tests;

[Collection(RedisTestCollection.Name)]
public sealed class EntrySpotDescriptorParityTests(RedisTestFixture fixture)
{
    private const string EntryA = "mesh-entry-00000000-0000-4000-8000-000000000001";
    private const string EntryB = "mesh-entry-00000000-0000-4000-8000-000000000002";

    [SkippableTheory]
    [InlineData(EntryA, (int)ZLinkLocationWriteStatus.RejectedConflict)]
    [InlineData(EntryB, (int)ZLinkLocationWriteStatus.Stored)]
    public async Task NewClaimReturnsSameStatusInMemoryAndRedis(string entryId, int expected)
    {
        Skip.IfNot(fixture.RedisAvailable, fixture.SkipReason);
        await using var redis = fixture.CreateStore();
        var memoryStatus = await PublishSecondAsync(new ZLinkInMemoryLocationStore(), entryId);
        var redisStatus = await PublishSecondAsync(
            new ZLinkProviderLocationRepository(redis),
            entryId
        );

        Assert.Equal((ZLinkLocationWriteStatus)expected, memoryStatus);
        Assert.Equal(memoryStatus, redisStatus);
    }

    [SkippableFact]
    public async Task RenewAndCleanupPreserveTheExactEntryClaim()
    {
        Skip.IfNot(fixture.RedisAvailable, fixture.SkipReason);
        await using var redis = fixture.CreateStore();
        await VerifyClaimLifetimeAsync(new ZLinkInMemoryLocationStore());
        await VerifyClaimLifetimeAsync(new ZLinkProviderLocationRepository(redis));
    }

    [SkippableTheory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task GenericReserveSharesTheEntrySpotNamespace(bool instance)
    {
        Skip.IfNot(fixture.RedisAvailable, fixture.SkipReason);
        await using var redis = fixture.CreateStore();
        var kind = instance
            ? ZLinkPlacementObjectKind.InstanceSpot
            : ZLinkPlacementObjectKind.UserSpot;
        await VerifySharedNamespaceAsync(new ZLinkInMemoryLocationStore(), kind);
        await VerifySharedNamespaceAsync(new ZLinkProviderLocationRepository(redis), kind);
    }

    [SkippableFact]
    public async Task ExactNewClaimReplayRepairsMissingClaimWithoutChangingTheDescriptor()
    {
        Skip.IfNot(fixture.RedisAvailable, fixture.SkipReason);
        await using var redis = fixture.CreateStore();
        var repository = new ZLinkProviderLocationRepository(redis);
        var ownerA = await ClaimAsync(repository, "entry-owner-a");
        var ownerB = await ClaimAsync(repository, "entry-owner-b");
        var descriptor = Descriptor(ownerA, 1, EntryA);
        Assert.Equal(ZLinkLocationWriteStatus.Stored, await PublishAsync(repository, descriptor));

        var descriptorKey = ZLinkProviderLocationRepository.MeshKey(
            descriptor.MeshName,
            descriptor.Rid
        );
        var claimKey = new ZLinkStoreKey($"entry-spot-id\0{EntryA}");
        var originalDescriptor = Assert.IsType<ZLinkStoreReadResult.Found>(
            await redis.ReadAsync(descriptorKey)
        );
        var originalClaim = Assert.IsType<ZLinkStoreReadResult.Found>(
            await redis.ReadAsync(claimKey)
        );
        Assert.Equal(ZLinkLocationWriteStatus.Stored, await PublishAsync(repository, descriptor));
        Assert.Equal(
            originalClaim.Value.Version,
            Assert.IsType<ZLinkStoreReadResult.Found>(await redis.ReadAsync(claimKey)).Value.Version
        );

        await DeleteClaimAsync(redis, claimKey, originalClaim.Value.Version);
        Assert.Equal(ZLinkLocationWriteStatus.Stored, await PublishAsync(repository, descriptor));
        Assert.IsType<ZLinkStoreReadResult.Found>(await redis.ReadAsync(claimKey));
        Assert.Equal(
            originalDescriptor.Value.Version,
            Assert
                .IsType<ZLinkStoreReadResult.Found>(await redis.ReadAsync(descriptorKey))
                .Value.Version
        );
        Assert.Equal(
            ZLinkLocationWriteStatus.RejectedConflict,
            await PublishAsync(repository, Descriptor(ownerB, 2, EntryA))
        );
    }

    [SkippableFact]
    public async Task RemoveCannotOrphanClaimRestoredByConcurrentNewClaim()
    {
        Skip.IfNot(fixture.RedisAvailable, fixture.SkipReason);
        await using var redis = fixture.CreateStore();
        var repository = new ZLinkProviderLocationRepository(redis);
        var owner = await ClaimAsync(repository, "entry-owner-a");
        var descriptor = Descriptor(owner, 1, EntryA);
        var descriptorKey = new ZLinkMeshNodeDescriptorKey(descriptor.MeshName, descriptor.Rid);
        Assert.Equal(ZLinkLocationWriteStatus.Stored, await PublishAsync(repository, descriptor));

        var claimKey = new ZLinkStoreKey($"entry-spot-id\0{EntryA}");
        var claim = Assert.IsType<ZLinkStoreReadResult.Found>(await redis.ReadAsync(claimKey));
        await DeleteClaimAsync(redis, claimKey, claim.Value.Version);

        var blockedStore = new PauseDescriptorDelete(
            redis,
            ZLinkProviderLocationRepository.MeshKey(descriptor.MeshName, descriptor.Rid)
        );
        var removing = new ZLinkProviderLocationRepository(blockedStore)
            .RemoveMeshNodeAsync(descriptorKey, owner)
            .AsTask();
        try
        {
            await blockedStore.Entered.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Equal(
                ZLinkLocationWriteStatus.Stored,
                await PublishAsync(repository, descriptor)
            );
        }
        finally
        {
            blockedStore.Release.TrySetResult();
        }
        Assert.Equal(ZLinkLocationWriteStatus.IgnoredStale, await removing);
        Assert.IsType<ZLinkStoreReadResult.Found>(await redis.ReadAsync(claimKey));
        Assert.Equal(
            ZLinkLocationWriteStatus.Stored,
            await repository.RemoveMeshNodeAsync(descriptorKey, owner)
        );
        Assert.IsType<ZLinkStoreReadResult.Missing>(await redis.ReadAsync(claimKey));
    }

    [SkippableFact]
    public async Task ConcurrentNewClaimsHaveOneWinner()
    {
        Skip.IfNot(fixture.RedisAvailable, fixture.SkipReason);
        await using var redis = fixture.CreateStore();
        var repository = new ZLinkProviderLocationRepository(redis);
        var ownerA = await ClaimAsync(repository, "entry-owner-a");
        var ownerB = await ClaimAsync(repository, "entry-owner-b");
        var results = await Task.WhenAll(
            PublishAsync(repository, Descriptor(ownerA, 1, EntryA)).AsTask(),
            PublishAsync(new ZLinkProviderLocationRepository(redis), Descriptor(ownerB, 2, EntryA))
                .AsTask()
        );

        Assert.Contains(ZLinkLocationWriteStatus.Stored, results);
        Assert.Contains(ZLinkLocationWriteStatus.RejectedConflict, results);
    }

    private static async Task VerifyClaimLifetimeAsync(IZLinkLocationRepository repository)
    {
        var ownerA = await ClaimAsync(repository, "entry-owner-a");
        var ownerB = await ClaimAsync(repository, "entry-owner-b");
        var ownerC = await ClaimAsync(repository, "entry-owner-c");
        var a = Descriptor(ownerA, 1, EntryA);
        var b = Descriptor(ownerB, 2, EntryA);
        var c = Descriptor(ownerC, 3, EntryA);
        var keyA = new ZLinkMeshNodeDescriptorKey(a.MeshName, a.Rid);
        Assert.Equal(ZLinkLocationWriteStatus.Stored, await PublishAsync(repository, a));
        Assert.Equal(
            ZLinkLocationWriteStatus.IgnoredStale,
            (
                await repository.UpdateMeshNodeAsync(
                    a with
                    {
                        EntrySpotId = EntryB,
                        DescriptorRevision = 2,
                    },
                    ZLinkLocationWriteIntent.Renew
                )
            ).Status
        );
        Assert.Equal(
            ZLinkLocationWriteStatus.Stored,
            (
                await repository.UpdateMeshNodeAsync(
                    a with
                    {
                        DescriptorRevision = 2,
                    },
                    ZLinkLocationWriteIntent.Renew
                )
            ).Status
        );
        Assert.Equal(
            ZLinkLocationWriteStatus.IgnoredStale,
            await repository.RemoveMeshNodeAsync(keyA, ownerB)
        );
        Assert.Equal(ZLinkLocationWriteStatus.RejectedConflict, await PublishAsync(repository, b));
        Assert.Equal(
            ZLinkLocationWriteStatus.Stored,
            await repository.RemoveMeshNodeAsync(keyA, ownerA)
        );
        Assert.Equal(ZLinkLocationWriteStatus.Stored, await PublishAsync(repository, b));
        await repository.RemoveAllByOwnerAsync(ownerA);
        Assert.Equal(ZLinkLocationWriteStatus.RejectedConflict, await PublishAsync(repository, c));
        await repository.RemoveAllByOwnerAsync(ownerB);
        Assert.Contains(
            (await repository.ListMeshNodesAsync(b.MeshName, default)).Items,
            row => row.Rid == b.Rid
        );
        Assert.Equal(ZLinkLocationWriteStatus.Stored, await PublishAsync(repository, c));
    }

    private static async Task VerifySharedNamespaceAsync(
        IZLinkLocationRepository repository,
        ZLinkPlacementObjectKind kind
    )
    {
        var ownerA = await ClaimAsync(repository, "entry-owner-a");
        var ownerB = await ClaimAsync(repository, "entry-owner-b");
        var first = Descriptor(ownerA, 1, EntryA);
        var second = Descriptor(ownerB, 2, EntryB) with
        {
            ObjectCapabilities =
            [
                new ZLinkObjectCapability(
                    kind,
                    "game",
                    ZLinkObjectMaintenancePolicyKind.Disabled,
                    false,
                    10
                ),
            ],
            Capacity = new ZLinkPlacementCapacity(
                new ZLinkPopulationCapacity(0, 0, 0),
                new ZLinkPopulationCapacity(0, 0, 10),
                [new ZLinkSpotTypeCapacity(kind, "game", 0, 0, 10)]
            ),
        };
        Assert.Equal(ZLinkLocationWriteStatus.Stored, await PublishAsync(repository, first));
        Assert.Equal(ZLinkLocationWriteStatus.Stored, await PublishAsync(repository, second));
        var request = new ZLinkObjectReservationRequest(
            kind,
            ZLinkAuthorityKeyCodec.EncodeSpot(EntryA),
            "game",
            "inline:entry-spot-parity",
            new byte[32],
            0,
            new ZLinkMeshNodeDescriptorKey(second.MeshName, second.Rid),
            second.LifecycleGeneration,
            ownerB,
            ReadOnlyMemory<byte>.Empty,
            new ZLinkCapacityVector(0, 1, new ZLinkSpotTypeCapacityDelta(kind, "game", 1))
        );

        Assert.IsType<ZLinkObjectReserveResult.Conflict>(await repository.ReserveAsync(request));
        Assert.Equal(
            ZLinkLocationWriteStatus.Stored,
            await repository.RemoveMeshNodeAsync(new(first.MeshName, first.Rid), ownerA)
        );
        Assert.IsType<ZLinkObjectReserveResult.Reserved>(await repository.ReserveAsync(request));
        Assert.Equal(
            ZLinkLocationWriteStatus.RejectedConflict,
            await PublishAsync(repository, first)
        );
    }

    private static async Task DeleteClaimAsync(
        IZLinkLocationStore store,
        ZLinkStoreKey key,
        ZLinkStoreVersion version
    ) =>
        Assert.IsType<ZLinkStoreWriteResult.Applied>(
            await store.WriteAsync(
                new ZLinkStoreWriteRequest(
                    [new ZLinkStoreCondition.Version(key, version)],
                    [new ZLinkStoreMutation.Delete(key)]
                )
            )
        );

    private static async Task<ZLinkLocationWriteStatus> PublishSecondAsync(
        IZLinkLocationRepository repository,
        string secondEntryId
    )
    {
        var ownerA = await ClaimAsync(repository, "entry-owner-a");
        var ownerB = await ClaimAsync(repository, "entry-owner-b");
        Assert.Equal(
            ZLinkLocationWriteStatus.Stored,
            await PublishAsync(repository, Descriptor(ownerA, 1, EntryA))
        );
        return await PublishAsync(repository, Descriptor(ownerB, 2, secondEntryId));
    }

    private static async ValueTask<ZLinkLocationWriteStatus> PublishAsync(
        IZLinkLocationRepository repository,
        ZLinkMeshNodeDescriptor descriptor
    ) =>
        (
            await repository.UpdateMeshNodeAsync(descriptor, ZLinkLocationWriteIntent.NewClaim)
        ).Status;

    private static async ValueTask<ZLinkLocationOwnerToken> ClaimAsync(
        IZLinkLocationRepository repository,
        string ownerId
    ) =>
        Assert
            .IsType<ZLinkOwnerLeaseClaimResult.Claimed>(
                await repository.ClaimOwnerLeaseAsync(ownerId, TimeSpan.FromSeconds(30))
            )
            .Token;

    private static ZLinkMeshNodeDescriptor Descriptor(
        ZLinkLocationOwnerToken owner,
        byte node,
        string entryId
    ) =>
        new(
            "mesh",
            RoutingId.From(new byte[] { node }),
            node,
            1,
            $"tcp://127.0.0.1:{7000 + node}",
            new Dictionary<string, int>(StringComparer.Ordinal) { ["game"] = 100 },
            "security",
            owner.OwnerId,
            owner.LeaseGeneration,
            default
        )
        {
            ApplicationVersion = 1,
            ObjectRole = ZLinkMeshNodeObjectRole.Server,
            EntrySpotId = entryId,
            State = ZLinkFrameworkRuntimeState.Serving,
        };

    private sealed class PauseDescriptorDelete(IZLinkLocationStore inner, ZLinkStoreKey descriptor)
        : IZLinkLocationStore
    {
        public TaskCompletionSource Entered { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource Release { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public ValueTask<ZLinkStoreReadResult> ReadAsync(
            ZLinkStoreKey key,
            CancellationToken cancellationToken = default
        ) => inner.ReadAsync(key, cancellationToken);

        public async ValueTask<ZLinkStoreWriteResult> WriteAsync(
            ZLinkStoreWriteRequest request,
            CancellationToken cancellationToken = default
        )
        {
            if (
                request.Mutations.Any(mutation =>
                    mutation is ZLinkStoreMutation.Delete delete && delete.Key == descriptor
                )
            )
            {
                Entered.TrySetResult();
                await Release.Task.WaitAsync(cancellationToken);
            }
            return await inner.WriteAsync(request, cancellationToken);
        }

        public ValueTask<ZLinkStoreScanResult> ScanAsync(
            ZLinkStoreScanRequest request,
            CancellationToken cancellationToken = default
        ) => inner.ScanAsync(request, cancellationToken);
    }
}
