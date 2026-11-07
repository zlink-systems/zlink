using System.Collections.Concurrent;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Contracts.Timers;
using Zlink.Framework.Runtime.Host;

namespace Zlink.Framework.UnitTests;

public sealed class SpotShutdownExecutionTests
{
    [Theory]
    [InlineData(0)]
    [InlineData(1)]
    [InlineData(2)]
    [InlineData(3)]
    [InlineData(4)]
    public async Task Finalized_activation_accepts_stop_requests_from_stale_catalog_snapshot(
        int operation
    )
    {
        using var host = new HostBuilder()
            .ConfigureServices(services =>
                services.AddZLinkFramework(options =>
                {
                    options.UseTestLocationStore();
                    options
                        .AddRouteMesh("finalized-stop")
                        .Listen("tcp://127.0.0.1:0")
                        .Objects()
                        .Server()
                        .AddSpotFactory<FinalizedSpot>(
                            "target",
                            factory => factory.DisableRelocation()
                        );
                })
            )
            .Build();
        await host.StartAsync();
        var runtime = host.Services.GetRequiredService<ZLinkFrameworkRuntime>();
        await host
            .Services.GetRequiredService<IZLinkSpotManager>()
            .GetOrCreate("finalized-target", "target")
            .InMesh("finalized-stop")
            .Async();
        var node = runtime.GetSpotNodeRuntime("finalized-stop");
        var activation = Assert.Single(node.Spots);
        var token = activation.StopToken;
        var cancellationRequested = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var releaseMembers = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var callbacks = 0;
        using var registration = token.Register(() =>
        {
            try
            {
                Assert.False(activation._lane.IsOnLane);
                activation.RequestStop();
                Interlocked.Increment(ref callbacks);
            }
            finally
            {
                cancellationRequested.TrySetResult();
            }
        });
        try
        {
            var finalization = activation.DiscardLocationExecutionAsync(() =>
                new ValueTask(releaseMembers.Task)
            );
            await cancellationRequested.Task;
            Assert.False(finalization.IsCompleted);
            activation.RequestStop();
            activation.CancelActiveOperations();
            releaseMembers.TrySetResult();
            // Await CTS disposal while retaining catalog membership to model a
            // previously taken snapshot. No clock controls this ordering.
            await finalization;
            Assert.Throws<ObjectDisposedException>(() =>
            {
                _ = activation.StopToken;
            });
            Assert.Equal(1, callbacks);
            Assert.Same(activation, Assert.Single(node.Spots));
            Assert.True(token.IsCancellationRequested);
            Assert.True(activation.IsDisposed);
            switch (operation)
            {
                case 0:
                    activation.CancelActiveOperations();
                    break;
                case 1:
                    activation.RequestStop();
                    break;
                case 2:
                    node.CancelActiveOperations();
                    break;
                case 3:
                    node.RequestStop();
                    break;
                case 4:
                    await runtime.ForceStopAsync(CancellationToken.None);
                    break;
            }
            Assert.True(token.IsCancellationRequested);
            Assert.Equal(1, callbacks);
        }
        finally
        {
            releaseMembers.TrySetResult();
            await runtime.DiscardLocationExecutionAsync(activation);
            await runtime.ForceStopAsync(CancellationToken.None);
        }
    }

