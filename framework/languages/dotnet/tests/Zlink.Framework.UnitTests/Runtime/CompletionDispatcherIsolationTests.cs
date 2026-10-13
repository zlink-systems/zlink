using System.Diagnostics;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Runtime.Backend.DotNet;

namespace Zlink.Framework.UnitTests;

public sealed class CompletionDispatcherIsolationTests
{
    private static readonly TimeSpan TestGuard = TimeSpan.FromSeconds(5);

    [Fact]
    public async Task CallerCanSynchronouslyWaitForAnotherTableRequest()
    {
        await using var fixture = await ActorRequests.StartAsync();
        var (first, second) = await fixture.SubmitAsync();
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var caller = CallerAsync();
        async Task<bool> CallerAsync()
        {
            _ = await first.ConfigureAwait(false);
            entered.SetResult();
            try
            {
                // The guard only releases a deadlocked test; it does not change
                // the request deadline or arrange its completion.
                return second.WaitAsync(TestGuard).GetAwaiter().GetResult().Number == 1;
            }
            catch (TimeoutException)
            {
                return false;
            }
        }
        fixture.Reply(0);
        await entered.Task.WaitAsync(TestGuard);
        fixture.Reply(1);
        var progressed = await caller.WaitAsync(TestGuard + TestGuard);
        await second.WaitAsync(TestGuard);
        Assert.True(progressed, "Caller exhausted the test guard while the other reply was ready.");
    }

    [Fact]
    public async Task OneSecondBlockedCallerDoesNotDelayAnotherCaller()
    {
        await using var fixture = await ActorRequests.StartAsync();
        var (first, second) = await fixture.SubmitAsync();
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        using var release = new ManualResetEventSlim();
        var firstCaller = FirstCallerAsync();
        async Task FirstCallerAsync()
        {
            _ = await first.ConfigureAwait(false);
            entered.SetResult();
            if (!release.Wait(TestGuard))
                throw new TimeoutException("Blocked caller exhausted the test cleanup guard.");
        }
        fixture.Reply(0);
        await entered.Task.WaitAsync(TestGuard);
        var elapsed = Stopwatch.StartNew();
        var releaseTask = Task.Run(async () =>
        {
            await Task.Delay(TimeSpan.FromSeconds(1));
            release.Set();
        });
        try
        {
            var secondCaller = ObserveSecondAsync();
            fixture.Reply(1);
            var blocked = await secondCaller.WaitAsync(TestGuard);
            Console.WriteLine(
                $"Other caller delay: {blocked.Delay.TotalMilliseconds:F3} ms; first released: {blocked.Released}"
            );
            Assert.False(blocked.Released, "The other completion waited for the blocked caller.");
        }
        finally
        {
            await releaseTask;
            release.Set();
            await firstCaller.WaitAsync(TestGuard);
        }

        async Task<(TimeSpan Delay, bool Released)> ObserveSecondAsync()
        {
            _ = await second.ConfigureAwait(false);
            return (elapsed.Elapsed, release.IsSet);
        }
    }

    [Fact]
    public async Task TableRequestContinuationRunsInTheThreadPoolDispatcherTurn()
    {
        await using var fixture = await ActorRequests.StartAsync();
        var (first, second) = await fixture.SubmitAsync();
        var observed = new TaskCompletionSource<(bool Dispatcher, bool ThreadPool)>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        first
            .ConfigureAwait(false)
            .GetAwaiter()
            .UnsafeOnCompleted(() =>
                observed.SetResult(
                    (
                        ZLinkCompletionDispatcher.IsCurrentExecution,
                        Thread.CurrentThread.IsThreadPoolThread
                    )
                )
            );
        fixture.Reply(0);
        fixture.Reply(1);
        var turn = await observed.Task.WaitAsync(TestGuard);
        Assert.Equal((true, true), turn);
        await Task.WhenAll(first, second).WaitAsync(TestGuard);
    }

    private sealed class ReplySignals
    {
        internal TaskCompletionSource[] Entered { get; } = [Signal(), Signal()];
        internal TaskCompletionSource[] Releases { get; } = [Signal(), Signal()];

        private static TaskCompletionSource Signal() =>
            new(TaskCreationOptions.RunContinuationsAsynchronously);
    }

    private sealed record CompletionProbe(int Number);

    private sealed record CompletionReply(int Number);

    private sealed class CompletionActor(IZLinkActorContext context) : IZLinkActor
    {
        public IZLinkActorContext Context { get; } = context;
    }

