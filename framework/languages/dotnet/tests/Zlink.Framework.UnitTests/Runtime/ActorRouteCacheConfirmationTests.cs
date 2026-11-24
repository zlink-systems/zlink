using Zlink.Framework.Contracts.Locations;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests;

public sealed class ActorRouteCacheConfirmationTests
{
    private static readonly ZLinkActorLocationKey Key = new("actor-1");
    private readonly ZLinkInMemoryLocationStore _store;
    private readonly PausableAuthorityStore _reads;
    private readonly ZLinkStoreLocationResolvers _resolvers;

    public ActorRouteCacheConfirmationTests()
    {
        var time = new ManualTimeProvider();
        _store = new ZLinkInMemoryLocationStore(time);
        _reads = new PausableAuthorityStore(_store);
        var options = new ZLinkLocationOptions { PollingInterval = TimeSpan.Zero };
        _resolvers = new ZLinkStoreLocationResolvers(
            _reads,
            new ZLinkOwnerLeaseTracker(_reads, options, time),
            options: options
        );
    }

    [Fact]
    public async Task First_Direct_Lookup_After_Binding_And_Two_Owner_Changes_Reads_Current_Owner()
    {
        var actorA = await PublishAsync("node-a");
        var binding = await _resolvers.ReadActorRowAsync(Key);
        Assert.Equal(actorA, binding!.ActorRef);
        await PublishAsync("node-b");
        var actorC = await PublishAsync("node-c");

        var direct = await _resolvers.ResolveActorRowAsync(Key);

        Assert.Equal(actorC.NodeRid, direct!.OwnerNodeRid);
        Assert.Same(direct, await _resolvers.ResolveActorRowAsync(Key));
    }

    [Fact]
    public async Task Binding_Confirmation_With_Newer_Authority_Evicts_The_Direct_Route()
    {
        await PublishAsync("node-a");
        Assert.NotNull(await _resolvers.ResolveActorRowAsync(Key));
        Assert.True(_resolvers.HasCachedActorRoute(Key));
        var actorB = await PublishAsync("node-b");

        var binding = await _resolvers.ReadActorRowAsync(Key);

        Assert.Equal(actorB, binding!.ActorRef);
        Assert.False(_resolvers.HasCachedActorRoute(Key));
        Assert.Equal(actorB, (await _resolvers.ResolveActorRowAsync(Key))!.ActorRef);
    }

    [Fact]
    public async Task Binding_Confirmation_With_Same_Authority_Preserves_The_Direct_Route()
    {
        await PublishAsync("node-a");
        var direct = await _resolvers.ResolveActorRowAsync(Key);
        Assert.NotNull(direct);

        await _resolvers.ReadActorRowAsync(Key);

        Assert.Same(direct, await _resolvers.ResolveActorRowAsync(Key));
    }

    [Fact]
    public async Task Binding_Confirmation_Of_Missing_Authority_Evicts_The_Direct_Route()
    {
        var actor = await PublishAsync("node-a");
        Assert.NotNull(await _resolvers.ResolveActorRowAsync(Key));
        Assert.True(_resolvers.HasCachedActorRoute(Key));
        var authorityKey = ZLinkActorAuthorityPayloadCodec.AuthorityKey(actor.ActorId);
        var stored = Assert.IsType<ZLinkAuthorityReadResult.Found>(
            await _store.ReadAuthorityAsync(authorityKey)
        );
        Assert.IsType<ZLinkAuthorityCompareExchangeResult.Deleted>(
            await _store.CompareExchangeAuthorityAsync(
                authorityKey,
                stored.Snapshot.StoreVersion,
                new ZLinkAuthorityMutation.Delete()
            )
        );

        Assert.Null(await _resolvers.ReadActorRowAsync(Key));

        Assert.False(_resolvers.HasCachedActorRoute(Key));
    }

    [Fact]
    public async Task Binding_Confirmation_Of_Invalid_Owner_Lease_Evicts_The_Direct_Route()
    {
        await PublishAsync("node-a");
        var direct = await _resolvers.ResolveActorRowAsync(Key);
        Assert.NotNull(direct);
        Assert.True(_resolvers.HasCachedActorRoute(Key));
        Assert.Equal(
            ZLinkOwnerLeaseReleaseResult.Released,
            await _store.ReleaseOwnerLeaseAsync(
                new ZLinkLocationOwnerToken(direct.OwnerId, direct.LeaseGeneration)
            )
        );

        Assert.Null(await _resolvers.ReadActorRowAsync(Key));

        Assert.False(_resolvers.HasCachedActorRoute(Key));
    }

    [Fact]
    public async Task Delayed_Binding_Confirmation_Preserves_A_Newer_Direct_Route()
    {
        await PublishAsync("node-a");
        Assert.NotNull(await _resolvers.ResolveActorRowAsync(Key));
        var actorB = await PublishAsync("node-b");
        _reads.PauseNextRead = true;
        var binding = _resolvers.ReadActorRowAsync(Key).AsTask();
        await _reads.ReadCaptured.Task.WaitAsync(TimeSpan.FromSeconds(5));

        try
        {
            await _resolvers.InvalidateActorRouteAsync(Key);
            var actorC = await PublishAsync("node-c");
            var direct = await _resolvers.ResolveActorRowAsync(Key);
            Assert.Equal(actorC, direct!.ActorRef);

            _reads.ResumeRead.SetResult();
            Assert.Equal(actorB, (await binding)!.ActorRef);
            Assert.Same(direct, await _resolvers.ResolveActorRowAsync(Key));
        }
        finally
        {
            _reads.ResumeRead.TrySetResult();
        }
    }

    private async Task<ActorRef> PublishAsync(string node)
    {
        await _store.ClaimLiveOwnerAsync(node, TimeSpan.FromMinutes(1));
        var rid = RoutingId.From(node);
        var row = InMemoryLocationStoreTests.Actor(node) with { OwnerNodeRid = rid };
        var snapshot = Assert.IsType<ZLinkAuthoritySnapshot>(
            await AuthorityLocationTestFixture.PublishActorAsync(_store, row, replace: true)
        );
        return new ActorRef(row.ActorId, snapshot.ObjectGeneration, row.MeshName, rid);
    }

    private sealed class PausableAuthorityStore(ZLinkInMemoryLocationStore store)
        : ZLinkLocationStoreTestDouble
    {
        internal bool PauseNextRead { get; set; }
        internal TaskCompletionSource ReadCaptured { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        internal TaskCompletionSource ResumeRead { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public override ValueTask<ZLinkOwnerLeaseReadResult> ReadOwnerLeaseAsync(
            string ownerId,
            CancellationToken cancellationToken = default
        ) => store.ReadOwnerLeaseAsync(ownerId, cancellationToken);

        public override async ValueTask<ZLinkAuthorityReadResult> ReadAuthorityAsync(
            ZLinkAuthorityKey key,
            CancellationToken cancellationToken = default
        )
        {
            var result = await store.ReadAuthorityAsync(key, cancellationToken);
            if (PauseNextRead)
            {
                PauseNextRead = false;
                ReadCaptured.SetResult();
                await ResumeRead.Task.WaitAsync(cancellationToken);
            }
            return result;
        }
    }
}
