using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Configuration;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class EntrySpotActorDispatchTests
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task EndedActorCreationUsesRemoteTargetPolicy(bool createOnly)
    {
        var time = new ManualTimeProvider();
        var node = new CapturingSpotNode();
        AdmitAcceptingPeer(node);
        var (runtime, _) = await CreateStartedRuntimeAsync(
            node,
            includeActorFactory: false,
            preseedActorOwnership: false,
            locationTimeProvider: time,
            joinTimeProvider: time
        );
        try
        {
            Assert.Empty(runtime.Registration.SpotNodes["entry"].ActorFactories);
            runtime.Registration.SpotNodes["entry"].ObjectRole = ZLinkMeshNodeObjectRole.Client;
            runtime.Registration.SpotNodes["entry"].ObjectRoleSelected = true;
            var store = RequireLocationStore(runtime);
            var target = await PublishAcceptingNodeAsync(runtime, store);
            var oldOwner = await store.ClaimLiveOwnerAsync("ended-owner", TimeSpan.FromMinutes(1));
            var old = target with
            {
                Rid = RoutingId.From("ended-owner-node"),
                Endpoint = "inproc://ended-owner-node",
                EntrySpotId = "entry:ended-owner-node",
                OwnerId = oldOwner.OwnerId,
                LeaseGeneration = oldOwner.LeaseGeneration,
            };
            Assert.Equal(
                ZLinkLocationWriteStatus.Stored,
                (await store.UpdateMeshNodeAsync(old, ZLinkLocationWriteIntent.NewClaim)).Status
            );
            const string actorId = "ended-remote-actor";
            const string stableType = "probe";
            var key = ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorId);
            var previous = await ReserveAndCommitLocalAuthorityAsync(
                store,
                key,
                ZLinkPlacementObjectKind.Actor,
                stableType,
                old.Rid,
                oldOwner,
                ZLinkActorAuthorityPayloadCodec.Encode(
                    new ZLinkActorAuthorityPayload(
                        ZLinkActorAuthorityState.Ready,
                        stableType,
                        actorId,
                        old.EntrySpotId!,
                        1,
                        ZLinkSpotKind.Entry,
                        oldOwner.OwnerId,
                        checked((ulong)oldOwner.LeaseGeneration),
                        old.MeshName,
                        old.Rid,
                        old.LifecycleGeneration
                    )
                ),
                new ZLinkCapacityVector(1, 0, null)
            );
            await store.ReleaseOwnerLeaseAsync(oldOwner);
            var creates = HandleRemoteCreatesOnAcceptingNode(node, runtime, store);
            var manager = new ZLinkActorManagerService(runtime);

            async Task<ZLinkActorCreateResult> CreateAsync() =>
                createOnly
                    ? await manager.Create(actorId, stableType).InMesh("entry").Async()
                    : await manager.GetOrCreate(actorId, stableType).InMesh("entry").Async();

            var waiting = await Assert.ThrowsAsync<ZLinkFrameworkException>(CreateAsync);
            Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, waiting.Kind);
            Assert.Empty(creates);
            time.Advance(TimeSpan.FromSeconds(15));

            var created = Assert.IsType<ZLinkActorCreateResult.Created>(await CreateAsync());
            Assert.True(created.Actor.ObjectGeneration > previous.ObjectGeneration);
            Assert.Equal(AcceptingNodeRid, created.Actor.NodeRid);
            Assert.Equal(actorId, Assert.Single(creates));
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }
}
