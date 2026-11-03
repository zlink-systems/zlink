using Zlink.Framework.Contracts.Locations;
using Zlink.Framework.Runtime.Identifiers;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests;

public sealed class AutoConnectLoopTests
{
    private static ZLinkMeshName Mesh(string value) =>
        ZLinkMeshName.FromBoundary(value, nameof(value));

    [Fact]
    public async Task Repeated_Dispose_Callers_Share_AutoConnect_Loop_Finalization()
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryLocationStore(time);
        var options = new ZLinkLocationOptions { PollingInterval = TimeSpan.Zero };
        var runtime = new ZLinkLocationRuntime(options, store, time);
        var tracker = new ZLinkOwnerLeaseTracker(store, options, time);
        var resolvers = new ZLinkStoreLocationResolvers(store, tracker);
        var local = new ZLinkAutoConnectLocal(
            ZLinkLocationAutoConnectType.ClientServer,
            Mesh("dispose"),
            ZLinkLocationRole.Dealer,
            NodeRid: null,
            Endpoint: string.Empty
        );
        var reconciler = new ZLinkAutoConnectReconciler(
            local,
            null,
            runtime,
            resolvers,
            new NullExecutor(),
            options,
            time
        );
        var loop = new ZLinkAutoConnectLoop(
            new ZLinkRuntimeErrorSink(),
            reconciler,
            local,
            options,
            store,
            timeProvider: time
        );

        var first = loop.DisposeAsync().AsTask();
        var second = loop.DisposeAsync().AsTask();

