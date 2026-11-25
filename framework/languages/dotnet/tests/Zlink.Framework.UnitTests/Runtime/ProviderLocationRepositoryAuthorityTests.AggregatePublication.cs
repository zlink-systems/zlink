using System.Text;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests;

public sealed partial class ProviderLocationRepositoryAuthorityTests
{
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
