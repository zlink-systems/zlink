using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Systems.Zlink.Stream.Connector;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

// Host relocation §7/§10.3 and relocation 04 §4.2: a SpotWide User Spot and
// its member Actors are one relocation unit, so the Spot seal is also the
// member Actors' unit seal. A member frame accepted after that seal is held
// for the handoff (carried, or returned to the source queue on abort); it is
// never refused by the sealed Spot and dropped (#1877).
public sealed class SpotWideMemberSealCaptureTests
{
    private static readonly TimeSpan Wait = TimeSpan.FromSeconds(10);

    [Fact]
    public async Task Member_frame_after_spot_seal_is_held_and_runs_after_abort()
    {
        var trace = new RelocationBehaviorTrace();
        trace.ReleaseJoinHandler.TrySetResult();
        trace.ReleaseTargetLifecycle.TrySetResult();
        trace.ReleaseSourceLeave.TrySetResult();
        trace.ReleaseSealStageWork.TrySetResult();
        var backlog = new HandoffBacklogLoggerProvider();
        var store = new ZLinkInMemoryProviderLocationStore();
        var roots = new SynchronizedRelocationStore();
        await using var source = await RelocationBehaviorHost.StartAsync(
            "source",
            trace,
            store,
            roots,
            true,
            relocateSpots: true,
            configureServices: services =>
                services.AddLogging(logging => logging.AddProvider(backlog))
        );
        _ = await source
            .Services.GetRequiredService<IZLinkSpotManager>()
            .GetOrCreate("zone-2", RelocationBehaviorHost.SpotType)
            .InMesh(RelocationBehaviorHost.MeshName)
            .Async();
        var actorId = $"member-seal-{Guid.NewGuid():N}";
        _ = await source
            .Services.GetRequiredService<IZLinkActorManager>()
            .GetOrCreate(actorId, RelocationBehaviorHost.ActorType)
            .InMesh(RelocationBehaviorHost.MeshName)
            .Request(new BehaviorCreate(7))
            .Async();

        var builder = Host.CreateApplicationBuilder();
        var handler = new SealStageHandler();
        builder.Services.AddSingleton(handler);
        builder.Services.AddZLinkFramework(options =>
        {
            options.AddLocationStore(store);
            options.AddRelocationStore(roots);
            options.ConfigureLocations().PollingInterval = TimeSpan.FromMilliseconds(10);
            options
                .AddRouteMesh(RelocationBehaviorHost.MeshName)
                .Listen("tcp://127.0.0.1:0")
                .SetRoutingIdPrefix("aaa-session")
                .Objects()
                .Client();
            options
                .AddStreamNode("member-seal-stream")
                .Bind("tcp://127.0.0.1:0")
                .EnableActorDispatch()
                .AddSession<SealStageSession>();
        });
        using var session = builder.Build();
        await session.StartAsync();
        try
        {
            using (var ready = new CancellationTokenSource(Wait))
                await Task.WhenAll(
                    WaitForReadyPeersAsync(source.Services, 1, ready.Token),
                    WaitForReadyPeersAsync(session.Services, 1, ready.Token)
                );
            await using var connector = ZlinkStreamConnectorFactory.Create(
                new ZlinkStreamConnectorOptions
                {
                    Endpoint = new Uri(
                        session
                            .Services.GetRequiredService<IZLinkFrameworkRuntime>()
                            .GetListenerStatus(ZLinkListenerKind.Stream, "member-seal-stream")
                            .Endpoint!
                    ),
                    DispatchMode = ZlinkStreamDispatchMode.Immediate,
                    Reconnect = new ZlinkStreamReconnectOptions { Enabled = false },
                    Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
                }
            );
            await connector.Connect.Async();
            await connector.Send(new SealStageBind(actorId)).Async();
            await handler.Bound.Task.WaitAsync(Wait);
            await connector.Request(new BeginBehaviorJoin("zone-2")).Async<BehaviorAck>();
            await trace.WaitAsync("publicJoinCompleted");

            // The bound one-way relay path runs on the member before the seal.
            var beforeSeal = connector.WaitFor<BehaviorAck>().Timeout(Wait).Async();
            await connector.Send(new SealStageWork("before-seal")).Async();
            Assert.Equal("before-seal", (await beforeSeal).Payload.Marker);

            var state = source.Runtime.GetOrCreateActorState(actorId);
            var activation = Assert.IsAssignableFrom<ZLinkSpotActivation>(state.LiveActivation);
            Assert.Equal("zone-2", activation.SpotId);

            // The SpotWide aggregate seal (ZLinkSpotRetireScheduler) refuses
            // member Actor claims. The same step must hold member ingress.
            var seal = await activation.SealRelocationAsync(
                allowActorClaims: false,
                CancellationToken.None
            );
            Assert.True(state.Handoff.CapturesSourceIngress);

            var afterSeal = connector.WaitFor<BehaviorAck>().Timeout(Wait).Async();
            await connector.Send(new SealStageWork("after-seal")).Async();
            await backlog.WaitForBacklogAsync(actorId).WaitAsync(Wait);
            Assert.False(afterSeal.IsCompleted);

            // Pre-relay-ready abort: the held frame goes back to the source
            // queue in its accepted order and runs there exactly once.
            Assert.True(await activation.AbortRelocationAsync(seal));
            await source.Runtime.RestoreStandaloneActorRelocationSourceAsync(state);
            Assert.Equal("after-seal", (await afterSeal).Payload.Marker);
            Assert.False(state.Handoff.CapturesSourceIngress);
            Assert.False(handler.Disconnected.Task.IsCompleted);
            await connector.Close.Async();
        }
        finally
        {
            await session.StopAsync();
        }
    }