    private sealed class CompletionActorFactory : IZLinkActorFactory<CompletionActor>
    {
        public ValueTask<CompletionActor> CreateAsync(
            IZLinkActorContext context,
            CancellationToken cancellationToken = default
        ) => ValueTask.FromResult(new CompletionActor(context));
    }

    private sealed class CompletionEntrySpot(IZLinkEntrySpotContext context)
        : IZLinkEntrySpot<CompletionActor>
    {
        public IZLinkEntrySpotContext Context { get; } = context;

        public void Configure() =>
            Context.Handlers.AddActorPacket<CompletionReplyHandler, CompletionActor>(
                nameof(CompletionProbe)
            );

        public ValueTask OnJoinedActorAsync(
            CompletionActor actor,
            CancellationToken cancellationToken
        ) => ValueTask.CompletedTask;

        public ValueTask OnLeaveActorAsync(
            CompletionActor actor,
            CancellationToken cancellationToken
        ) => ValueTask.CompletedTask;
    }

    private sealed class CompletionReplyHandler(ReplySignals signals)
        : IZLinkEntrySpotActorRequestHandler<
            CompletionEntrySpot,
            CompletionActor,
            CompletionProbe,
            CompletionReply
        >
    {
        public async ValueTask<CompletionReply> HandleAsync(
            CompletionEntrySpot entrySpot,
            CompletionActor actor,
            IZLinkMessageContext context,
            CompletionProbe request,
            CancellationToken cancellationToken
        )
        {
            signals.Entered[request.Number].SetResult();
            await signals.Releases[request.Number].Task.WaitAsync(cancellationToken);
            return new CompletionReply(request.Number);
        }
    }

    private sealed class ActorRequests(IHost host, ReplySignals signals) : IAsyncDisposable
    {
        private const string MeshName = "completion-isolation";
        private const string ActorType = "completion-probe";
        private readonly string[] _actorIds =
        [
            $"completion-a-{Guid.NewGuid():N}",
            $"completion-b-{Guid.NewGuid():N}",
        ];

        internal static async Task<ActorRequests> StartAsync()
        {
            var signals = new ReplySignals();
            var host = Host.CreateDefaultBuilder()
                .ConfigureLogging(logging => logging.ClearProviders())
                .ConfigureServices(services =>
                {
                    services.AddSingleton(signals);
                    services.AddTransient<CompletionReplyHandler>();
                    services.AddZLinkFramework(options =>
                    {
                        options.UseTestLocationStore();
                        options
                            .AddRouteMesh(MeshName)
                            .Listen("tcp://127.0.0.1:0")
                            .Objects()
                            .Server()
                            .AddEntrySpot<CompletionEntrySpot>()
                            .AddActorFactory<CompletionActor, CompletionActorFactory>(
                                ActorType,
                                factory => factory.DisableRelocation()
                            );
                    });
                })
                .Build();
            try
            {
                await host.StartAsync();
                var fixture = new ActorRequests(host, signals);
                var manager = host.Services.GetRequiredService<IZLinkActorManager>();
                foreach (var actorId in fixture._actorIds)
                    Assert.IsType<ZLinkActorCreateResult.Created>(
                        await manager
                            .GetOrCreate(actorId, ActorType)
                            .InMesh(MeshName)
                            .Request(ZLinkMessage.Empty)
                            .Async()
                    );
                return fixture;
            }
            catch
            {
                await host.StopAsync();
                host.Dispose();
                throw;
            }
        }

        internal async Task<(
            Task<CompletionReply> First,
            Task<CompletionReply> Second
        )> SubmitAsync()
        {
            var client = host.Services.GetRequiredService<IZLinkActorClient>();
            var first = client
                .RequestToActor(_actorIds[0], new CompletionProbe(0))
                .Async<CompletionReply>()
                .AsTask();
            var second = client
                .RequestToActor(_actorIds[1], new CompletionProbe(1))
                .Async<CompletionReply>()
                .AsTask();
            await Task.WhenAll(signals.Entered.Select(signal => signal.Task)).WaitAsync(TestGuard);
            Assert.False(first.IsCompleted);
            Assert.False(second.IsCompleted);
            return (first, second);
        }

        internal void Reply(int number) => signals.Releases[number].TrySetResult();

        public async ValueTask DisposeAsync()
        {
            foreach (var signal in signals.Releases)
                signal.TrySetResult();
            await host.StopAsync();
            host.Dispose();
        }
    }
}
