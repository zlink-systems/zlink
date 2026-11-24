using System.Text;
using System.Text.Json.Nodes;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests;

public sealed partial class ProviderLocationRepositoryAuthorityTests
{
    [Theory]
    [InlineData("firstUse")]
    [InlineData("staging")]
    [InlineData("rootOnly")]
    [InlineData("prepared")]
    [InlineData("concurrentPrepared")]
    public async Task ReclaimsAbandonedAggregate(string state)
    {
        var inner = new ZLinkInMemoryProviderLocationStore();
        var provider = new ApplyThenThrowLocationStore(inner);
        var repository = new ZLinkProviderLocationRepository(provider);
        var sourceOwner = await ClaimAsync(repository, "source-owner");
        var targetOwner = await ClaimAsync(repository, "target-owner");
        var sourceDescriptor = Descriptor("source", sourceOwner);
        var targetDescriptor = Descriptor("target", targetOwner);
        _ = await repository.UpdateMeshNodeAsync(
            sourceDescriptor,
            ZLinkLocationWriteIntent.NewClaim
        );
        _ = await repository.UpdateMeshNodeAsync(
            targetDescriptor,
            ZLinkLocationWriteIntent.NewClaim
        );
        var create = Reservation("actor:staging-recovery", sourceDescriptor, sourceOwner);
        var reserved = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await repository.ReserveAsync(create)
        );
        var created = Assert
            .IsType<ZLinkObjectCommitResult.Committed>(
                await repository.CommitAsync(reserved.Reservation, new byte[] { 0x91 })
            )
            .Snapshot;
        var participant = new ZLinkAggregateParticipant(
            create.Key,
            created.StoreVersion,
            ZLinkAuthorityGenerationTransition.NewOwner,
            new byte[] { 0x92 },
            new byte[] { 0x93 }
        );
        var abandoned = AggregateRequest([participant], targetDescriptor, targetOwner);
        var abandonedFence = new ZLinkAggregateFence(
            abandoned.AggregateId,
            abandoned.AggregateGeneration
        );

        var prepared = state is "prepared" or "concurrentPrepared";
        if (prepared)
        {
            Assert.IsType<ZLinkAggregatePrepareResult.Prepared>(
                await repository.PrepareAggregateAsync(abandoned)
            );
        }
        else
        {
            provider.ThrowAfterNextWrite = true;
            provider.SkipWritesBeforeThrow = state == "rootOnly" ? 0 : 1;
            provider.BlockWritesAfterThrownResponse = true;
            var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
                await repository.PrepareAggregateAsync(abandoned)
            );
            Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, failure.Kind);
            Assert.IsType<IOException>(failure.InnerException);
            provider.BlockWrites = false;
        }
        await BackdateAggregateRootAsync(inner, abandonedFence);
        Assert.Equal(
            ZLinkOwnerLeaseReleaseResult.Released,
            await repository.ReleaseOwnerLeaseAsync(targetOwner)
        );

        var recovering = new ZLinkProviderLocationRepository(inner);
        if (state != "firstUse")
        {
            Assert.IsType<ZLinkStoreWriteResult.Applied>(
                await inner.WriteAsync(
                    new ZLinkStoreWriteRequest(
                        [],
                        [
                            new ZLinkStoreMutation.Delete(new("zlink:v12:format")),
                            new ZLinkStoreMutation.Delete(new("zlink:v12:generation-floor")),
                        ]
                    )
                )
            );
            var legacyCapacity = new ZLinkStoreKey("zlink:v11:capacity:play:target");
            if (prepared)
            {
                const string pending = """
                    {"actorsActive":2,"actorsPending":1,"spotsActive":0,"spotsPending":0,"spotTypes":{}}
                    """;
                await SeedCounterAsync(inner, legacyCapacity, pending);
                await inner.WriteAsync(
                    new(
                        [],
                        [
                            new ZLinkStoreMutation.Delete(
                                CapacityKey(targetDescriptor, pending: true)
                            ),
                        ]
                    )
                );
            }
            await using var runtime = new ZLinkLocationRuntime(new(), recovering);
            if (state == "concurrentPrepared")
            {
                var inventoryKey = Assert.Single(
                    (await ScanKeysAsync(inner, AggregatePrefix(abandonedFence))).Where(key =>
                        key.Contains(":inventory:", StringComparison.Ordinal)
                    )
                );
                var reachedInventory = new TaskCompletionSource(
                    TaskCreationOptions.RunContinuationsAsynchronously
                );
                var resumeInventory = new TaskCompletionSource(
                    TaskCreationOptions.RunContinuationsAsynchronously
                );
                var concurrentStore = new InspectAuthorityBatchLocationStore(inner)
                {
                    BeforeRead = async key =>
                    {
                        if (key.Value == inventoryKey)
                        {
                            reachedInventory.TrySetResult();
                            await resumeInventory.Task;
                        }
                    },
                };
                using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(15));
                await using var concurrent = new ZLinkLocationRuntime(
                    new(),
                    new ZLinkProviderLocationRepository(concurrentStore)
                );
                var starting = concurrent
                    .StartAsync(RoutingId.From("v12-concurrent"), deadline.Token)
                    .AsTask();
                try
                {
                    await reachedInventory.Task.WaitAsync(deadline.Token);
                    await runtime.StartAsync(RoutingId.From("v12-host"), deadline.Token);
                }
                finally
                {
                    resumeInventory.TrySetResult();
                }
                await starting;
            }
            else
            {
                await runtime.StartAsync(RoutingId.From("v12-host"));
            }
            var marker = Assert.IsType<ZLinkStoreReadResult.Found>(
                await inner.ReadAsync(new("zlink:v12:format"))
            );
            Assert.Equal("converted", Encoding.UTF8.GetString(marker.Value.Bytes.Span));
            if (prepared)
            {
                Assert.IsType<ZLinkStoreReadResult.Missing>(await inner.ReadAsync(legacyCapacity));
                Assert.IsType<ZLinkStoreReadResult.Missing>(
                    await inner.ReadAsync(CapacityKey(targetDescriptor, pending: true))
                );
                Assert.Equal("2", await ReadCounterAsync(inner, CapacityKey(targetDescriptor)));
            }
        }
        else
        {
            var next = abandoned with { AggregateId = Guid.NewGuid() };
            Assert.IsType<ZLinkAggregatePrepareResult.Conflict>(
                await recovering.PrepareAggregateAsync(next)
            );
        }
        var root = Assert.IsType<ZLinkStoreReadResult.Found>(
            await inner.ReadAsync(new(AggregatePrefix(abandonedFence)))
        );
        var aggregate = JsonNode.Parse(root.Value.Bytes.Span)!;
        Assert.Equal(3, aggregate["status"]!.GetValue<int>());
        Assert.True(aggregate["participantsCleaned"]!.GetValue<bool>());

        Assert.DoesNotContain(
            await ScanKeysAsync(inner, AggregatePrefix(abandonedFence)),
            static key => key.Contains(":participant:", StringComparison.Ordinal)
        );
        Assert.Equal(
            ZLinkAggregateCommitResult.Stale,
            await recovering.CommitAggregateAsync(abandonedFence)
        );
    }
}
