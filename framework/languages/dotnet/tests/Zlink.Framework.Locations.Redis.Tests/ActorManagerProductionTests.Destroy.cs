using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Errors;
using Zlink.Framework.Contracts.Locations;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.Locations.Redis.Tests;

public sealed partial class ActorManagerProductionTests
{
    [Theory]
    [InlineData("get-or-create")]
    [InlineData("find")]
    [InlineData("join")]
    [InlineData("join-after-delete")]
    [InlineData("self-turn")]
    [InlineData("self-turn-failure")]
    [InlineData("self-turn-joins")]
    [InlineData("stale-ref")]
    public async Task DestroyPreservesActorIncarnationContract(string scenario)
    {
        var actorId = $"destroy-{scenario}";
        var key = ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorId);
        var store = new DestroyLocationStore(
            key,
            holdAuthorityRead: scenario == "join-after-delete"
        );
        var repository = new ZLinkProviderLocationRepository(store);
        await using var provider = BuildServer(store, "tcp://127.0.0.1:0");
        var runtime = provider.GetRequiredService<ZLinkFrameworkRuntime>();
        await FrameworkHost(provider).StartAsync(CancellationToken.None);
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        var ct = timeout.Token;
        var releaseTurn = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        try
        {
            var actors = provider.GetRequiredService<IZLinkActorManager>();
            var old = Assert
                .IsType<ZLinkActorCreateResult.Created>(
                    await actors.GetOrCreate(actorId, "player").Async(ct)
                )
                .Actor;
            if (scenario is "self-turn" or "self-turn-failure" or "self-turn-joins")
            {
                Assert.True(runtime.TryGetCreatedActorState(actorId, out var state));
                var accepted = new TaskCompletionSource(
                    TaskCreationOptions.RunContinuationsAsynchronously
                );
                var entered = new TaskCompletionSource(
                    TaskCreationOptions.RunContinuationsAsynchronously
                );
                var startSelf = new TaskCompletionSource(
                    TaskCreationOptions.RunContinuationsAsynchronously
                );
                var turn = state
                    .ExecuteDispatchAsync(
                        async _ =>
                        {
                            try
                            {
                                entered.SetResult();
                                await startSelf.Task.WaitAsync(ct);
                                Assert.True(await actors.DestroyAsync(old, ct));
                                accepted.SetResult();
                                await releaseTurn.Task.WaitAsync(ct);
                            }
                            catch (Exception error)
                            {
                                accepted.TrySetException(error);
                                throw;
                            }
                        },
                        ct
                    )
                    .AsTask();

                try
                {
                    Task<bool>? first = null;
                    if (scenario == "self-turn-joins")
                    {
                        await entered.Task.WaitAsync(ct);
                        first = actors.DestroyAsync(old, ct).AsTask();
                        await WaitUntilAsync(async () =>
                            await repository.ReadAuthorityAsync(key, ct)
                                is ZLinkAuthorityReadResult.Found found
                            && ZLinkActorAuthorityPayloadCodec.IsClosing(found.Snapshot)
                        );
                        Assert.False(first.IsCompleted);
                    }
                    startSelf.SetResult();
                    await accepted.Task.WaitAsync(ct);
                    var closing = Assert
                        .IsType<ZLinkAuthorityReadResult.Found>(
                            await repository.ReadAuthorityAsync(key, ct)
                        )
                        .Snapshot;
                    Assert.True(
                        ZLinkActorAuthorityPayloadCodec.TryDecode(
                            closing.Payload.Span,
                            out var payload
                        )
                    );
                    Assert.Equal(ZLinkActorAuthorityState.Closing, payload.State);
                    Assert.Equal(old.ObjectGeneration, closing.ObjectGeneration);
                    Assert.False(store.DeleteStarted.Task.IsCompleted);

                    store.FailDelete = scenario == "self-turn-failure";
                    var joined = actors.DestroyAsync(old, ct).AsTask();
                    releaseTurn.SetResult();
                    await turn.WaitAsync(ct);
                    await store.DeleteStarted.Task.WaitAsync(ct);
                    Assert.False(joined.IsCompleted);
                    store.ReleaseDelete.SetResult();
                    if (store.FailDelete)
                    {
                        var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                            joined
                        );
                        Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, failure.Kind);
                        Assert.IsType<InvalidOperationException>(failure.InnerException);
                        Assert.Same(
                            failure,
                            await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                                actors.DestroyAsync(old, ct).AsTask()
                            )
                        );
                        var remaining = Assert
                            .IsType<ZLinkAuthorityReadResult.Found>(
                                await repository.ReadAuthorityAsync(key, ct)
                            )
                            .Snapshot;
                        Assert.Equal(closing.StoreVersion, remaining.StoreVersion);
                    }
                    else
                        Assert.True(await joined.WaitAsync(ct));
                    if (first is not null)
                        Assert.True(await first.WaitAsync(ct));
                }
                finally
                {
                    startSelf.TrySetResult();
                    releaseTurn.TrySetResult();
                    store.ReleaseDelete.TrySetResult();
                    await turn.WaitAsync(ct);
                }
                return;
            }

            var destroy = actors.DestroyAsync(old, ct).AsTask();
            await store.DeleteStarted.Task.WaitAsync(ct);
            switch (scenario)
            {
                case "get-or-create":
                    var create = actors.GetOrCreate(actorId, "player").Async(ct).AsTask();
                    await store.AuthorityRead.Task.WaitAsync(ct);
                    store.ReleaseDelete.SetResult();
                    Assert.True(await destroy.WaitAsync(ct));
                    var created = Assert.IsType<ZLinkActorCreateResult.Created>(
                        await create.WaitAsync(ct)
                    );
                    Assert.True(created.Actor.ObjectGeneration > old.ObjectGeneration);
                    break;
                case "find":
                    Assert.Null(await actors.FindAsync(actorId, ct));
                    var duplicate = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                        actors.Create(actorId, "player").Async(ct).AsTask()
                    );
                    Assert.Equal(ZLinkFrameworkErrorKind.AlreadyExists, duplicate.Kind);
                    store.ReleaseDelete.SetResult();
                    Assert.True(await destroy.WaitAsync(ct));
                    break;
                case "join":
                case "join-after-delete":
                    var second = actors.DestroyAsync(old, ct).AsTask();
                    await store.AuthorityRead.Task.WaitAsync(ct);
                    Assert.False(second.IsCompleted);
                    store.ReleaseDelete.SetResult();
                    Assert.True(await destroy.WaitAsync(ct));
                    store.ReleaseAuthorityRead.SetResult();
                    Assert.True(await second.WaitAsync(ct));
                    Assert.Equal(1, store.DeleteCalls);
                    break;
                case "stale-ref":
                    store.ReleaseDelete.SetResult();
                    Assert.True(await destroy.WaitAsync(ct));
                    var replacement = Assert
                        .IsType<ZLinkActorCreateResult.Created>(
                            await actors.GetOrCreate(actorId, "player").Async(ct)
                        )
                        .Actor;
                    var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                        actors.DestroyAsync(old, ct).AsTask()
                    );
                    Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, error.Kind);
                    Assert.Equal(replacement, await actors.FindAsync(actorId, ct));
                    break;
            }
        }
        finally
        {
            releaseTurn.TrySetResult();
            store.ReleaseDelete.TrySetResult();
            store.ReleaseAuthorityRead.TrySetResult();
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    private sealed class DestroyLocationStore(ZLinkAuthorityKey key, bool holdAuthorityRead)
        : IZLinkLocationStore
    {
        private readonly ZLinkInMemoryProviderLocationStore _inner = new();
        private readonly ZLinkStoreKey _authorityKey =
            ZLinkProviderLocationRepository.AuthorityMetaKey(key);
        private int _deleteCalls;

        public TaskCompletionSource DeleteStarted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource ReleaseDelete { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource AuthorityRead { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource ReleaseAuthorityRead { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public int DeleteCalls => Volatile.Read(ref _deleteCalls);
        public bool FailDelete { get; set; }

        public async ValueTask<ZLinkStoreReadResult> ReadAsync(
            ZLinkStoreKey key,
            CancellationToken cancellationToken
        )
        {
            var result = await _inner.ReadAsync(key, cancellationToken);
            if (
                key == _authorityKey
                && DeleteStarted.Task.IsCompleted
                && AuthorityRead.TrySetResult()
            )
            {
                if (holdAuthorityRead)
                    await ReleaseAuthorityRead.Task.WaitAsync(cancellationToken);
            }
            return result;
        }

        public async ValueTask<ZLinkStoreWriteResult> WriteAsync(
            ZLinkStoreWriteRequest request,
            CancellationToken cancellationToken
        )
        {
            if (
                request
                    .Mutations.OfType<ZLinkStoreMutation.Delete>()
                    .Any(delete => delete.Key == _authorityKey)
            )
            {
                Interlocked.Increment(ref _deleteCalls);
                DeleteStarted.TrySetResult();
                await ReleaseDelete.Task.WaitAsync(cancellationToken);
                if (FailDelete)
                    throw new InvalidOperationException("Injected Actor authority delete failure.");
            }
            return await _inner.WriteAsync(request, cancellationToken);
        }

        public ValueTask<ZLinkStoreScanResult> ScanAsync(
            ZLinkStoreScanRequest request,
            CancellationToken cancellationToken
        ) => _inner.ScanAsync(request, cancellationToken);
    }
}