    private static async Task WaitForReadyPeersAsync(
        IServiceProvider services,
        int count,
        CancellationToken cancellationToken
    )
    {
        await foreach (
            var observed in services
                .GetRequiredService<IZLinkRouteMeshRuntime>()
                .ObserveAsync(RelocationBehaviorHost.MeshName, cancellationToken)
        )
            if (observed.Status.ReadyPeerCount == count)
                return;
    }

    // Completes when the runtime reports that a frame entered the source
    // handoff backlog (ZLinkActorHandoffState capture). The marker is the
    // runtime's own ActorHandoff log line, so the test waits on the event
    // instead of polling state.
    internal sealed class HandoffBacklogLoggerProvider : ILoggerProvider
    {
        private readonly object _gate = new();
        private readonly List<string> _lines = [];
        private readonly List<(string Actor, TaskCompletionSource Signal)> _waiters = [];

        public IReadOnlyList<string> Lines
        {
            get
            {
                lock (_gate)
                    return _lines.ToArray();
            }
        }

        public Task WaitForBacklogAsync(string actorId)
        {
            lock (_gate)
            {
                var signal = new TaskCompletionSource(
                    TaskCreationOptions.RunContinuationsAsynchronously
                );
                if (_lines.Any(line => Matches(line, actorId)))
                    signal.TrySetResult();
                else
                    _waiters.Add((actorId, signal));
                return signal.Task;
            }
        }

        public ILogger CreateLogger(string categoryName) =>
            categoryName == "Zlink.Framework.ActorHandoff"
                ? new Logger(this)
                : Microsoft.Extensions.Logging.Abstractions.NullLogger.Instance;

        public void Dispose() { }

        private static bool Matches(string line, string actorId) =>
            line.StartsWith($"handoff_backlog actor={actorId} ", StringComparison.Ordinal);

        private void Record(string line)
        {
            lock (_gate)
            {
                _lines.Add(line);
                foreach (var waiter in _waiters.Where(waiter => Matches(line, waiter.Actor)))
                    waiter.Signal.TrySetResult();
            }
        }

        private sealed class Logger(HandoffBacklogLoggerProvider owner) : ILogger
        {
            public IDisposable? BeginScope<TState>(TState state)
                where TState : notnull => null;

            public bool IsEnabled(LogLevel logLevel) => true;

            public void Log<TState>(
                LogLevel logLevel,
                EventId eventId,
                TState state,
                Exception? exception,
                Func<TState, Exception?, string> formatter
            ) => owner.Record(formatter(state, exception));
        }
    }
}
