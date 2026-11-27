using System.Diagnostics;
using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Errors;
using Zlink.Framework.Contracts.Locations;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.Locations.Redis.Tests;

public sealed partial class ActorManagerProductionTests
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task RemoteDestroyObservesAuthorityAfterOneSubmission(bool failDelete)
    {
        const string endpoint = "tcp://127.0.0.1:0";
        const string actorId = "destroy-admitted-owner";
        var key = ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorId);
        var store = new DestroyLocationStore(key, holdAuthorityRead: false)
        {
            FailDelete = failDelete,
        };
        var repository = new ZLinkProviderLocationRepository(store);
        await using var owner = BuildServer(store, endpoint);
        await using var caller = BuildClient(store, endpoint, TimeSpan.FromSeconds(2));
        var ownerRuntime = owner.GetRequiredService<ZLinkFrameworkRuntime>();
        var callerRuntime = caller.GetRequiredService<ZLinkFrameworkRuntime>();
        await FrameworkHost(owner).StartAsync(CancellationToken.None);
        await FrameworkHost(caller).StartAsync(CancellationToken.None);
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        var ct = timeout.Token;
        try
        {
            var actor = Assert
                .IsType<ZLinkActorCreateResult.Created>(
                    await owner
                        .GetRequiredService<IZLinkActorManager>()
                        .GetOrCreate(actorId, "player")
                        .Async(ct)
                )
                .Actor;
            var callerNode = callerRuntime.GetSpotNodeRuntime("objects").Node;
            await WaitUntilAsync(() => callerNode.MeshStatus().AdmittedPeerCount == 1);
            using var monitor = callerNode.OpenMeshMonitor(MeshMonitorEventMask.MessageSubmitted);

            var destroy = caller
                .GetRequiredService<IZLinkActorManager>()
                .DestroyAsync(actor, ct)
                .AsTask();
            await store.DeleteStarted.Task.WaitAsync(ct);
            var closing = Assert.IsType<ZLinkAuthorityReadResult.Found>(
                await repository.ReadAuthorityAsync(key, ct)
            );
            Assert.True(ZLinkActorAuthorityPayloadCodec.IsClosing(closing.Snapshot));
            Assert.False(destroy.IsCompleted);
            store.ReleaseDelete.SetResult();

            if (failDelete)
            {
                var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(() => destroy);
                Assert.Equal(ZLinkFrameworkErrorKind.DeadlineExceeded, failure.Kind);
                var remaining = Assert.IsType<ZLinkAuthorityReadResult.Found>(
                    await repository.ReadAuthorityAsync(key, ct)
                );
                Assert.Equal(closing.Snapshot.StoreVersion, remaining.Snapshot.StoreVersion);
            }
            else
            {
                Assert.True(await destroy);
                Assert.IsType<ZLinkAuthorityReadResult.Missing>(
                    await repository.ReadAuthorityAsync(key, ct)
                );
            }
            Assert.Equal(1, store.DeleteCalls);
            Assert.Equal(1UL, monitor.Status().SubmittedMessages);
        }
        finally
        {
            store.ReleaseDelete.TrySetResult();
            await callerRuntime.StopAsync(CancellationToken.None);
            await ownerRuntime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task Remote_destroy_without_admitted_owner_is_unavailable()
    {
        const string endpoint = "tcp://127.0.0.1:0";
        const string actorId = "destroy-unadmitted-owner";
        var requestTimeout = TimeSpan.FromSeconds(2);
        var store = new ZLinkInMemoryProviderLocationStore();
        var repository = new ZLinkProviderLocationRepository(store);
        await using var owner = BuildServer(store, endpoint, "z-destroy-owner");
        await using var caller = BuildClient(
            new AuthorityOnlyStore(store),
            endpoint,
            requestTimeout,
            "a-destroy-caller"
        );
        var ownerRuntime = owner.GetRequiredService<ZLinkFrameworkRuntime>();
        var callerRuntime = caller.GetRequiredService<ZLinkFrameworkRuntime>();
        await FrameworkHost(owner).StartAsync(CancellationToken.None);
        await FrameworkHost(caller).StartAsync(CancellationToken.None);
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        var ct = timeout.Token;
        try
        {
            var actor = Assert
                .IsType<ZLinkActorCreateResult.Created>(
                    await owner
                        .GetRequiredService<IZLinkActorManager>()
                        .GetOrCreate(actorId, "player")
                        .Async(ct)
                )
                .Actor;
            var key = ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorId);
            var before = Assert.IsType<ZLinkAuthorityReadResult.Found>(
                await repository.ReadAuthorityAsync(key, ct)
            );
            var actors = caller.GetRequiredService<IZLinkActorManager>();
            Assert.Equal(actor, await actors.FindAsync(actorId, ct));
            var callerNode = callerRuntime.GetSpotNodeRuntime("objects").Node;
            Assert.Equal(0U, callerNode.MeshStatus().AdmittedPeerCount);
            using var monitor = callerNode.OpenMeshMonitor(MeshMonitorEventMask.MessageSubmitted);

            var started = Stopwatch.GetTimestamp();
            var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                actors.DestroyAsync(actor, ct).AsTask()
            );
            var elapsed = Stopwatch.GetElapsedTime(started);
            var submitted = monitor.Status().SubmittedMessages;
            var after = Assert.IsType<ZLinkAuthorityReadResult.Found>(
                await repository.ReadAuthorityAsync(key, ct)
            );
            Assert.Equal(before.Snapshot.StoreVersion, after.Snapshot.StoreVersion);
            Assert.True(
                failure.Kind == ZLinkFrameworkErrorKind.Unavailable
                    && elapsed < requestTimeout
                    && submitted == 0,
                $"Destroy returned {failure.Kind} after {elapsed.TotalMilliseconds:F0} ms "
                    + $"with {submitted} mesh submissions."
            );
        }
        finally
        {
            await callerRuntime.StopAsync(CancellationToken.None);
            await ownerRuntime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task RemoteDestroyReleasesClosingAfterOwnerLeaseEnds()
    {
        const string actorId = "destroy-ended-owner";
        const string endpoint = "tcp://127.0.0.1:0";
        var key = ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorId);
        var store = new ZLinkInMemoryProviderLocationStore();
        var ownerStore = new DestroyLocationStore(key, holdAuthorityRead: false, store);
        var repository = new ZLinkProviderLocationRepository(store);
        await using var owner = BuildServer(ownerStore, endpoint);
        await using var caller = BuildClient(
            store,
            endpoint,
            TimeSpan.FromSeconds(2),
            ownerLeaseFencingMargin: TimeSpan.FromMilliseconds(100)
        );
        var ownerRuntime = owner.GetRequiredService<ZLinkFrameworkRuntime>();
        var callerRuntime = caller.GetRequiredService<ZLinkFrameworkRuntime>();
        await FrameworkHost(owner).StartAsync(CancellationToken.None);
        await FrameworkHost(caller).StartAsync(CancellationToken.None);
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        var ct = timeout.Token;

        try
        {
            var actor = Assert
                .IsType<ZLinkActorCreateResult.Created>(
                    await owner
                        .GetRequiredService<IZLinkActorManager>()
                        .GetOrCreate(actorId, "player")
                        .Async(ct)
                )
                .Actor;
            var callerNode = callerRuntime.GetSpotNodeRuntime("objects").Node;
            await WaitUntilAsync(() => callerNode.MeshStatus().AdmittedPeerCount == 1);
            var destroy = caller
                .GetRequiredService<IZLinkActorManager>()
                .DestroyAsync(actor, ct)
                .AsTask();
            await ownerStore.DeleteStarted.Task.WaitAsync(ct);

            var closing = Assert
                .IsType<ZLinkAuthorityReadResult.Found>(
                    await repository.ReadAuthorityAsync(key, ct)
                )
                .Snapshot;
            Assert.Equal(actor.ObjectGeneration, closing.ObjectGeneration);
            Assert.True(ZLinkActorAuthorityPayloadCodec.IsClosing(closing));
            Assert.False(destroy.IsCompleted);
            var ownerToken = new ZLinkLocationOwnerToken(
                closing.OwnerId,
                closing.OwnerLeaseGeneration
            );
            Assert.Equal(
                ZLinkOwnerLeaseReleaseResult.Released,
                await repository.ReleaseOwnerLeaseAsync(ownerToken, ct)
            );
            Assert.IsType<ZLinkStoreReadResult.Missing>(
                await store.ReadAsync(ZLinkProviderLocationRepository.OwnerKey(closing.OwnerId), ct)
            );

            Assert.True(await destroy);
            Assert.IsType<ZLinkAuthorityReadResult.Missing>(
                await repository.ReadAuthorityAsync(key, ct)
            );
        }
        finally
        {
            ownerStore.ReleaseDelete.TrySetResult();
            await callerRuntime.StopAsync(CancellationToken.None);
            await ownerRuntime.StopAsync(CancellationToken.None);
        }
    }

    private sealed class AuthorityOnlyStore(IZLinkLocationStore store) : IZLinkLocationStore
    {
        public ValueTask<ZLinkStoreReadResult> ReadAsync(
            ZLinkStoreKey key,
            CancellationToken cancellationToken
        ) => store.ReadAsync(key, cancellationToken);

        public ValueTask<ZLinkStoreWriteResult> WriteAsync(
            ZLinkStoreWriteRequest request,
            CancellationToken cancellationToken
        ) => store.WriteAsync(request, cancellationToken);

        public ValueTask<ZLinkStoreScanResult> ScanAsync(
            ZLinkStoreScanRequest request,
            CancellationToken cancellationToken
        ) =>
            ValueTask.FromResult<ZLinkStoreScanResult>(
                new ZLinkStoreScanResult.Page(new([], null, DateTimeOffset.UtcNow))
            );
    }
}
