using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests;

public sealed class ActivationReadyTests
{
    private static readonly TimeSpan Timeout = TimeSpan.FromSeconds(10);

    [Theory]
    [InlineData(ZLinkUserSpotExecutionMode.SpotWide, false)]
    [InlineData(ZLinkUserSpotExecutionMode.PerActor, false)]
    [InlineData(ZLinkUserSpotExecutionMode.SpotWide, true)]
    [InlineData(ZLinkUserSpotExecutionMode.PerActor, true)]
    public async Task User_timer_due_during_ready_commit_runs_after_publication_and_closes(
        ZLinkUserSpotExecutionMode mode,
        bool replaceGeneration
    )
    {
        var probe = new ReadyProbe(-1, false) { RegisterTimer = true };
        var store = new ReadyStore(probe);
        var services = new ServiceCollection().AddSingleton(probe);
        services.AddZLinkFramework(options =>
        {
            options.DisableImplicitHandlerAutoRegistration();
            options.AddLocationStore(store);
            options.AddRelocationStore(new InMemoryRelocationStore());
            options
                .AddRouteMesh("ready-probe")
                .Listen("tcp://127.0.0.1:0")
                .Objects()
                .Server()
                .AddSpotFactory<ReadySpot>(
                    "ready-probe",
                    factory => factory.ExecutionMode(mode).RecreateOnRelocation()
                );
        });
        await using var provider = services.BuildServiceProvider();
        var hosted = provider
            .GetServices<IHostedService>()
            .Single(service => service is ZLinkFrameworkHostedService);
        await hosted.StartAsync(CancellationToken.None);
        try
        {
            var creation = provider
                .GetRequiredService<IZLinkSpotManager>()
                .GetOrCreate("ready-probe", "ready-probe")
                .InMesh("ready-probe")
                .Async()
                .AsTask();
            await store.Held.Task.WaitAsync(Timeout);
            var timer = Assert.IsType<ZLinkTimer>(probe.Timer);
            await timer.NotifyDueAsync(1);
            var executor = probe.Activation!._serial;
            using var deadline = new CancellationTokenSource(Timeout);
            while (executor.PendingApplicationCount == 0 && !probe.Tick.Task.IsCompleted)
            {
                deadline.Token.ThrowIfCancellationRequested();
                await Task.Yield();
            }
            await executor.RunIngressAsync(() => { });
            Assert.False(probe.Tick.Task.IsCompleted);
            if (replaceGeneration)
            {
                var replacement = await probe.Activation!.AddTimer<ReadyReplacementTick>(
                    "ready",
                    TimeSpan.FromHours(1)
                );
                Assert.True(timer.IsDisposed);
                await Assert.IsType<ZLinkTimer>(replacement).NotifyDueAsync(1);
                await executor.RunIngressAsync(() => { });
                Assert.False(probe.Tick.Task.IsCompleted);
            }
            store.Release.TrySetResult();
            await creation.WaitAsync(Timeout);
            var close = await probe.Tick.Task.WaitAsync(Timeout);
            Assert.True(await close.WaitAsync(Timeout));
            Assert.Equal(replaceGeneration ? 0 : 1, probe.OriginalTicks);
            Assert.Equal(replaceGeneration ? 1 : 0, probe.ReplacementTicks);
        }
        finally
        {
            store.Release.TrySetResult();
            await hosted.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task Instance_initialization_close_waits_for_handler_and_durable_replay_cursor()
    {
        await using var host = await Runtime.SpotCloseHost.StartAsync();
        const string spotId = "initializing-instance-close";
        host.Store.HoldReplayCursorFor = spotId;
        var handlerEntered = Signal();
        var releaseHandler = Signal();
        host.State.OnInstanceInitializing = context =>
            host.State.ContextCloseTask = context.CloseAsync().AsTask();
        host.State.OnInstanceHandler = async ct =>
        {
            handlerEntered.TrySetResult();
            await releaseHandler.Task.WaitAsync(ct);
        };
        var request = host.RequestInstanceAsync(spotId);
        try
        {
            await handlerEntered.Task.WaitAsync(Timeout);
            Assert.False(host.State.ContextCloseTask!.IsCompleted);
            Assert.False(host.State.OnClosingEntered.Task.IsCompleted);
            releaseHandler.TrySetResult();
            await host.Store.ReplayCursorHeld.Task.WaitAsync(Timeout);
            Assert.False(host.State.ContextCloseTask.IsCompleted);
            Assert.False(host.State.OnClosingEntered.Task.IsCompleted);
        }
        finally
        {
            releaseHandler.TrySetResult();
            host.Store.ReleaseReplayCursor.TrySetResult();
        }
        await request.WaitAsync(Timeout);
        Assert.True(await host.State.ContextCloseTask!.WaitAsync(Timeout));
        Assert.Equal("Missing", await host.AuthorityAsync(spotId));
    }

    [Theory]
    [InlineData(ZLinkUserSpotExecutionMode.SpotWide, ZLinkTimerOverrunPolicy.SkipLateTicks, false)]
    [InlineData(ZLinkUserSpotExecutionMode.PerActor, ZLinkTimerOverrunPolicy.SkipLateTicks, false)]
    [InlineData(ZLinkUserSpotExecutionMode.SpotWide, ZLinkTimerOverrunPolicy.CatchUpBounded, false)]
    [InlineData(ZLinkUserSpotExecutionMode.PerActor, ZLinkTimerOverrunPolicy.CatchUpBounded, false)]
    [InlineData(ZLinkUserSpotExecutionMode.SpotWide, ZLinkTimerOverrunPolicy.DelayNextTick, false)]
    [InlineData(ZLinkUserSpotExecutionMode.PerActor, ZLinkTimerOverrunPolicy.DelayNextTick, false)]
    [InlineData(ZLinkUserSpotExecutionMode.SpotWide, ZLinkTimerOverrunPolicy.SkipLateTicks, true)]
    [InlineData(ZLinkUserSpotExecutionMode.PerActor, ZLinkTimerOverrunPolicy.SkipLateTicks, true)]
    public async Task Due_tick_waits_in_existing_gate_and_cancelled_generation_never_runs(
        ZLinkUserSpotExecutionMode mode,
        ZLinkTimerOverrunPolicy policy,
        bool cancel
    )
    {
        using var errors = new ZLinkRuntimeErrorSink();
        await using var executor = new ZLinkSpotSerialExecutor(
            null!,
            () => false,
            CancellationToken.None,
            errors,
            executionMode: mode
        );
        var published = Signal();
        await executor.PrepareActivationAsync(
            _ => ValueTask.CompletedTask,
            () => published.Task.IsCompleted
        );
        var time = new ReadyTime();
        await using var scheduler = new ZLinkTimerScheduler(time);
        var period = TimeSpan.FromMilliseconds(100);
        var delivered = new TaskCompletionSource<ZLinkTimerTick>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var calls = 0;
        await using var timer = new ZLinkTimer(
            new ZLinkTimerLogicalSnapshot(
                "ready",
                period,
                new ZLinkTimerOptions { OverrunPolicy = policy, MaxCatchUpTicks = 2 },
                time.GetUtcNow(),
                0,
                0,
                null,
                null
            ),
            CancellationToken.None,
            async (tick, ct) =>
            {
                await executor.ExecuteTimerAsync(
                    "ready",
                    (_, _, _) =>
                    {
                        Interlocked.Increment(ref calls);
                        delivered.TrySetResult(tick);
                        return ValueTask.CompletedTask;
                    },
                    0,
                    ct
                );
                return true;
            },
            (_, failure, _, _) => ValueTask.FromException(failure),
            scheduler: scheduler
        );
        const long initialScheduleVersion = 1;
        time.Advance(TimeSpan.FromTicks(period.Ticks * 5));
        await timer.NotifyDueAsync(initialScheduleVersion);
        using var pendingDeadline = new CancellationTokenSource(Timeout);
        while (executor.PendingApplicationCount == 0)
        {
            pendingDeadline.Token.ThrowIfCancellationRequested();
            await Task.Yield();
        }
        await executor.RunIngressAsync(() => { });
        Assert.Equal(0, Volatile.Read(ref calls));
        Assert.NotNull(timer.Snapshot().PendingTick);
        var close = executor.PostLifecycleOperation((_, _) => ValueTask.FromResult(true))!;
        Assert.False(close.IsCompleted);
        if (cancel)
            await timer.CancelAsync();
        published.TrySetResult();
        await executor.FinishActivationPreparation().WaitAsync(Timeout);
        Assert.True(await close.WaitAsync(Timeout));
        if (cancel)
        {
            await executor.RunIngressAsync(() => { });
            Assert.Equal(0, Volatile.Read(ref calls));
        }
        else
        {
            var tick = await delivered.Task.WaitAsync(Timeout);
            Assert.Equal(1UL, tick.DeliveryIndex);
            Assert.Equal(
                policy == ZLinkTimerOverrunPolicy.SkipLateTicks ? 5UL
                    : policy == ZLinkTimerOverrunPolicy.CatchUpBounded ? 4UL
                    : 1UL,
                tick.ScheduledIndex
            );
            Assert.Equal(tick.ScheduledIndex - 1, tick.SkippedTicks);
        }
    }

    [Theory]
    [InlineData(ZLinkUserSpotExecutionMode.SpotWide)]
    [InlineData(ZLinkUserSpotExecutionMode.PerActor)]
    public async Task Target_staging_seal_is_the_only_preparation_owner(
        ZLinkUserSpotExecutionMode mode
    )
    {
        using var errors = new ZLinkRuntimeErrorSink();
        await using var executor = new ZLinkSpotSerialExecutor(
            null!,
            () => false,
            CancellationToken.None,
            errors,
            executionMode: mode
        );
        await executor.BeginTargetPreparationAsync();
        var close = executor.PostLifecycleOperation((_, _) => ValueTask.FromResult(true))!;
        var seal = await executor.TrySealRelocationAsync();
        Assert.NotNull(seal);
        await executor.ExecuteTargetPreparationAsync(
            (_, _) =>
            {
                Assert.False(close.IsCompleted);
                return ValueTask.CompletedTask;
            },
            CancellationToken.None
        );
        Assert.True(await executor.TryAbortRelocationAsync(seal));
        Assert.True(await close.WaitAsync(Timeout));
    }

    private static TaskCompletionSource Signal() =>
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    private sealed class ReadyTime : TimeProvider
    {
        private long ticks;
        private readonly DateTimeOffset origin = DateTimeOffset.UtcNow;
        public override long TimestampFrequency => TimeSpan.TicksPerSecond;

        public override long GetTimestamp() => Interlocked.Read(ref ticks);

        public override DateTimeOffset GetUtcNow() => origin.AddTicks(GetTimestamp());

        internal void Advance(TimeSpan value) => Interlocked.Add(ref ticks, value.Ticks);
    }

    [Theory]
    [InlineData(ZLinkUserSpotExecutionMode.SpotWide, 0, false)]
    [InlineData(ZLinkUserSpotExecutionMode.SpotWide, 1, false)]
    [InlineData(ZLinkUserSpotExecutionMode.SpotWide, 2, false)]
    [InlineData(ZLinkUserSpotExecutionMode.PerActor, 0, false)]
    [InlineData(ZLinkUserSpotExecutionMode.PerActor, 1, false)]
    [InlineData(ZLinkUserSpotExecutionMode.PerActor, 2, false)]
    [InlineData(ZLinkUserSpotExecutionMode.SpotWide, 0, true)]
    [InlineData(ZLinkUserSpotExecutionMode.SpotWide, 1, true)]
    [InlineData(ZLinkUserSpotExecutionMode.SpotWide, 2, true)]
    [InlineData(ZLinkUserSpotExecutionMode.PerActor, 0, true)]
    [InlineData(ZLinkUserSpotExecutionMode.PerActor, 1, true)]
    [InlineData(ZLinkUserSpotExecutionMode.PerActor, 2, true)]
    public async Task Initialization_close_finishes_after_creation_or_returns_false_on_failure(
        ZLinkUserSpotExecutionMode mode,
        int phase,
        bool fail
    )
    {
        await CheckInitializationClose(mode, phase, fail, false, false);
    }

    [Theory]
    [InlineData(ZLinkUserSpotExecutionMode.SpotWide, true)]
    [InlineData(ZLinkUserSpotExecutionMode.PerActor, true)]
    [InlineData(ZLinkUserSpotExecutionMode.SpotWide, false)]
    [InlineData(ZLinkUserSpotExecutionMode.PerActor, false)]
    public Task Rejected_creation_or_ready_store_failure_discards_initialization_close(
        ZLinkUserSpotExecutionMode mode,
        bool rejected
    ) => CheckInitializationClose(mode, rejected ? 1 : 2, false, rejected, !rejected);

    private static async Task CheckInitializationClose(
        ZLinkUserSpotExecutionMode mode,
        int phase,
        bool fail,
        bool rejected,
        bool failStore
    )
    {
        var probe = new ReadyProbe(phase, fail)
        {
            RejectCreation = rejected,
            FailCommit = failStore,
            RegisterTimerInCreate = fail || rejected || failStore,
            HoldCommit = !fail && !rejected && !failStore,
        };
        var store = new ReadyStore(probe);
        var services = new ServiceCollection();
        services.AddSingleton(probe);
        services.AddZLinkFramework(options =>
        {
            options.DisableImplicitHandlerAutoRegistration();
            options.AddLocationStore(store);
            options.AddRelocationStore(new InMemoryRelocationStore());
            options
                .AddRouteMesh("ready-probe")
                .Listen("tcp://127.0.0.1:0")
                .Objects()
                .Server()
                .AddSpotFactory<ReadySpot>(
                    "ready-probe",
                    factory => factory.ExecutionMode(mode).RecreateOnRelocation()
                );
        });
        await using var provider = services.BuildServiceProvider();
        var hosted = provider
            .GetServices<IHostedService>()
            .Single(service => service is ZLinkFrameworkHostedService);
        await hosted.StartAsync(CancellationToken.None);
        try
        {
            var creation = provider
                .GetRequiredService<IZLinkSpotManager>()
                .GetOrCreate("ready-probe", "ready-probe")
                .InMesh("ready-probe")
                .Async()
                .AsTask();
            if (fail || failStore)
                await Assert.ThrowsAnyAsync<Exception>(() => creation.WaitAsync(Timeout));
            else if (rejected)
                await creation.WaitAsync(Timeout);
            else
            {
                await store.Held.Task.WaitAsync(Timeout);
                await probe.Activation!._serial.RunIngressAsync(() => { });
                Assert.NotNull(probe.Close);
                Assert.False(probe.Close.IsCompleted);
                store.Release.TrySetResult();
                await creation.WaitAsync(Timeout);
            }
            Assert.NotNull(probe.Close);
            Assert.Equal(!fail && !rejected && !failStore, await probe.Close.WaitAsync(Timeout));
            if (probe.RegisterTimerInCreate)
            {
                Assert.True(probe.Timer!.IsDisposed);
                Assert.False(probe.Tick.Task.IsCompleted);
            }
        }
        finally
        {
            store.Release.TrySetResult();
            await hosted.StopAsync(CancellationToken.None);
        }
    }

    private sealed class ReadyProbe(int phase, bool fail)
    {
        internal Task<bool>? Close;
        internal bool RegisterTimer;
        internal bool RegisterTimerInCreate;
        internal bool HoldCommit;
        internal bool RejectCreation;
        internal bool FailCommit;
        internal bool Initialized;
        internal int OriginalTicks;
        internal int ReplacementTicks;
        internal IZLinkTimer? Timer;
        internal ZLinkSpotActivation? Activation;
        internal TaskCompletionSource<Task<bool>> Tick { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        internal void Visit(IZLinkSpotContext context, int current)
        {
            Activation = (ZLinkSpotActivation)context;
            if (current == phase)
                Close = context.CloseAsync().AsTask();
            if (current == 2 && fail)
                throw new InvalidOperationException("ready-probe initialization failed");
        }
    }

    private sealed class ReadySpot(IZLinkSpotContext context, ReadyProbe probe) : IZLinkSpot
    {
        public IZLinkSpotContext Context { get; } = context;

        public void Configure() => probe.Visit(Context, 0);

        public async ValueTask<ZLinkSpotCreateResponse> OnCreateAsync(
            ZLinkMessage request,
            CancellationToken ct
        )
        {
            if (probe.RegisterTimerInCreate)
            {
                probe.Timer = await Context.AddTimer<ReadyTick>(
                    "ready",
                    TimeSpan.FromHours(1),
                    cancellationToken: ct
                );
                await Assert.IsType<ZLinkTimer>(probe.Timer).NotifyDueAsync(1);
            }
            probe.Visit(Context, 1);
            return probe.RejectCreation
                ? ZLinkSpotCreateResponse.Reject()
                : ZLinkSpotCreateResponse.Accept();
        }

        public async ValueTask OnInitializeAsync(CancellationToken ct)
        {
            probe.Visit(Context, 2);
            if (probe.RegisterTimer)
            {
                probe.Activation = (ZLinkSpotActivation)Context;
                probe.Timer = await Context.AddTimer<ReadyTick>(
                    "ready",
                    TimeSpan.FromHours(1),
                    cancellationToken: ct
                );
            }
            probe.Initialized = true;
        }

        internal void Tick(bool replacement = false)
        {
            if (replacement)
                Interlocked.Increment(ref probe.ReplacementTicks);
            else
                Interlocked.Increment(ref probe.OriginalTicks);
            probe.Tick.TrySetResult(Context.CloseAsync().AsTask());
        }
    }

    private sealed class ReadyTick : IZLinkSpotTimerHandler<ReadySpot>
    {
        public ValueTask HandleAsync(ReadySpot spot, ZLinkTimerTick tick, CancellationToken ct)
        {
            spot.Tick();
            return ValueTask.CompletedTask;
        }
    }

    private sealed class ReadyReplacementTick : IZLinkSpotTimerHandler<ReadySpot>
    {
        public ValueTask HandleAsync(ReadySpot spot, ZLinkTimerTick tick, CancellationToken ct)
        {
            spot.Tick(replacement: true);
            return ValueTask.CompletedTask;
        }
    }

    private sealed class ReadyStore(ReadyProbe probe) : IZLinkLocationStore
    {
        private readonly IZLinkLocationStore inner = new ZLinkInMemoryProviderLocationStore();
        internal TaskCompletionSource Held { get; } = Signal();
        internal TaskCompletionSource Release { get; } = Signal();

        public ValueTask<ZLinkStoreReadResult> ReadAsync(
            ZLinkStoreKey key,
            CancellationToken ct = default
        ) => inner.ReadAsync(key, ct);

        public ValueTask<ZLinkStoreScanResult> ScanAsync(
            ZLinkStoreScanRequest request,
            CancellationToken ct = default
        ) => inner.ScanAsync(request, ct);

        public async ValueTask<ZLinkStoreWriteResult> WriteAsync(
            ZLinkStoreWriteRequest request,
            CancellationToken ct = default
        )
        {
            if (
                probe.Initialized
                && probe.FailCommit
                && request
                    .Mutations.OfType<ZLinkStoreMutation.Put>()
                    .Any(put => put.Key.Value == "authority\0spot\0ready-probe")
            )
                throw new InvalidOperationException("ready-probe commit failed");
            if (
                probe.Initialized
                && (probe.RegisterTimer || probe.HoldCommit)
                && !Release.Task.IsCompleted
                && request
                    .Mutations.OfType<ZLinkStoreMutation.Put>()
                    .Any(put => put.Key.Value == "authority\0spot\0ready-probe")
            )
            {
                Held.TrySetResult();
                await Release.Task.WaitAsync(ct);
            }
            return await inner.WriteAsync(request, ct);
        }
    }
}