        Assert.Same(first, second);
        await Task.WhenAll(first, second).WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task Cancelled_disposal_remains_terminal_for_later_callers()
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryLocationStore(time);
        var options = new ZLinkLocationOptions { PollingInterval = TimeSpan.Zero };
        await using var runtime = new ZLinkLocationRuntime(options, store, time);
        await runtime.RenewOwnerLeaseOnceAsync();
        var resolvers = new ZLinkStoreLocationResolvers(
            store,
            new ZLinkOwnerLeaseTracker(store, options, time)
        );
        var local = new ZLinkAutoConnectLocal(
            ZLinkLocationAutoConnectType.ClientServer,
            Mesh("play"),
            ZLinkLocationRole.Dealer,
            RoutingId.From("local"),
            "tcp://l:1"
        );
        var reconciler = new ZLinkAutoConnectReconciler(
            local,
            InMemoryLocationStoreTests.MeshNode("ignored", "tcp://l:1", "local"),
            runtime,
            resolvers,
            new NullExecutor(),
            options,
            time
        );
        var loop = new ZLinkAutoConnectLoop(
            new ZLinkRuntimeErrorSink(),
            reconciler,
            local,
            options,
            store,
            timeProvider: time
        );
        await loop.TickAsync();
        using var deadline = new CancellationTokenSource();
        deadline.Cancel();
        var first = loop.DisposeAsync(deadline.Token).AsTask();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => first);
        var second = loop.DisposeAsync().AsTask();
        Assert.Same(first, second);
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => second);
        Assert.Single((await store.ListMeshNodesAsync("play", default)).Items);
    }

    [Fact]
    public async Task Unchanged_Change_Stamp_Skips_The_List_Query()
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryLocationStore(time);
        var options = new ZLinkLocationOptions { PollingInterval = TimeSpan.Zero };
        var runtime = new ZLinkLocationRuntime(options, store, time);
        await runtime.RenewOwnerLeaseOnceAsync();
        await store.ClaimLiveOwnerAsync("peer-owner", TimeSpan.FromMinutes(10));

        var tracker = new ZLinkOwnerLeaseTracker(store, options, time);
        var resolvers = new ZLinkStoreLocationResolvers(store, tracker);
        var countingResolver = new CountingPeerResolver(resolvers);
        var local = new ZLinkAutoConnectLocal(
            ZLinkLocationAutoConnectType.ClientServer,
            Mesh("play"),
            ZLinkLocationRole.Dealer,
            RoutingId.From("local"),
            "tcp://l:1"
        );
        var localRow = InMemoryLocationStoreTests.MeshNode("ignored", "tcp://l:1", "local");
        var reconciler = new ZLinkAutoConnectReconciler(
            local,
            localRow,
            runtime,
            countingResolver,
            new NullExecutor(),
            options,
            time
        );
        var loop = new ZLinkAutoConnectLoop(
            new ZLinkRuntimeErrorSink(),
            reconciler,
            local,
            options,
            store,
            timeProvider: time
        );

        // First tick always reads the list (and publishes the local row,
        // which bumps the stamp), so the second tick still reads once more
        // before the stamp settles.
        await loop.TickAsync();
        await loop.TickAsync();
        var reads = countingResolver.ListCalls;

        // No writes since the last tick: the stamp is unchanged and the
        // polling tick costs one stamp read, not a full list query.
        await loop.TickAsync();
        await loop.TickAsync();
        Assert.Equal(reads, countingResolver.ListCalls);

        // A peer write bumps the stamp and the next tick reads the list.
        await store.UpdateMeshNodeAsync(
            InMemoryLocationStoreTests.MeshNode(
                "peer-owner",
                "tcp://r:1",
                "r1",
                leaseGeneration: 2
            ),
            ZLinkLocationWriteIntent.NewClaim
        );
        await loop.TickAsync();
        Assert.Equal(reads + 1, countingResolver.ListCalls);
    }

    [Fact]
    public async Task Reclaimed_Owner_Token_Does_Not_Revive_A_Stale_Descriptor()
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryLocationStore(time);
        var options = new ZLinkLocationOptions { PollingInterval = TimeSpan.Zero };
        var runtime = new ZLinkLocationRuntime(options, store, time);
        await runtime.RenewOwnerLeaseOnceAsync();

        var tracker = new ZLinkOwnerLeaseTracker(store, options, time);
        var resolvers = new ZLinkStoreLocationResolvers(store, tracker);
        var executor = new RecordingAutoConnectExecutor();
        var local = new ZLinkAutoConnectLocal(
            ZLinkLocationAutoConnectType.ClientServer,
            Mesh("play"),
            ZLinkLocationRole.Dealer,
            NodeRid: null,
            Endpoint: string.Empty
        );
        var reconciler = new ZLinkAutoConnectReconciler(
            local,
            localRow: null,
            runtime,
            resolvers,
            executor,
            options,
            time
        );
        var loop = new ZLinkAutoConnectLoop(
            new ZLinkRuntimeErrorSink(),
            reconciler,
            local,
            options,
            store,
            timeProvider: time,
            leaseTracker: tracker
        );

        await store.ClaimLiveOwnerAsync("late-owner", TimeSpan.FromSeconds(15));
        await store.UpdateMeshNodeAsync(
            InMemoryLocationStoreTests.MeshNode(
                "late-owner",
                "tcp://r:1",
                "r1",
                leaseGeneration: 2
            ),
            ZLinkLocationWriteIntent.NewClaim
        );
        Assert.Equal(
            ZLinkOwnerLeaseReleaseResult.Released,
            await store.ReleaseOwnerLeaseAsync(new ZLinkLocationOwnerToken("late-owner", 2))
        );
        await loop.TickAsync();
        Assert.Empty(executor.Connected);

        // A reclaimed owner ID receives a different token. The stale
        // descriptor cannot become live merely because the owner ID matches.
        await store.ClaimLiveOwnerAsync("late-owner", TimeSpan.FromSeconds(15));
        await loop.TickAsync();
        Assert.Empty(executor.Connected);
        Assert.Empty(executor.Disconnected);
    }

    [Fact]
    public async Task Pending_Connect_Retries_When_The_Change_Stamp_Is_Unchanged()
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryLocationStore(time);
        var options = new ZLinkLocationOptions { PollingInterval = TimeSpan.Zero };
        var runtime = new ZLinkLocationRuntime(options, store, time);
        await runtime.RenewOwnerLeaseOnceAsync();
        await store.ClaimLiveOwnerAsync("peer-owner", TimeSpan.FromMinutes(1));
        await store.UpdateMeshNodeAsync(
            InMemoryLocationStoreTests.MeshNode(
                "peer-owner",
                "tcp://r:1",
                "r1",
                leaseGeneration: 2
            ),
            ZLinkLocationWriteIntent.NewClaim
        );
        var tracker = new ZLinkOwnerLeaseTracker(store, options, time);
        var resolvers = new ZLinkStoreLocationResolvers(store, tracker);
        var executor = new RetryExecutor();
        var local = new ZLinkAutoConnectLocal(
            ZLinkLocationAutoConnectType.ClientServer,
            Mesh("play"),
            ZLinkLocationRole.Dealer,
            NodeRid: null,
            Endpoint: string.Empty
        );
        var reconciler = new ZLinkAutoConnectReconciler(
            local,
            null,
            runtime,
            resolvers,
            executor,
            options,
            time
        );
        var loop = new ZLinkAutoConnectLoop(
            new ZLinkRuntimeErrorSink(),
            reconciler,
            local,
            options,
            store,
            timeProvider: time,
            leaseTracker: tracker
        );

        await loop.TickAsync();
        await loop.TickAsync();

        Assert.Equal(2, executor.ConnectCalls);
        Assert.Single(reconciler.ActiveTargets);
    }

    [Fact]
    public async Task Failed_Stamp_Preflight_Defers_Missing_Target_Until_Owner_Lease_Ttl()
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryLocationStore(time);
        var options = new ZLinkLocationOptions
        {
            PollingInterval = TimeSpan.Zero,
            OwnerLeaseRenewInterval = TimeSpan.FromSeconds(1),
        };
        var runtime = new ZLinkLocationRuntime(options, store, time);
        await runtime.RenewOwnerLeaseOnceAsync();
        var peer = InMemoryLocationStoreTests.MeshNode("peer-owner", "tcp://r:1", "r1");
        var resolver = new SwitchablePeerResolver([peer]);
        var executor = new RecordingAutoConnectExecutor();
        var local = new ZLinkAutoConnectLocal(
            ZLinkLocationAutoConnectType.ClientServer,
            Mesh("play"),
            ZLinkLocationRole.Dealer,
            NodeRid: null,
            Endpoint: string.Empty
        );
        var reconciler = new ZLinkAutoConnectReconciler(
            local,
            null,
            runtime,
            resolver,
            executor,
            options,
            time
        );
        var stamps = new FailingStampStore();
        var loop = new ZLinkAutoConnectLoop(
            new ZLinkRuntimeErrorSink(),
            reconciler,
            local,
            options,
            stamps,
            timeProvider: time
        );

        await loop.TickAsync();
        Assert.Single(reconciler.ActiveTargets);

        // The failed stamp read marks recovery before the complete list read succeeds.
        stamps.FailNext = true;
        resolver.Rows = [];
        await loop.TickAsync();

        Assert.Empty(executor.Disconnected);
        Assert.Single(reconciler.ActiveTargets);

        time.Advance(TimeSpan.FromSeconds(14));
        Assert.True(await runtime.RenewOwnerLeaseOnceAsync());
        time.Advance(TimeSpan.FromSeconds(1));
        await loop.TickAsync();

        Assert.Single(executor.Disconnected);
        Assert.Empty(reconciler.ActiveTargets);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Publication_awaits_without_blocking_and_preserves_snapshot_order(
        bool recovery
    )
    {
        await using var fixture = await PublicationFixture.CreateAsync(TimeSpan.FromSeconds(5));
        if (recovery)
        {
            await fixture.Loop.TickAsync();
            await fixture.Reconciler.NoteStoreFailureAsync();
        }
        var reads = fixture.Peers.ListCalls;
        fixture.Store.HoldWrites = true;

        // The invoking thread must return the pending task before the Store completes.
        var invocation = Task.Factory.StartNew(
            () => fixture.Loop.TickAsync().AsTask(),
            CancellationToken.None,
            TaskCreationOptions.LongRunning,
            TaskScheduler.Default
        );
        var first = await invocation.WaitAsync(TimeSpan.FromSeconds(1));
        var firstWrite = await fixture.Store.NextWriteAsync();
        Assert.False(first.IsCompleted);
        Assert.Equal(reads, fixture.Peers.ListCalls);

        fixture.Reconciler.SetLocalWeight(2);
        var second = fixture.Loop.TickAsync().AsTask();
        Assert.False(second.IsCompleted);
        Assert.Equal(reads, fixture.Peers.ListCalls);
        firstWrite.Complete();
        await first.WaitAsync(TimeSpan.FromSeconds(1));
        var secondWrite = await fixture.Store.NextWriteAsync();
        Assert.Equal(reads + 1, fixture.Peers.ListCalls);
        Assert.Equal(2, secondWrite.Row.ChannelWeights["play"]);
        secondWrite.Complete();
        await second.WaitAsync(TimeSpan.FromSeconds(1));

        Assert.Equal(reads + 2, fixture.Peers.ListCalls);
        Assert.Equal(
            Enumerable.Range(1, reads + 2).Select(index => $"r{index}"),
            fixture.Executor.Connected.Select(target => target.NodeRid.ToString())
        );
    }

    [Fact]
    public async Task Descriptor_publication_uses_runtime_clock_for_its_deadline()
    {
        await using var fixture = await PublicationFixture.CreateAsync(
            TimeSpan.FromMilliseconds(100)
        );
        fixture.Store.HoldWrites = true;
        var write = fixture
            .Runtime.WriteDescriptorAsync(
                InMemoryLocationStoreTests.MeshNode("clock", "tcp://l:1", "local"),
                ZLinkLocationWriteIntent.NewClaim
            )
            .AsTask();
        var pending = await fixture.Store.NextWriteAsync();

        Assert.False(write.IsCompleted);
        Assert.Equal(1, fixture.Time.ActiveTimerCount);
        fixture.Time.Advance(fixture.Options.OwnerLeaseRenewTimeout);
        Assert.True(pending.Token.IsCancellationRequested);
        await Assert.ThrowsAsync<ZLinkDescriptorPublicationUnconfirmedException>(() => write);
        Assert.Equal(0, fixture.Time.ActiveTimerCount);
        Assert.False(pending.Result.Task.IsCompleted);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Unresponsive_publication_times_out_and_next_tick_republishes(bool recovery)
    {
        await using var fixture = await PublicationFixture.CreateAsync(
            TimeSpan.FromMilliseconds(100)
        );
        if (recovery)
        {
            await fixture.Loop.TickAsync();
            await fixture.Reconciler.NoteStoreFailureAsync();
        }
        var reads = fixture.Peers.ListCalls;
        fixture.Store.HoldWrites = true;
        var tick = fixture.Loop.TickAsync().AsTask();
        var pending = await fixture.Store.NextWriteAsync();

        Assert.False(tick.IsCompleted);
        fixture.Time.Advance(fixture.Options.OwnerLeaseRenewTimeout);
        Assert.True(pending.Token.IsCancellationRequested);
        await tick.WaitAsync(TimeSpan.FromSeconds(2));

        Assert.True(pending.Token.IsCancellationRequested);
        Assert.False(pending.Result.Task.IsCompleted);
        Assert.True(fixture.Reconciler.StoreFailed);
        Assert.Null(fixture.Reconciler.LocalWriteStatus);
        Assert.Equal(reads, fixture.Peers.ListCalls);
        var renewal = fixture.Loop.TickAsync().AsTask();
        var retry = await fixture.Store.NextWriteAsync();
        retry.Complete();
        await renewal.WaitAsync(TimeSpan.FromSeconds(1));
        Assert.False(fixture.Reconciler.StoreFailed);
        Assert.Equal(ZLinkLocationWriteStatus.Stored, fixture.Reconciler.LocalWriteStatus);
        Assert.Equal(reads + 1, fixture.Peers.ListCalls);
        pending.Complete();
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Stop_cancels_unresponsive_publication_and_late_completion_cannot_resume_it(
        bool lateFailure
    )
    {
        await using var fixture = await PublicationFixture.CreateAsync(TimeSpan.FromSeconds(2));
        await fixture.Loop.StartAsync();
        await fixture.Reconciler.NoteStoreFailureAsync();
        fixture.Store.HoldWrites = true;
        fixture.Loop.Wake();
        var pending = await fixture.Store.NextWriteAsync();
        var reads = fixture.Peers.ListCalls;
        var writes = fixture.Store.WriteCalls;

        await fixture.Loop.StopAsync().AsTask().WaitAsync(fixture.Options.OwnerLeaseRenewTimeout);

        Assert.True(pending.Token.IsCancellationRequested);
        Assert.False(pending.Result.Task.IsCompleted);
        Assert.Equal(reads, fixture.Peers.ListCalls);
        Assert.Equal(writes, fixture.Store.WriteCalls);
        Assert.Empty(fixture.Reconciler.ActiveTargets);
        if (lateFailure)
            pending.Result.SetException(new InvalidOperationException("late Store failure"));
        else
            pending.Complete();
        await pending.Returned.Task.WaitAsync(TimeSpan.FromSeconds(1));
        // A second stop is a barrier after the late Store completion as well.
        await fixture.Loop.StopAsync();
        Assert.Equal(reads, fixture.Peers.ListCalls);
        Assert.Equal(writes, fixture.Store.WriteCalls);
        Assert.Null(fixture.Reconciler.LocalWriteStatus);
        Assert.Empty(fixture.Reconciler.ActiveTargets);
    }

    [Fact]
    public async Task Completion_after_stop_cancellation_does_not_process_another_snapshot()
    {
        await using var fixture = await PublicationFixture.CreateAsync(TimeSpan.FromSeconds(2));
        await fixture.Loop.StartAsync();
        await fixture.Reconciler.NoteStoreFailureAsync();
        fixture.Store.HoldWrites = true;
        fixture.Loop.Wake();
        var pending = await fixture.Store.NextWriteAsync();
        var reads = fixture.Peers.ListCalls;
        var writes = fixture.Store.WriteCalls;
        var cancelled = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        using var registration = pending.Token.Register(() => cancelled.TrySetResult());

        var stop = fixture.Loop.StopAsync().AsTask();
        await cancelled.Task.WaitAsync(TimeSpan.FromSeconds(1));
        pending.Complete();
        await stop.WaitAsync(fixture.Options.OwnerLeaseRenewTimeout);

        Assert.Equal(reads, fixture.Peers.ListCalls);
        Assert.Equal(writes, fixture.Store.WriteCalls);
        Assert.Empty(fixture.Reconciler.ActiveTargets);
    }

    [Fact]
    public async Task Publication_rejects_result_when_Store_completes_synchronously_after_cancellation()
    {
        using var stop = new CancellationTokenSource();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
            ZLinkDescriptorPublicationUnconfirmedException
                .WriteAsync(
                    _ =>
                    {
                        // Cancellation can arrive before the provider returns its completed ValueTask.
                        stop.Cancel();
                        return ValueTask.FromResult(ZLinkLocationWriteResult.Stored(1, default));
                    },
                    TimeSpan.FromSeconds(2),
                    stop.Token
                )
                .AsTask()
        );
    }

    private sealed class PublicationFixture : IAsyncDisposable
    {
        internal ZLinkLocationOptions Options { get; }
        internal ControllableTimeProvider Time { get; } = new();
        internal ControlledPublicationStore Store { get; }
        internal OrderedPeerResolver Peers { get; } = new();
        internal RecordingAutoConnectExecutor Executor { get; } = new();
        internal ZLinkLocationRuntime Runtime { get; }
        internal ZLinkAutoConnectReconciler Reconciler { get; }
        internal ZLinkAutoConnectLoop Loop { get; }

        private PublicationFixture(TimeSpan timeout)
        {
            Store = new ControlledPublicationStore(Time);
            Options = new ZLinkLocationOptions
            {
                OwnerLeaseRenewTimeout = timeout,
                PollingInterval = TimeSpan.FromHours(1),
            };
            Runtime = new ZLinkLocationRuntime(Options, Store, Time);
            var local = new ZLinkAutoConnectLocal(
                ZLinkLocationAutoConnectType.ClientServer,
                Mesh("play"),
                ZLinkLocationRole.Dealer,
                RoutingId.From("local"),
                "tcp://l:1"
            );
            Reconciler = new ZLinkAutoConnectReconciler(
                local,
                InMemoryLocationStoreTests.MeshNode("ignored", "tcp://l:1", "local"),
                Runtime,
                Peers,
                Executor,
                Options,
                Time
            );
            Loop = new ZLinkAutoConnectLoop(
                new ZLinkRuntimeErrorSink(),
                Reconciler,
                local,
                Options,
                Store,
                timeProvider: Time
            );
        }

        internal static async Task<PublicationFixture> CreateAsync(TimeSpan timeout)
        {
            var fixture = new PublicationFixture(timeout);
            Assert.True(await fixture.Runtime.RenewOwnerLeaseOnceAsync());
            return fixture;
        }

        public async ValueTask DisposeAsync()
        {
            foreach (var pending in Store.Pending)
                pending.Complete();
            await Loop.DisposeAsync();
            await Runtime.DisposeAsync();
        }
    }

    private sealed class OrderedPeerResolver : IZLinkMeshNodeLocationResolver
    {
        internal int ListCalls { get; private set; }

        public ValueTask<IReadOnlyList<ZLinkMeshNodeDescriptor>> ListLiveMeshNodesAsync(
            string meshName,
            CancellationToken cancellationToken = default
        )
        {
            var index = ++ListCalls;
            return ValueTask.FromResult<IReadOnlyList<ZLinkMeshNodeDescriptor>>([
                InMemoryLocationStoreTests.MeshNode("peer", $"tcp://r:{index}", $"r{index}"),
            ]);
        }
    }

    private sealed class PendingPublication(ZLinkMeshNodeDescriptor row, CancellationToken token)
    {
        internal ZLinkMeshNodeDescriptor Row { get; } = row;
        internal CancellationToken Token { get; } = token;
        internal TaskCompletionSource<ZLinkLocationWriteResult> Result { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        internal TaskCompletionSource Returned { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        internal void Complete() =>
            Result.TrySetResult(
                new ZLinkLocationWriteResult(ZLinkLocationWriteStatus.Stored, 1, default)
            );

        internal async ValueTask<ZLinkLocationWriteResult> WaitAsync()
        {
            try
            {
                // Deliberately ignore cancellation, like an unresponsive Store provider.
                return await Result.Task;
            }
            finally
            {
                Returned.TrySetResult();
            }
        }
    }

    private sealed class ControlledPublicationStore(TimeProvider time)
        : ZLinkLocationStoreTestDouble
    {
        private readonly ZLinkInMemoryLocationStore _inner = new(time);
        private readonly System.Threading.Channels.Channel<PendingPublication> _writes =
            System.Threading.Channels.Channel.CreateUnbounded<PendingPublication>();
        internal System.Collections.Concurrent.ConcurrentBag<PendingPublication> Pending { get; } =
        [];
        internal bool HoldWrites { get; set; }
        internal int WriteCalls { get; private set; }

        internal Task<PendingPublication> NextWriteAsync() =>
            _writes.Reader.ReadAsync().AsTask().WaitAsync(TimeSpan.FromSeconds(1));

        public override ValueTask<ZLinkLocationWriteResult> UpdateMeshNodeAsync(
            ZLinkMeshNodeDescriptor descriptor,
            ZLinkLocationWriteIntent intent,
            CancellationToken cancellationToken = default
        )
        {
            WriteCalls++;
            if (!HoldWrites)
                return ValueTask.FromResult(
                    new ZLinkLocationWriteResult(ZLinkLocationWriteStatus.Stored, 1, default)
                );
            var pending = new PendingPublication(descriptor, cancellationToken);
            Pending.Add(pending);
            _writes.Writer.TryWrite(pending);
            return pending.WaitAsync();
        }

        public override ValueTask<ulong?> GetMeshNodeChangeStampAsync(
            string meshName,
            CancellationToken cancellationToken = default
        ) => ValueTask.FromResult<ulong?>(null);

        public override ValueTask<ZLinkLocationWriteStatus> RemoveMeshNodeAsync(
            ZLinkMeshNodeDescriptorKey key,
            ZLinkLocationOwnerToken owner,
            CancellationToken cancellationToken = default
        ) => ValueTask.FromResult(ZLinkLocationWriteStatus.Stored);

        public override ValueTask<ZLinkOwnerLeaseClaimResult> ClaimOwnerLeaseAsync(
            string ownerId,
            TimeSpan leaseTtl,
            CancellationToken cancellationToken = default
        ) => _inner.ClaimOwnerLeaseAsync(ownerId, leaseTtl, cancellationToken);

        public override ValueTask<ZLinkOwnerLeaseReleaseResult> ReleaseOwnerLeaseAsync(
            ZLinkLocationOwnerToken token,
            CancellationToken cancellationToken = default
        ) => _inner.ReleaseOwnerLeaseAsync(token, cancellationToken);

        public override ValueTask<long> RemoveAllByOwnerAsync(
            ZLinkLocationOwnerToken owner,
            CancellationToken cancellationToken = default
        ) => ValueTask.FromResult(0L);
    }

    private sealed class CountingPeerResolver(IZLinkMeshNodeLocationResolver inner)
        : IZLinkMeshNodeLocationResolver
    {
        public int ListCalls { get; private set; }

        public ValueTask<IReadOnlyList<ZLinkMeshNodeDescriptor>> ListLiveMeshNodesAsync(
            string meshName,
            CancellationToken cancellationToken = default
        )
        {
            ListCalls++;
            return inner.ListLiveMeshNodesAsync(meshName, cancellationToken);
        }
    }

    private sealed class SwitchablePeerResolver(IReadOnlyList<ZLinkMeshNodeDescriptor> rows)
        : IZLinkMeshNodeLocationResolver
    {
        public IReadOnlyList<ZLinkMeshNodeDescriptor> Rows { get; set; } = rows;

        public ValueTask<IReadOnlyList<ZLinkMeshNodeDescriptor>> ListLiveMeshNodesAsync(
            string meshName,
            CancellationToken cancellationToken = default
        ) => ValueTask.FromResult(Rows);
    }

    private sealed class FailingStampStore : ZLinkLocationStoreTestDouble
    {
        public bool FailNext { get; set; }

        public override ValueTask<ulong?> GetMeshNodeChangeStampAsync(
            string meshName,
            CancellationToken cancellationToken = default
        )
        {
            if (!FailNext)
                return ValueTask.FromResult<ulong?>(1);

            FailNext = false;
            return ValueTask.FromException<ulong?>(
                new InvalidOperationException("stamp unavailable")
            );
        }
    }

    private sealed class NullExecutor : IZLinkAutoConnectExecutor
    {
        public bool Connect(ZLinkAutoConnectTarget target) => true;

        public bool Disconnect(ZLinkAutoConnectTarget target) => true;
    }

    private sealed class RetryExecutor : IZLinkAutoConnectExecutor
    {
        public int ConnectCalls { get; private set; }

        public bool Connect(ZLinkAutoConnectTarget target) => ++ConnectCalls >= 2;

        public bool Disconnect(ZLinkAutoConnectTarget target) => true;
    }
}
