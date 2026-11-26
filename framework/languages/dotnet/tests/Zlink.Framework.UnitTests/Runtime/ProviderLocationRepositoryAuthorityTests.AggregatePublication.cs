using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests;

public sealed partial class ProviderLocationRepositoryAuthorityTests
{
    [Theory]
    [InlineData(false, "descriptor removed")]
    [InlineData(false, "lease expired")]
    [InlineData(false, "identity mismatch")]
    [InlineData(true, "descriptor removed")]
    [InlineData(true, "lease expired")]
    [InlineData(true, "identity mismatch")]
    public async Task AggregateCommitFencesAuthorityIdentity(bool inMemory, string change)
    {
        var time = new ManualTimeProvider();
        IZLinkLocationRepository repository = inMemory
            ? new ZLinkInMemoryLocationStore(time)
            : new ZLinkProviderLocationRepository(new ZLinkInMemoryProviderLocationStore(time));
        var sourceOwner = await ClaimAsync(repository, "aggregate-source");
        var targetOwner = await ClaimAsync(repository, "aggregate-target", TimeSpan.FromMinutes(1));
        var source = Descriptor("source", sourceOwner);
        var target = Descriptor("target", targetOwner);
        await PublishAsync(repository, source, ZLinkLocationWriteIntent.NewClaim);
        await PublishAsync(repository, target, ZLinkLocationWriteIntent.NewClaim);
        var request = Reservation("aggregate-fence", source, sourceOwner);
        var reserved = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await repository.ReserveAsync(request)
        );
        var ready = Assert
            .IsType<ZLinkObjectCommitResult.Committed>(
                await repository.CommitAsync(reserved.Reservation, new byte[] { 1 })
            )
            .Snapshot;
        var participant = new ZLinkAggregateParticipant(
            request.Key,
            ready.StoreVersion,
            ZLinkAuthorityGenerationTransition.NewOwner,
            new byte[] { 2 },
            ReadOnlyMemory<byte>.Empty
        );
        var aggregate = AggregateRequest([participant], target, targetOwner);
        var prepared = Assert.IsType<ZLinkAggregatePrepareResult.Prepared>(
            await repository.PrepareAggregateAsync(aggregate)
        );
        var fence = prepared.Fence;

        if (change == "descriptor removed")
        {
            Assert.Equal(
                ZLinkLocationWriteStatus.Stored,
                await repository.RemoveMeshNodeAsync(aggregate.TargetDescriptor, targetOwner)
            );
            var lease = Assert.IsType<ZLinkOwnerLeaseReadResult.Found>(
                await repository.ReadOwnerLeaseAsync(targetOwner.OwnerId)
            );
            Assert.True(lease.LeaseExpiresAt > lease.StoreNow);
        }
        else if (change == "lease expired")
        {
            time.Advance(TimeSpan.FromSeconds(61));
            var lease = await repository.ReadOwnerLeaseAsync(targetOwner.OwnerId);
            Assert.True(
                lease is ZLinkOwnerLeaseReadResult.Missing
                    || lease is ZLinkOwnerLeaseReadResult.Found found
                        && found.LeaseExpiresAt <= found.StoreNow
            );
            var nodes = await repository.ListMeshNodesAsync(target.MeshName, new(100));
            Assert.Contains(nodes.Items, node => node.Rid == target.Rid);
        }
        else
        {
            Assert.Equal(
                ZLinkAggregateCommitResult.Stale,
                await repository.CommitAggregateAsync(fence with { AggregateId = Guid.NewGuid() })
            );
            Assert.Equal(
                ZLinkAggregateCommitResult.Stale,
                await repository.CommitAggregateAsync(
                    fence with
                    {
                        AggregateGeneration = fence.AggregateGeneration + 1,
                    }
                )
            );
            var unchanged = Assert
                .IsType<ZLinkAuthorityReadResult.Found>(
                    await repository.ReadAuthorityAsync(request.Key)
                )
                .Snapshot;
            Assert.Equal(sourceOwner.OwnerId, unchanged.OwnerId);
            Assert.Equal(ready.AuthorityOwnerGeneration, unchanged.AuthorityOwnerGeneration);
        }