    public sealed class FinalizedSpot(IZLinkSpotContext context) : IZLinkSpot
    {
        public IZLinkSpotContext Context { get; } = context;
    }

    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public async Task Shutdown_returns_while_a_spot_waits_for_a_response(bool complete)
    {
        var probe = new Probe();
        using var host = new HostBuilder()
            .ConfigureServices(services =>
            {
                services.AddSingleton(probe);
                services.AddZLinkFramework(options =>
                {
                    options.UseTestLocationStore();
                    options
                        .AddRouteMesh("shutdown-execution")
                        .Listen("tcp://127.0.0.1:0")
                        .Objects()
                        .Server()
                        .AddSpotFactory<Target>("target", factory => factory.DisableRelocation())
                        .AddSpotFactory<Source>("source", factory => factory.DisableRelocation());
                });
            })
            .Build();
        await host.StartAsync();
        var runtime = host.Services.GetRequiredService<IZLinkFrameworkRuntime>();
        var spots = host.Services.GetRequiredService<IZLinkSpotManager>();
        Task<ZLinkFrameworkTerminationResult>? shutdown = null;
        var returned = false;
        try
        {
            probe.Target = (await spots.Create("target").InMesh("shutdown-execution").Async())
                .Spot
                .SpotId;
            await spots.Create("source").InMesh("shutdown-execution").Async();
            await probe.Entered.Task.WaitAsync(TimeSpan.FromSeconds(5));
            if (complete)
            {
                probe.Release.TrySetResult("reply");
                await probe.Completed.Task.WaitAsync(TimeSpan.FromSeconds(5));
            }
            shutdown = (
                complete
                    ? runtime.ShutdownAsync()
                    : runtime.ShutdownAsync(TimeSpan.FromMilliseconds(50))
            ).AsTask();
            try
            {
                await shutdown.WaitAsync(TimeSpan.FromSeconds(5));
                returned = true;
            }
            catch (TimeoutException) { }
            if (returned)
            {
                if (!complete)
                {
                    Assert.Equal(new[] { "target-started" }, probe.Events.ToArray());
                    probe.Release.TrySetResult("late reply");
                    probe.Release.TrySetResult("duplicate reply");
                }
                await probe.Disposed.Task.WaitAsync(TimeSpan.FromSeconds(5));
                await probe.TimerCancelled!.WaitAsync(TimeSpan.FromSeconds(5));
                Assert.Equal(
                    new[] { "target-started", "target-ended", "target-closing", "target-disposed" },
                    probe.Events.ToArray()
                );
                Assert.Equal(1, probe.RequestTerminals);
            }
        }
        finally
        {
            probe.Release.TrySetResult("late reply");
            if (shutdown is not null)
                await shutdown.WaitAsync(TimeSpan.FromSeconds(10));
            await host.StopAsync();
        }
        Assert.True(returned, "Shutdown must return before the application releases its response.");
        var result = await shutdown!;
        Assert.Equal(
            complete
                ? ZLinkFrameworkTerminationOutcome.Stopped
                : ZLinkFrameworkTerminationOutcome.ForceStopped,
            result.Outcome
        );
        Assert.Equal(
            complete
                ? ZLinkFrameworkTerminationReason.None
                : ZLinkFrameworkTerminationReason.DeadlineExceeded,
            result.Reason
        );
    }

    public sealed class Probe
    {
        public string Target = "";
        public TaskCompletionSource Entered { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource<string> Release { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource Completed { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource Disposed { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public ConcurrentQueue<string> Events { get; } = new();
        public int RequestTerminals;
        public Task? TimerCancelled;
    }

    public sealed record Hold(string Value);

    public sealed class Target(IZLinkSpotContext context, Probe probe) : IZLinkSpot
    {
        public IZLinkSpotContext Context { get; } = context;

        public ValueTask OnClosingAsync(
            ZLinkSpotClosingContext context,
            CancellationToken cancellationToken
        )
        {
            Assert.Equal("target-ended", probe.Events.Last());
            probe.Events.Enqueue("target-closing");
            return ValueTask.CompletedTask;
        }
    }

    public sealed class Source(IZLinkSpotContext context) : IZLinkSpot
    {
        public IZLinkSpotContext Context { get; } = context;
        public IZLinkTimer Timer { get; private set; } = null!;

        public async ValueTask OnInitializeAsync(CancellationToken cancellationToken)
        {
            Timer = await Context.AddTimer<RequestTimer>(
                "request",
                TimeSpan.FromMilliseconds(10),
                cancellationToken: cancellationToken
            );
        }
    }

    public sealed class HoldHandler(Probe probe)
        : IZLinkSpotRequestHandler<Target, Hold, string>,
            IAsyncDisposable
    {
        public async ValueTask<string> HandleAsync(
            Target spot,
            Hold request,
            CancellationToken cancellationToken
        )
        {
            probe.Events.Enqueue("target-started");
            probe.Entered.TrySetResult();
            var reply = await probe.Release.Task;
            probe.Events.Enqueue("target-ended");
            return reply;
        }

        public ValueTask DisposeAsync()
        {
            Assert.Equal("target-closing", probe.Events.Last());
            probe.Events.Enqueue("target-disposed");
            probe.Disposed.TrySetResult();
            return ValueTask.CompletedTask;
        }
    }

    public sealed class RequestTimer(Probe probe) : IZLinkSpotTimerHandler<Source>
    {
        public async ValueTask HandleAsync(
            Source spot,
            ZLinkTimerTick tick,
            CancellationToken cancellationToken
        )
        {
            probe.TimerCancelled = spot.Timer.CancelAsync().AsTask();
            try
            {
                await spot
                    .Context.Outbound.RequestToSpot(probe.Target, new Hold("request"))
                    .Async<string>(cancellationToken);
            }
            finally
            {
                Interlocked.Increment(ref probe.RequestTerminals);
                probe.Completed.TrySetResult();
            }
        }
    }
}
