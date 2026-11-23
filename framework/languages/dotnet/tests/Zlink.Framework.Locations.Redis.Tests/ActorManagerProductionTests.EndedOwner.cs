using System.Security.Cryptography;
using System.Text;
using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Errors;
using Zlink.Framework.Contracts.Locations;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.Locations.Redis.Tests;

public sealed partial class ActorManagerProductionTests
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task SplitObjectClientReclaimsEndedOwnerAndRunsTargetFactoryOnce(bool userSpot)
    {
        TestActorFactory.Reset();
        ReclaimProbeSpot.CreateCount = 0;
        var time = new MonotonicTimeProvider();
        var store = new ZLinkInMemoryProviderLocationStore();
        var repository = new ZLinkProviderLocationRepository(store);
        const string endpoint = "tcp://127.0.0.1:0";
        await using var server = BuildServer(store, endpoint, includeUserSpot: userSpot);
        await using var client = BuildClient(store, endpoint, time);
        var target = server.GetRequiredService<ZLinkFrameworkRuntime>();
        var source = client.GetRequiredService<ZLinkFrameworkRuntime>();
        await FrameworkHost(server).StartAsync(CancellationToken.None);
        await FrameworkHost(client).StartAsync(CancellationToken.None);
        try
        {
            var targetNode = target.GetSpotNodeRuntime("objects").Node;
            var targetRid = targetNode.RoutingId;
            await PublishServerDescriptorAsync(
                repository,
                target,
                targetRid,
                Assert.IsType<string>(targetNode.MeshStatus().LocalEndpoint)
            );
            await WaitUntilAsync(() =>
                source.GetSpotNodeRuntime("objects").Node.MeshStatus().AdmittedPeerCount == 1
            );
            var requester = source.Registration.SpotNodes["objects"];
            Assert.Empty(requester.ActorFactories);
            Assert.Empty(requester.SpotFactories);

            var nodes = await repository.ListMeshNodesAsync("objects", default);
            var descriptor = Assert.Single(nodes.Items, item => item.Rid == targetRid);
            var owner = Assert
                .IsType<ZLinkOwnerLeaseClaimResult.Claimed>(
                    await repository.ClaimOwnerLeaseAsync("ended-owner", TimeSpan.FromMinutes(1))
                )
                .Token;
            var old = descriptor with
            {
                Rid = RoutingId.From("ended-owner"),
                EntrySpotId = ZLinkSpotId.CreateEntrySpotId("ended-owner"),
                Endpoint = "tcp://127.0.0.1:1",
                OwnerId = owner.OwnerId,
                LeaseGeneration = owner.LeaseGeneration,
                DescriptorRevision = 1,
            };
            Assert.Equal(
                ZLinkLocationWriteStatus.Stored,
                (
                    await repository.UpdateMeshNodeAsync(old, ZLinkLocationWriteIntent.NewClaim)
                ).Status
            );

            var id = $"ended-{Guid.NewGuid():N}";
            var (kind, type, key, payload, capacity) = userSpot
                ? (
                    ZLinkPlacementObjectKind.UserSpot,
                    "room",
                    ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(id),
                    ZLinkUserSpotAuthorityPayloadCodec.Encode(
                        new(
                            ZLinkUserSpotAuthorityState.Ready,
                            "room",
                            id,
                            owner.OwnerId,
                            checked((ulong)owner.LeaseGeneration),
                            old.MeshName,
                            old.Rid,
                            old.LifecycleGeneration
                        )
                    ),
                    new ZLinkCapacityVector(
                        0,
                        1,
                        new ZLinkSpotTypeCapacityDelta(ZLinkPlacementObjectKind.UserSpot, "room", 1)
                    )
                )
                : (
                    ZLinkPlacementObjectKind.Actor,
                    "player",
                    ZLinkActorAuthorityPayloadCodec.AuthorityKey(id),
                    ZLinkActorAuthorityPayloadCodec.Encode(
                        new(
                            ZLinkActorAuthorityState.Ready,
                            "player",
                            id,
                            old.EntrySpotId!,
                            old.LifecycleGeneration,
                            ZLinkSpotKind.Entry,
                            owner.OwnerId,
                            checked((ulong)owner.LeaseGeneration),
                            old.MeshName,
                            old.Rid,
                            old.LifecycleGeneration
                        )
                    ),
                    new ZLinkCapacityVector(1, 0, null)
                );
            var request = Encoding.UTF8.GetBytes(id);
            var reservation = Assert
                .IsType<ZLinkObjectReserveResult.Reserved>(
                    await repository.ReserveAsync(
                        new(
                            kind,
                            key,
                            type,
                            ZLinkInlineCreationIntentCodec.Encode(request),
                            SHA256.HashData(request),
                            request.Length,
                            new ZLinkMeshNodeDescriptorKey(old.MeshName, old.Rid),
                            old.LifecycleGeneration,
                            owner,
                            payload,
                            capacity
                        )
                    )
                )
                .Reservation;
            var previous = Assert
                .IsType<ZLinkObjectCommitResult.Committed>(
                    await repository.CommitAsync(reservation, payload)
                )
                .Snapshot;
            Assert.Equal(
                ZLinkOwnerLeaseReleaseResult.Released,
                await repository.ReleaseOwnerLeaseAsync(owner)
            );

            async Task<(RoutingId Rid, ulong Generation)> CreateAsync()
            {
                if (userSpot)
                {
                    var result = await client
                        .GetRequiredService<IZLinkSpotManager>()
                        .GetOrCreate(id, type)
                        .Timeout(TimeSpan.FromSeconds(5))
                        .Async();
                    Assert.Equal(ZLinkSpotCreateState.Created, result.State);
                    return (result.Spot.NodeRid, result.Spot.ObjectGeneration);
                }
                var actor = Assert
                    .IsType<ZLinkActorCreateResult.Created>(
                        await client
                            .GetRequiredService<IZLinkActorManager>()
                            .GetOrCreate(id, type)
                            .Timeout(TimeSpan.FromSeconds(5))
                            .Async()
                    )
                    .Actor;
                return (actor.NodeRid, actor.ObjectGeneration);
            }

            var waiting = await Assert.ThrowsAsync<ZLinkFrameworkException>(CreateAsync);
            Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, waiting.Kind);
            Assert.Equal(0, TestActorFactory.CreateCount + ReclaimProbeSpot.CreateCount);
            time.Advance(TimeSpan.FromSeconds(15));

            var created = await CreateAsync();
            Assert.Equal(targetRid, created.Rid);
            Assert.True(created.Generation > previous.ObjectGeneration);
            Assert.Equal(1, userSpot ? ReclaimProbeSpot.CreateCount : TestActorFactory.CreateCount);
        }
        finally
        {
            await source.StopAsync(CancellationToken.None);
            await target.StopAsync(CancellationToken.None);
        }
    }

    private sealed class ReclaimProbeSpot : IZLinkSpot
    {
        public static int CreateCount;

        public ReclaimProbeSpot(IZLinkSpotContext context)
        {
            Context = context;
            Interlocked.Increment(ref CreateCount);
        }

        public IZLinkSpotContext Context { get; }

        public void Configure() { }
    }
}
