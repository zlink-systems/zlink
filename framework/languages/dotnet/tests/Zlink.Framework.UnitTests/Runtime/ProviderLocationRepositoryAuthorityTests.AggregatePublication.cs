using System.Text;
using System.Text.Json;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests;

public sealed partial class ProviderLocationRepositoryAuthorityTests
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task PreparedRelocationPreserveAndTargetCommitCompete(bool sourceWins)
    {
        var provider = new InspectAuthorityBatchLocationStore(
            new ZLinkInMemoryProviderLocationStore()
        );
        var repository = new ZLinkProviderLocationRepository(provider);
        var sourceOwner = await ClaimAsync(repository, "source");
        var targetOwner = await ClaimAsync(repository, "target");
        var source = Descriptor("source", sourceOwner);
        var target = Descriptor("target", targetOwner);
        await PublishAsync(repository, source, ZLinkLocationWriteIntent.NewClaim);
        await PublishAsync(repository, target, ZLinkLocationWriteIntent.NewClaim);

        var reservation = Reservation("aggregate-preserve", source, sourceOwner);
        var reserved = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await repository.ReserveAsync(reservation)
        );
        byte[] payload = [1];
        var ready = Assert
            .IsType<ZLinkObjectCommitResult.Committed>(
                await repository.CommitAsync(reserved.Reservation, payload)
            )
            .Snapshot;
        var coordinator = new ZLinkAuthorityRelocation.CoordinatorFence(
            source.Rid.ToHex(),
            source.LifecycleGeneration,
            sourceOwner.OwnerId,
            (ulong)sourceOwner.LeaseGeneration,
            ready.StoreVersion
        );
        var participant = new ZLinkAggregateParticipant(
            reservation.Key,
            ready.StoreVersion,
            ZLinkAuthorityGenerationTransition.NewOwner,
            new byte[] { 2 },
            ReadOnlyMemory<byte>.Empty
        );
        var aggregate = AggregateRequest([participant], target, targetOwner);
        var preparedRelocation = new ZLinkAuthorityRelocation(
            aggregate.AggregateId,
            ServiceWireCodec.RelocationPhase.Prepared,
            coordinator.Identity,
            new(
                target.Rid.ToHex(),
                target.LifecycleGeneration,
                targetOwner.OwnerId,
                (ulong)targetOwner.LeaseGeneration,
                1
            ),
            coordinator,
            Convert.ToHexString(aggregate.InventoryDigest.Span).ToLowerInvariant(),
            1
        );
        var capture = new ZLinkAuthorityMutation.Put(
            payload,
            ZLinkAuthorityGenerationTransition.Preserve,
            null,
            null
        )
        {
            Relocation = preparedRelocation,
        };
        var captured = Assert
            .IsType<ZLinkAuthorityCompareExchangeResult.Stored>(
                await repository.CompareExchangeAuthorityAsync(
                    reservation.Key,
                    ready.StoreVersion,
                    capture
                )
            )
            .Snapshot;
        aggregate = aggregate with
        {
            Participants = [participant with { ExpectedStoreVersion = captured.StoreVersion }],
            Relocation = preparedRelocation with
            {
                Phase = ServiceWireCodec.RelocationPhase.Committed,
            },
        };
        var prepared = Assert.IsType<ZLinkAggregatePrepareResult.Prepared>(
            await repository.PrepareAggregateAsync(aggregate)
        );
        var root = new ZLinkStoreKey(AggregatePrefix(prepared.Fence));
        var pending = CapacityKey(target, pending: true);
        Assert.Equal("1", await ReadCounterAsync(provider, pending));

        var sourceBefore = Assert
            .IsType<ZLinkAuthorityReadResult.Found>(
                await repository.ReadAuthorityAsync(reservation.Key)
            )
            .Snapshot;
        var preserve = capture with
        {
            Relocation = preparedRelocation with
            {
                Phase = ServiceWireCodec.RelocationPhase.Aborted,
            },
        };
        var rejected = Assert.IsType<ZLinkAuthorityCompareExchangeResult.Conflict>(
            await repository.CompareExchangeAuthorityAsync(
                reservation.Key,
                sourceBefore.StoreVersion,
                preserve with
                {
                    Payload = new byte[] { 3 },
                }
            )
        );
        Assert.Equal("1", await ReadCounterAsync(provider, pending));

        var targetWriting = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var releaseTarget = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        provider.Inspect = async write =>
        {
            foreach (var put in write.Mutations.OfType<ZLinkStoreMutation.Put>())
            {
                if (put.Key != root)
                    continue;
                using var record = JsonDocument.Parse(put.Bytes);
                if (record.RootElement.GetProperty("status").GetInt32() == 2)
                {
                    targetWriting.TrySetResult();
                    await releaseTarget.Task;
                }
            }
        };
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        var targetCommit = repository.CommitAggregateAsync(prepared.Fence, timeout.Token).AsTask();
        try
        {
            await targetWriting.Task.WaitAsync(timeout.Token);
            if (!sourceWins)
            {
                releaseTarget.SetResult();
                Assert.Equal(ZLinkAggregateCommitResult.Committed, await targetCommit);
                Assert.IsType<ZLinkAuthorityCompareExchangeResult.Conflict>(
                    await repository.CompareExchangeAuthorityAsync(
                        reservation.Key,
                        sourceBefore.StoreVersion,
                        preserve
                    )
                );
                var targetAfter = Assert
                    .IsType<ZLinkAuthorityReadResult.Found>(
                        await repository.ReadAuthorityAsync(reservation.Key)
                    )
                    .Snapshot;
                Assert.Equal(targetOwner.OwnerId, targetAfter.OwnerId);
                Assert.Equal("0", await ReadCounterAsync(provider, pending));
                return;
            }

            var leaseChecks = 0;
            var result = await ZLinkStandaloneActorRelocationRuntime.SettleSourceAsync(
                repository,
                reservation.Key,
                sourceBefore,
                prepared.Fence.AggregateId,
                target,
                restoreDeadline: TimeSpan.Zero,
                isSourceLeaseValid: () => ++leaseChecks == 1,
                submitBoundary: _ =>
                    throw new InvalidOperationException("Restore deadline has elapsed."),
                pollingInterval: TimeSpan.Zero,
                cancellationToken: timeout.Token,
                observeRoute: () =>
                    throw new InvalidOperationException("Restore deadline has elapsed.")
            );
            Assert.Equal(ZLinkSourceSettlement.SourcePreserved, result.Outcome);
            var preserved = Assert.IsType<ZLinkAuthoritySnapshot>(result.Snapshot);
            Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, rejected.MembershipFailure);
            Assert.NotEqual(sourceBefore.StoreVersion, preserved.StoreVersion);
            Assert.Equal(sourceBefore.OwnerId, preserved.OwnerId);
            Assert.Equal(sourceBefore.OwnerLeaseGeneration, preserved.OwnerLeaseGeneration);
            Assert.Equal(sourceBefore.ObjectGeneration, preserved.ObjectGeneration);
            Assert.Equal(sourceBefore.AuthorityOwnerGeneration, preserved.AuthorityOwnerGeneration);
            Assert.Equal(sourceBefore.Allocation, preserved.Allocation);
            Assert.Equal(payload, preserved.Payload.ToArray());
            Assert.Equal(preserve.Relocation, preserved.Relocation);
            var aborted = Assert.IsType<ZLinkStoreReadResult.Found>(await provider.ReadAsync(root));
            using var marker = JsonDocument.Parse(aborted.Value.Bytes);
            Assert.Equal(3, marker.RootElement.GetProperty("status").GetInt32());
            Assert.Equal("0", await ReadCounterAsync(provider, pending));

            releaseTarget.SetResult();
            Assert.Equal(ZLinkAggregateCommitResult.Stale, await targetCommit);
            Assert.Equal(
                ZLinkAggregateAbortResult.AlreadyAborted,
                await repository.AbortAggregateAsync(prepared.Fence)
            );
            Assert.Equal("0", await ReadCounterAsync(provider, pending));
        }
        finally
        {
            releaseTarget.TrySetResult();
            await targetCommit;
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