        Assert.Equal(
            ZLinkAggregateCommitResult.Committed,
            await repository.CommitAggregateAsync(fence)
        );
        var committed = Assert
            .IsType<ZLinkAuthorityReadResult.Found>(
                await repository.ReadAuthorityAsync(request.Key)
            )
            .Snapshot;
        Assert.Equal(targetOwner.OwnerId, committed.OwnerId);
        Assert.Equal(targetOwner.LeaseGeneration, committed.OwnerLeaseGeneration);
        Assert.True(committed.AuthorityOwnerGeneration > ready.AuthorityOwnerGeneration);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task AggregatePreparedBatchRevalidatesChangedTargetDescriptor(bool newOwner)
    {
        var provider = new InspectAuthorityBatchLocationStore(
            new ZLinkInMemoryProviderLocationStore()
        );
        var repository = new ZLinkProviderLocationRepository(provider);
        var sourceOwner = await ClaimAsync(repository, "aggregate-source");
        var targetOwner = newOwner ? await ClaimAsync(repository, "aggregate-target") : sourceOwner;
        var source = Descriptor("source", sourceOwner);
        var target = newOwner ? Descriptor("target", targetOwner) : source;
        await PublishAsync(repository, source, ZLinkLocationWriteIntent.NewClaim);
        if (newOwner)
            await PublishAsync(repository, target, ZLinkLocationWriteIntent.NewClaim);
        var request = Reservation("aggregate-descriptor-change", source, sourceOwner);
        var reserved = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await repository.ReserveAsync(request)
        );
        var ready = Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await repository.CommitAsync(reserved.Reservation, new byte[] { 1 })
        );
        var participant = new ZLinkAggregateParticipant(
            request.Key,
            ready.Snapshot.StoreVersion,
            newOwner
                ? ZLinkAuthorityGenerationTransition.NewOwner
                : ZLinkAuthorityGenerationTransition.Preserve,
            new byte[] { 2 },
            ReadOnlyMemory<byte>.Empty
        );
        var aggregate = AggregateRequest([participant], target, targetOwner) with
        {
            Capacity = new ZLinkCapacityVector(newOwner ? 1 : 0, 0, null),
        };
        var root = AggregatePrefix(
            new ZLinkAggregateFence(aggregate.AggregateId, aggregate.AggregateGeneration)
        );
        var attempts = 0;
        provider.Inspect = async batch =>
        {
            if (
                !batch
                    .Mutations.OfType<ZLinkStoreMutation.Put>()
                    .Any(put =>
                        put.Key.Value == root
                        && ZLinkAggregateInventory
                            .Decode<ZLinkAggregateInventory.Aggregate>(put.Bytes)
                            .State == "prepared"
                    )
            )
                return;
            if (++attempts == 1)
                await PublishAsync(
                    repository,
                    target with
                    {
                        DescriptorRevision = target.DescriptorRevision + 1,
                    },
                    ZLinkLocationWriteIntent.Renew
                );
        };

        var prepared = Assert.IsType<ZLinkAggregatePrepareResult.Prepared>(
            await repository.PrepareAggregateAsync(aggregate)
        );
        Assert.Equal(2, attempts);
        Assert.Equal(newOwner ? 2UL : 1UL, prepared.TargetAuthorityOwnerGenerations[request.Key]);
        Assert.Equal(
            newOwner ? "3" : "2",
            await ReadCounterAsync(provider, AuthorityOwnerCounterKey(request.Key))
        );
        Assert.Equal(
            ZLinkAggregateCommitResult.Committed,
            await repository.CommitAggregateAsync(prepared.Fence)
        );
    }
}
