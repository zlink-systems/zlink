using System.Diagnostics;
using System.Text.Json;
using System.Threading.Channels;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Systems.Zlink.Stream.Connector;
using Systems.Zlink.Stream.Connector.Contracts;
using Xunit;
using Xunit.Abstractions;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Contracts.Streams;
using Zlink.Framework.Locations.Redis;
using Zlink.NetworkSync.Server;

[assembly: CollectionBehavior(DisableTestParallelization = true)]

namespace Zlink.NetworkSync.Interop;

public sealed class StreamRelayTests(ITestOutputHelper output)
{
    private const string Mesh = "interop";
    private const string MatchType = "match";
    private const string PlayerType = "player";
    private const string Stream = "players";
    private const int TickRate = 20;
    private const ulong TickUs = 1_000_000 / TickRate;

    [Fact]
    public async Task UnityPluginRoundTripsThroughBoundActorAndSpot()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));
        var ct = timeout.Token;
        using var flow = CaptureFlow();
        var fixture = new RelayFixture();
        var settings = new MatchSettings(TickRate, new TestClock());
        var prefix = $"network-sync-interop:{Guid.NewGuid():N}:";
        await using var matchNode = await StartAsync(prefix, settings, fixture, options =>
            options.AddRouteMesh(Mesh).SetRoutingIdPrefix("match").Listen("tcp://127.0.0.1:0")
                .Objects().Server().AddEntrySpot<Entry>()
                .AddActorFactory<TestPlayer, PlayerFactory>(PlayerType, f => f.DisableRelocation())
                .AddSpotFactory<TestMatch>(MatchType, f =>
                    f.ExecutionMode(ZLinkUserSpotExecutionMode.SpotWide).DisableRelocation()), ct);
        var spot = await matchNode.Provider.GetRequiredService<IZLinkSpotManager>()
            .GetOrCreate(MatchSettings.MatchId, MatchType).InMesh(Mesh).Async(ct);
        Assert.NotEqual(ZLinkSpotCreateState.Rejected, spot.State);

        await using var sessionNode = await StartAsync(prefix, settings, fixture, options =>
        {
            options.AddRouteMesh(Mesh).SetRoutingIdPrefix("session").Listen("tcp://127.0.0.1:0")
                .Objects().Client();
            options.AddStreamNode(Stream).Bind("tcp://127.0.0.1:0")
                .EnableActorDispatch().AddSession<PlayerSession>();
        }, ct);
        Assert.NotEqual(matchNode.Endpoint(ZLinkListenerKind.RouteMesh, Mesh),
            sessionNode.Endpoint(ZLinkListenerKind.RouteMesh, Mesh));
        await WaitForPeerAsync(sessionNode, ct);
        await WaitForPeerAsync(matchNode, ct);

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri(sessionNode.Endpoint(ZLinkListenerKind.Stream, Stream)),
            DispatchMode = ZlinkStreamDispatchMode.Manual,
        });
        ulong clientNow = 0;
        using var unity = new NetworkSyncClient<int, int, int>(() => clientNow);
        unity.Attach(connector, MatchSettings.MatchId);
        var pongSeen = false;
        using var pongHandler = connector.On<NetworkSyncPongV1>(NetworkSyncPackets.Pong, (_, _) =>
        {
            pongSeen = true;
            return ValueTask.CompletedTask;
        });
        var snapshots = new List<NetworkSyncSnapshotV1<int>>();
        using var snapshotHandler = connector.On<NetworkSyncSnapshotV1<int>>(NetworkSyncPackets.Snapshot, (message, _) =>
        {
            snapshots.Add(message.Payload);
            return ValueTask.CompletedTask;
        });
        using var errorHandler = connector.OnErrorReceived((error, _) =>
        {
            fixture.Errors.Add(error.ToString());
            return ValueTask.CompletedTask;
        });

        try
        {
            await connector.Connect.Async(ct);
            await connector.Send(new JoinMatch()).Async(ct);
            await PumpUntilAsync(connector, fixture, () => settings.Joined.Task.IsCompletedSuccessfully, ct);
            await unity.Update();
            clientNow = 2_000;
            await PumpUntilAsync(connector, fixture, () => pongSeen, ct);
            await AdvanceToAsync(3);
            Assert.True(unity.TryGetInterpolation(out var ready) && !ready.IsHolding);

            Assert.True(await unity.SendInputAsync(0, [7], ct));
            await fixture.RelayedInputs.Reader.ReadAsync(ct);
            await SendBarrierAsync(false);
            await AdvanceToAsync(6);
            var accepted = Assert.Single(snapshots.Select(s => s.InputFeedback).OfType<InputFeedback>());
            Assert.Equal(InputDisposition.Accepted, accepted.Disposition);
            Assert.Equal(152_000UL, accepted.T1Us);
            Assert.Equal((long)(accepted.TargetTick * TickUs) - 150_000, accepted.ArrivalMarginUs);
            Assert.All(snapshots.Where(s => s.Anchor.Tick < accepted.TargetTick), s => Assert.Equal(0, s.State));
            var applied = Assert.Single(snapshots, s => s.Anchor.Tick == accepted.TargetTick);
            Assert.Equal(7, applied.State);
            Assert.Equal(accepted.TargetTick, applied.LastAppliedInputTick);

            await SendBarrierAsync(true);
            Assert.True(await unity.SendInputAsync(0, [11], ct));
            await fixture.RelayedInputs.Reader.ReadAsync(ct);
            settings.Clock.Set(600_000);
            clientNow = 602_000;
            settings.ReleaseTurn.SetResult();
            await SendBarrierAsync(false);
            await AdvanceToAsync(13);

            var late = Assert.Single(snapshots.Select(s => s.InputFeedback).OfType<InputFeedback>(),
                f => f.Disposition == InputDisposition.Late);
            Assert.Equal(302_000UL, late.T1Us);
            Assert.True(late.ArrivalMarginUs < 0);
            Assert.Equal((long)(late.TargetTick * TickUs) - 600_000, late.ArrivalMarginUs);
            Assert.All(snapshots.Where(s => s.Anchor.Tick >= accepted.TargetTick), s => Assert.Equal(7, s.State));
            output.WriteLine($"relay accepted tick={accepted.TargetTick} margin={accepted.ArrivalMarginUs} us; " +
                             $"held Spot turn 300000 us, late tick={late.TargetTick} margin={late.ArrivalMarginUs} us; " +
                             $"snapshots={snapshots.Count}");
        }
        finally
        {
            settings.ReleaseTurn.TrySetResult();
        }

        async Task SendBarrierAsync(bool hold)
        {
            await connector.Send(new SpotBarrier(hold)).Async(ct);
            Assert.Equal(hold, await settings.Barriers.Reader.ReadAsync(ct));
        }

        async Task AdvanceToAsync(ulong target)
        {
            var nextTick = settings.Clock.Now / TickUs + 1;
            for (var tick = nextTick; tick <= target; tick++)
            {
                settings.Clock.Set(tick * TickUs);
                clientNow = settings.Clock.Now + 2_000;
                await PumpUntilAsync(connector, fixture, () => snapshots.Count != 0 && snapshots[^1].Anchor.Tick >= tick, ct);
            }
        }
    }

    private static async Task WaitForPeerAsync(Node node, CancellationToken ct)
    {
        var topology = node.Provider.GetRequiredService<IZLinkRouteMeshRuntime>();
        await foreach (var observation in topology.ObserveAsync(Mesh, ct))
        {
            if (observation.Status.ReadyPeerCount == 1)
                return;
        }
        throw new InvalidOperationException("RouteMesh stopped before the remote peer was ready.");
    }

    private static async Task PumpUntilAsync(IZlinkStreamConnector connector, RelayFixture fixture,
        Func<bool> completed, CancellationToken ct)
    {
        do
        {
            await connector.Dispatch.Async(ct);
            Assert.Empty(fixture.Errors);
            if (completed())
                return;
            await Task.Delay(1, ct);
        } while (true);
    }

    private static async Task<Node> StartAsync(string prefix, MatchSettings settings, RelayFixture fixture,
        Action<IZLinkFrameworkOptions> configure, CancellationToken ct)
    {
        var services = new ServiceCollection();
        services.AddLogging(builder => builder.AddSimpleConsole());
        services.AddSingleton(settings);
        services.AddSingleton(fixture);
        services.AddZLinkFramework(options =>
        {
            options.DisableImplicitHandlerAutoRegistration();
            options.ConfigureDispatch().Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
            options.AddLocationStore(new ZLinkRedisLocationStore(redis =>
            {
                redis.ConnectionString = Environment.GetEnvironmentVariable("ZLINK_REDIS_TEST_ENDPOINT") ?? "127.0.0.1:6379";
                redis.KeyPrefix = prefix;
            }));
            configure(options);
        });

        var provider = services.BuildServiceProvider();
        var node = new Node(provider, provider.GetServices<IHostedService>().ToArray(),
            provider.GetRequiredService<IZLinkFrameworkRuntime>());
        try
        {
            foreach (var service in node.Hosted)
                await service.StartAsync(ct);
            await foreach (var observation in node.Runtime.ObserveAsync(ct))
            {
                if (observation.Status.State == ZLinkFrameworkRuntimeState.Serving)
                    return node;
                Assert.Equal(ZLinkFrameworkRuntimeState.Preparing, observation.Status.State);
            }
            throw new InvalidOperationException("Runtime stopped before serving.");
        }
        catch
        {
            await node.DisposeAsync();
            throw;
        }
    }

    private sealed record Node(ServiceProvider Provider, IHostedService[] Hosted, IZLinkFrameworkRuntime Runtime) : IAsyncDisposable
    {
        public string Endpoint(ZLinkListenerKind kind, string name) => Runtime.GetListenerStatus(kind, name).Endpoint!;

        public async ValueTask DisposeAsync()
        {
            try
            {
                foreach (var service in Hosted.Reverse())
                    await service.StopAsync(CancellationToken.None);
            }
            finally
            {
                await Provider.DisposeAsync();
            }
        }
    }

    private static ActivityListener? CaptureFlow()
    {
        if (Environment.GetEnvironmentVariable("NETWORK_SYNC_FLOW_LOG") is not { } path)
            return null;

        var gate = new object();
        var listener = new ActivityListener
        {
            ShouldListenTo = source => source.Name == "Zlink.Framework",
            Sample = (ref ActivityCreationOptions<ActivityContext> _) => ActivitySamplingResult.AllData,
            ActivityStopped = activity =>
            {
                lock (gate)
                {
                    File.AppendAllText(path, $"{activity.OperationName} {JsonSerializer.Serialize(activity.TagObjects)}{Environment.NewLine}");
                }
            },
        };
        ActivitySource.AddActivityListener(listener);
        return listener;
    }

    public sealed class RelayFixture
    {
        public Channel<bool> RelayedInputs { get; } = Channel.CreateUnbounded<bool>();
        public List<string> Errors { get; } = [];
    }

    public sealed record JoinMatch;

    public sealed class PlayerFactory : IZLinkActorFactory<TestPlayer>
    {
        public ValueTask<TestPlayer> CreateAsync(IZLinkActorContext context, CancellationToken ct) =>
            ValueTask.FromResult(new TestPlayer(context));
    }

    public sealed class Entry(IZLinkEntrySpotContext context) : IZLinkEntrySpot<TestPlayer>
    {
        public IZLinkEntrySpotContext Context { get; } = context;
        public void Configure() => Context.Handlers.AddActorPacket<JoinHandler, TestPlayer>(nameof(JoinMatch));
        public ValueTask OnJoinedActorAsync(TestPlayer actor, CancellationToken ct) => ValueTask.CompletedTask;
        public ValueTask OnLeaveActorAsync(TestPlayer actor, CancellationToken ct) => ValueTask.CompletedTask;
    }

    public sealed class JoinHandler : IZLinkEntrySpotActorSendHandler<Entry, TestPlayer, JoinMatch>
    {
        public ValueTask HandleAsync(Entry spot, TestPlayer actor, IZLinkMessageContext context, JoinMatch message, CancellationToken ct)
        {
            actor.Context.JoinSpot(MatchSettings.MatchId).Defer();
            return ValueTask.CompletedTask;
        }
    }

    public sealed class PlayerSession(IZLinkSessionContext context, IZLinkActorManager actors, RelayFixture fixture) : IZLinkSession
    {
        public IZLinkSessionContext Context { get; } = context;

        public async ValueTask OnDispatchAsync(ZLinkSessionDispatchContext dispatch, ZLinkMessage payload, CancellationToken ct)
        {
            if (dispatch.PacketName == nameof(JoinMatch))
            {
                var created = await actors.GetOrCreate(PlayerType, PlayerType).InMesh(Mesh)
                    .Request(ZLinkMessage.Empty).Async(ct);
                var actor = Assert.IsType<ZLinkActorCreateResult.Created>(created).Actor;
                var binding = await Context.Actors.BindAsync(actor, ct);
                await binding.RelayAsync(payload);
            }
            else if (dispatch.PacketName == nameof(SpotBarrier))
            {
                await Context.Actors.Bound.Single().RelayAsync(payload);
            }
            else
            {
                Assert.True(await NetworkSyncSession.TryRelayAsync(dispatch, payload, Context.Actors.Bound.Single()));
                if (dispatch.PacketName == NetworkSyncPackets.Input)
                    fixture.RelayedInputs.Writer.TryWrite(true);
            }
        }

        public ValueTask OnConnectedAsync(CancellationToken ct) => ValueTask.CompletedTask;
        public ValueTask OnDisconnectedAsync(CancellationToken ct) => ValueTask.CompletedTask;
        public ValueTask OnErrorAsync(ZLinkStreamError error, CancellationToken ct) =>
            ValueTask.FromException(new InvalidOperationException(error.Message));
    }
}
