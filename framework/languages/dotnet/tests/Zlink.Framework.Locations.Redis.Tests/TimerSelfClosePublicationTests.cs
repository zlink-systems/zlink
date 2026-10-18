using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Timers;

namespace Zlink.Framework.Locations.Redis.Tests;

[Collection(RedisTestCollection.Name)]
public sealed class TimerSelfClosePublicationTests(RedisTestFixture redis)
{
    private static readonly TimeSpan Deadline = TimeSpan.FromSeconds(10);
    private static readonly TimeSpan TimerPeriod = TimeSpan.FromMilliseconds(100);
    private static readonly TimeSpan InitializationDuration = TimeSpan.FromMilliseconds(150);
    private static readonly TimeSpan PublicationObservationWindow = TimeSpan.FromMilliseconds(250);

    [Theory]
    [InlineData(true, true)]
    [InlineData(true, false)]
    [InlineData(false, true)]
    public async Task Initialization_timer_close_completes_after_publication(
        bool selfClose,
        bool dueDuringInitialization
    )
    {
        Assert.True(redis.RedisAvailable, redis.SkipReason);
        var probe = new Probe(selfClose, dueDuringInitialization);
        var store = new PublicationStore(redis.CreateStore(), probe);
        var services = new ServiceCollection();
        services.AddSingleton(probe);
        services.AddZLinkFramework(options =>
        {
            options.DisableImplicitHandlerAutoRegistration();
            options.ConfigureDispatch().Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
            options.AddLocationStore(store);
            options
                .AddRouteMesh("timer-close")
                .Listen("tcp://127.0.0.1:0")
                .Objects()
                .Server()
                .AddSpotFactory<ProbeSpot>(
                    "probe",
                    factory =>
                        factory
                            .ExecutionMode(ZLinkUserSpotExecutionMode.SpotWide)
                            .DisableRelocation()
                );
        });
        await using var provider = services.BuildServiceProvider();
        var hosted = provider.GetServices<IHostedService>().ToArray();
        foreach (var service in hosted)
            await service.StartAsync(CancellationToken.None);
        try
        {
            var manager = provider.GetRequiredService<IZLinkSpotManager>();
            var creation = manager
                .GetOrCreate(probe.SpotId, "probe")
                .InMesh("timer-close")
                .Async()
                .AsTask();
            if (dueDuringInitialization)
            {
                try
                {
                    await store.Held.Task.WaitAsync(Deadline);
                    await Task.WhenAny(
                        probe.Returned.Task,
                        Task.Delay(PublicationObservationWindow)
                    );
                    if (probe.Close.Task.IsCompletedSuccessfully)
                        await Task.WhenAny(
                            await probe.Close.Task,
                            Task.Delay(PublicationObservationWindow)
                        );
                }
                finally
                {
                    store.Release.TrySetResult();
                }
            }
            var created = await creation.WaitAsync(Deadline);
            await probe.Returned.Task.WaitAsync(Deadline);
            if (selfClose)
            {
                var close = await probe.Close.Task.WaitAsync(Deadline);
                Assert.True(await close.WaitAsync(Deadline));
            }
            else
                Assert.True(await manager.CloseAsync(created.Spot));
            Assert.True(probe.Closing.Task.IsCompletedSuccessfully);
            Assert.True(probe.Timer!.IsDisposed);
            Assert.False(probe.TickBeforeInitializeReturned);
        }
        finally
        {
            foreach (var service in hosted.Reverse())
                await service.StopAsync(CancellationToken.None);
        }
    }

    public sealed class Probe(bool selfClose, bool dueDuringInitialization)
    {
        public bool SelfClose => selfClose;
        public string SpotId { get; } = $"probe-{Guid.NewGuid():N}";
        public bool DueDuringInitialization => dueDuringInitialization;
        public IZLinkTimer? Timer;
        public bool Initialized;
        public bool TickBeforeInitializeReturned;
        public TaskCompletionSource Returned { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource Closing { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource<Task<bool>> Close { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
    }

    private sealed class PublicationStore(IZLinkLocationStore inner, Probe probe)
        : IZLinkLocationStore
    {
        public TaskCompletionSource Held { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource Release { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

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
                probe.DueDuringInitialization
                && probe.Initialized
                && !Release.Task.IsCompleted
                && request
                    .Mutations.OfType<ZLinkStoreMutation.Put>()
                    .Any(put => put.Key.Value == $"authority\0spot\0{probe.SpotId}")
            )
            {
                Held.TrySetResult();
                await Release.Task.WaitAsync(ct);
            }
            return await inner.WriteAsync(request, ct);
        }
    }

    public sealed class ProbeSpot(IZLinkSpotContext context, Probe probe) : IZLinkSpot
    {
        public IZLinkSpotContext Context { get; } = context;
        public Probe Probe => probe;

        public void Configure() { }

        public async ValueTask OnInitializeAsync(CancellationToken ct)
        {
            probe.Timer = await Context.AddTimer<ProbeTick>(
                "close",
                TimerPeriod,
                new ZLinkTimerOptions { OverrunPolicy = ZLinkTimerOverrunPolicy.SkipLateTicks },
                ct
            );
            if (probe.DueDuringInitialization)
                await Task.Delay(InitializationDuration, ct);
            probe.Initialized = true;
        }

        public ValueTask OnClosingAsync(ZLinkSpotClosingContext closing, CancellationToken ct)
        {
            Assert.True(probe.Returned.Task.IsCompletedSuccessfully);
            probe.Closing.TrySetResult();
            return ValueTask.CompletedTask;
        }
    }

    public sealed class ProbeTick : IZLinkSpotTimerHandler<ProbeSpot>
    {
        public ValueTask HandleAsync(ProbeSpot spot, ZLinkTimerTick tick, CancellationToken ct)
        {
            var probe = spot.Probe;
            probe.TickBeforeInitializeReturned |= !probe.Initialized;
            if (probe.SelfClose && !probe.Close.Task.IsCompleted)
                probe.Close.TrySetResult(spot.Context.CloseAsync().AsTask());
            probe.Returned.TrySetResult();
            return ValueTask.CompletedTask;
        }
    }
}
