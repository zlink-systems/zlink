using System.Text;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests;

public sealed partial class ProviderLocationRepositoryAuthorityTests
{
    [Theory]
    [InlineData(0)]
    [InlineData(1)]
    [InlineData(3)]
    [InlineData(2048)]
    [InlineData(1600)]
    public async Task AggregateCommitPublishesAcceptedReplyRowsInTheCommitBatch(int requestCount)
    {
        var provider = new InspectAuthorityBatchLocationStore(
            new ZLinkInMemoryProviderLocationStore()
        );
        var repository = new ZLinkProviderLocationRepository(provider);
        var sourceOwner = await ClaimAsync(repository, "reply-row-source");
        var targetOwner = await ClaimAsync(repository, "reply-row-target");
        var source = Descriptor("source", sourceOwner);
        var target = Descriptor("target", targetOwner);
        await PublishAsync(repository, source, ZLinkLocationWriteIntent.NewClaim);
        await PublishAsync(repository, target, ZLinkLocationWriteIntent.NewClaim);
        var reservation = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await repository.ReserveAsync(Reservation("reply-row-actor", source, sourceOwner))
        );
        var ready = Assert
            .IsType<ZLinkObjectCommitResult.Committed>(
                await repository.CommitAsync(reservation.Reservation, new byte[] { 1 })
            )
            .Snapshot;
        var participant = new ZLinkAggregateParticipant(
            reservation.Reservation.Key,
            ready.StoreVersion,
            ZLinkAuthorityGenerationTransition.NewOwner,
            new byte[] { 2 },
            ReadOnlyMemory<byte>.Empty
        );
        var aggregate = AggregateRequest([participant], target, targetOwner);
        aggregate = aggregate with
        {
            Relocation = new(
                aggregate.AggregateId,
                ServiceWireCodec.RelocationPhase.Committed,
                new(
                    source.Rid.ToHex(),
                    source.LifecycleGeneration,
                    sourceOwner.OwnerId,
                    (ulong)sourceOwner.LeaseGeneration
                ),
                new(
                    target.Rid.ToHex(),
                    target.LifecycleGeneration,
                    targetOwner.OwnerId,
                    (ulong)targetOwner.LeaseGeneration,
                    1
                ),
                new(
                    source.Rid.ToHex(),
                    source.LifecycleGeneration,
                    sourceOwner.OwnerId,
                    (ulong)sourceOwner.LeaseGeneration,
                    ready.StoreVersion
                ),
                Convert.ToHexString(aggregate.InventoryDigest.Span).ToLowerInvariant(),
                1
            ),
        };
        var prepared = Assert.IsType<ZLinkAggregatePrepareResult.Prepared>(
            await repository.PrepareAggregateAsync(aggregate)
        );
        var root = AggregatePrefix(prepared.Fence);
        ZLinkStoreWriteRequest? commit = null;
        provider.Inspect = batch =>
        {
            if (
                batch.Mutations.OfType<ZLinkStoreMutation.Put>().Any(put => put.Key.Value == root)
                && batch
                    .Conditions.OfType<ZLinkStoreCondition.Version>()
                    .Any(c => c.Key.Value == root)
            )
                commit ??= batch;
            return Task.CompletedTask;
        };

        var sourceFence =
            requestCount == 1600
                ? new ZLinkCanonicalRequestSourceFence(
                    new string('x', 320),
                    1,
                    new string('a', 510),
                    1
                )
                : new(sourceOwner.OwnerId, 1, "01", 1);
        var replies = Enumerable
            .Range(1, requestCount)
            .Select(index => new ZLinkRelocationCompletionEntry(
                participant.Key.Value,
                $"{index:x32}",
                1,
                "",
                sourceFence,
                ZLinkRelocationCompletionDelivery.Accepted
            ))
            .ToArray();
        if (requestCount >= 1600)
        {
            await Assert.ThrowsAsync<ZLinkRelocationReplyBatchLimitException>(() =>
                repository.CommitAggregateAsync(prepared.Fence, replies, default).AsTask()
            );
            Assert.Empty(
                await repository.ReadRelocationRepliesAsync(aggregate.AggregateId, default)
            );
            var unchanged = Assert.IsType<ZLinkAuthorityReadResult.Found>(
                await repository.ReadAuthorityAsync(participant.Key)
            );
            Assert.Equal(sourceOwner.OwnerId, unchanged.Snapshot.OwnerId);
            Assert.Equal(
                ZLinkAggregateAbortResult.Aborted,
                await repository.AbortAggregateAsync(prepared.Fence)
            );
            return;
        }

        Assert.Equal(
            ZLinkAggregateCommitResult.Committed,
            await repository.CommitAggregateAsync(prepared.Fence, replies, default)
        );
        Assert.NotNull(commit);
        var rows = await repository.ReadRelocationRepliesAsync(aggregate.AggregateId, default);
        Assert.Equal(requestCount, rows.Count);
        foreach (var row in rows)
        {
            Assert.Equal(targetOwner, row.Writer);
            Assert.Equal(ZLinkRelocationCompletionDelivery.Accepted, row.Entry.Delivery);
            Assert.Contains(
                commit.Conditions,
                condition =>
                    condition is ZLinkStoreCondition.Missing missing && missing.Key == row.Key
            );
            Assert.Contains(
                commit.Mutations,
                mutation => mutation is ZLinkStoreMutation.Put put && put.Key == row.Key
            );
        }
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
                        && Encoding
                            .UTF8.GetString(put.Bytes.Span)
                            .Contains("\"status\":1", StringComparison.Ordinal)
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
