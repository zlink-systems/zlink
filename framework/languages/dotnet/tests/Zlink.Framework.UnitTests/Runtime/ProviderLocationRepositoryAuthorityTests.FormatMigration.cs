using System.Text;
using System.Text.Json.Nodes;
using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.AspNetCore;
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
        var clock = new ManualTimeProvider();
        var inner = new ZLinkInMemoryProviderLocationStore(state == "firstUse" ? null : clock);
        var prepared = state is "prepared" or "concurrentPrepared";
        var (abandoned, targetDescriptor) = await SeedAbandonedAggregateAsync(
            inner,
            prepared,
            rootOnly: state == "rootOnly"
        );
        var abandonedFence = new ZLinkAggregateFence(
            abandoned.AggregateId,
            abandoned.AggregateGeneration
        );
        if (state == "firstUse")
        {
            await BackdateAggregateRootAsync(inner, abandonedFence);
            var repository = new ZLinkProviderLocationRepository(inner);
            Assert.Equal(
                ZLinkOwnerLeaseReleaseResult.Released,
                await repository.ReleaseOwnerLeaseAsync(abandoned.TargetOwner)
            );
        }
        else
        {
            clock.Advance(TimeSpan.FromMinutes(2));
        }

        var recovering = new ZLinkProviderLocationRepository(inner);
        if (state != "firstUse")
        {
            await MarkAsLegacyStoreAsync(inner);
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
            if (state == "concurrentPrepared")
            {
                await using var runtime = new ZLinkLocationRuntime(new(), recovering);
                var inventoryKey = Assert.Single(
                    (await ScanInventoryKeysAsync(inner, abandonedFence)).Where(key =>
                        key.Contains(":page:", StringComparison.Ordinal)
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
                    var first = await Task.WhenAny(reachedInventory.Task, starting)
                        .WaitAsync(deadline.Token);
                    await first;
                    Assert.Same(reachedInventory.Task, first);
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
                await using var host = ConversionHost(inner);
                await FrameworkHostedService(host).StartAsync(CancellationToken.None);
            }
            var marker = Assert.IsType<ZLinkStoreReadResult.Found>(
                await inner.ReadAsync(new("zlink:v12:format"))
            );
            Assert.Equal("converted", Encoding.UTF8.GetString(marker.Value.Bytes.Span));
            Assert.Empty(await ScanKeysAsync(inner, AggregatePrefix(abandonedFence)));
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
        if (state == "firstUse")
        {
            var root = Assert.IsType<ZLinkStoreReadResult.Found>(
                await inner.ReadAsync(new(AggregatePrefix(abandonedFence)))
            );
            var aggregate = JsonNode.Parse(root.Value.Bytes.Span)!;
            Assert.Equal("aborted", aggregate["state"]!.GetValue<string>());
            var housekeeping = Assert.IsType<ZLinkStoreReadResult.Found>(
                await inner.ReadAsync(new(AggregatePrefix(abandonedFence) + ":housekeeping"))
            );
            Assert.True(
                Property(
                        JsonNode.Parse(housekeeping.Value.Bytes.Span)!.AsObject(),
                        "participantsCleaned"
                    )
                    .GetValue<bool>()
            );
        }

        Assert.DoesNotContain(
            await ScanKeysAsync(inner, AggregatePrefix(abandonedFence)),
            static key => key.Contains(":participant:", StringComparison.Ordinal)
        );
        Assert.Equal(
            ZLinkAggregateCommitResult.Stale,
            await recovering.CommitAggregateAsync(abandonedFence)
        );
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task LiveV11AggregateOwnerStopsStartupWithoutChangingStore(bool terminal)
    {
        var inner = new ZLinkInMemoryProviderLocationStore(new ManualTimeProvider());
        var (abandoned, _) = await SeedAbandonedAggregateAsync(
            inner,
            prepared: false,
            rootOnly: true
        );
        var liveFence = new ZLinkAggregateFence(
            abandoned.AggregateId,
            abandoned.AggregateGeneration
        );
        var rootKey = new ZLinkStoreKey(AggregatePrefix(liveFence));
        var root = Assert.IsType<ZLinkStoreReadResult.Found>(await inner.ReadAsync(rootKey));
        var live = ZLinkAggregateInventory.Decode<ZLinkAggregateInventory.Aggregate>(
            root.Value.Bytes
        );
        var earlierFence = new ZLinkAggregateFence(
            Guid.Parse("00000000-0000-4000-8000-000000000000"),
            abandoned.AggregateGeneration
        );
        var earlier = live with
        {
            AggregateId = earlierFence.AggregateId.ToString("D"),
            TargetOwner = live.TargetOwner with { OwnerId = "expired-owner" },
        };
        await SeedCounterAsync(
            inner,
            new(AggregatePrefix(earlierFence)),
            Encoding.UTF8.GetString(ZLinkAggregateInventory.Encode(earlier))
        );
        if (terminal)
        {
            live = live with { State = "aborted" };
            await SeedCounterAsync(
                inner,
                rootKey,
                Encoding.UTF8.GetString(ZLinkAggregateInventory.Encode(live))
            );
        }
        await MarkAsLegacyStoreAsync(inner);
        var before = await SnapshotAsync(inner);

        await using (var host = ConversionHost(inner))
            await Assert.ThrowsAsync<ZLinkConfigurationException>(() =>
                FrameworkHostedService(host).StartAsync(CancellationToken.None)
            );

        var after = await SnapshotAsync(inner);
        Assert.Equal(before.Select(pair => pair.Key), after.Select(pair => pair.Key));
        foreach (var (oldRow, newRow) in before.Zip(after))
        {
            Assert.Equal(oldRow.Value.Version, newRow.Value.Version);
            Assert.Equal(oldRow.Value.ExpiresAt, newRow.Value.ExpiresAt);
            Assert.Equal(oldRow.Value.Bytes.ToArray(), newRow.Value.Bytes.ToArray());
        }
    }

    private static async Task<
        IReadOnlyList<KeyValuePair<ZLinkStoreKey, ZLinkStoreValue>>
    > SnapshotAsync(IZLinkLocationStore store)
    {
        var page = Assert
            .IsType<ZLinkStoreScanResult.Page>(await store.ScanAsync(new("", null, 1000)))
            .Value;
        Assert.Null(page.NextCursor);
        return page.Items.OrderBy(pair => pair.Key.Value, StringComparer.Ordinal).ToArray();
    }

    private static ServiceProvider ConversionHost(IZLinkLocationStore store)
    {
        var services = new ServiceCollection();
        services.AddZLinkFramework(options =>
        {
            options.DisableImplicitHandlerAutoRegistration();
            options.AddLocationStore(store);
            options.AddRouteMesh("format-migration").Listen("tcp://127.0.0.1:0").Objects().Server();
        });
        return services.BuildServiceProvider();
    }

    private static async Task MarkAsLegacyStoreAsync(IZLinkLocationStore store) =>
        Assert.IsType<ZLinkStoreWriteResult.Applied>(
            await store.WriteAsync(
                new(
                    [],
                    [
                        new ZLinkStoreMutation.Delete(new("zlink:v12:format")),
                        new ZLinkStoreMutation.Delete(new("zlink:v12:generation-floor")),
                    ]
                )
            )
        );

    private static async Task<(
        ZLinkAggregatePrepareRequest Request,
        ZLinkMeshNodeDescriptor Target
    )> SeedAbandonedAggregateAsync(IZLinkLocationStore inner, bool prepared, bool rootOnly)
    {
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
        var abandoned = AggregateRequest([participant], targetDescriptor, targetOwner) with
        {
            AggregateId = Guid.Parse("00000000-0000-4000-8000-000000000001"),
        };
        if (prepared)
        {
            Assert.IsType<ZLinkAggregatePrepareResult.Prepared>(
                await repository.PrepareAggregateAsync(abandoned)
            );
        }
        else
        {
            provider.ThrowAfterNextWrite = true;
            provider.SkipWritesBeforeThrow = rootOnly ? 0 : 1;
            provider.BlockWritesAfterThrownResponse = true;
            var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
                await repository.PrepareAggregateAsync(abandoned)
            );
            Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, failure.Kind);
            Assert.IsType<IOException>(failure.InnerException);
            provider.BlockWrites = false;
        }
        return (abandoned, targetDescriptor);
    }
}
