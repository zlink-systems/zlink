using System.Collections.Concurrent;
using System.Diagnostics;
using System.Text.Json.Serialization;
using System.Threading.Channels;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Systems.Zlink.Stream.Connector;
using Systems.Zlink.Stream.Connector.Contracts;
using Xunit;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Contracts.Streams;
using Zlink.Framework.Locations.Redis;

namespace Zlink.NetworkSync.Server.Tests;

public sealed class SpotRelayTests
{
    private const string Mesh = "network-sync";
    private const string Stream = "players";
    private const string Match = "match";
    private const string Actor = "player";
    private const string PlayerType = "player";
    private const string MatchType = "network-sync-match";

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task RelayedInputAndSnapshotAdmission(bool delayAdmission)
    {
        using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(15));
        var ct = deadline.Token;
        using var traces = CaptureFlow();
        var fixture = new MatchFixture(delayAdmission);
        var prefix = $"network-sync:{Guid.NewGuid():N}:";
        await using var match = await StartAsync(prefix, fixture, options =>
            options.AddRouteMesh(Mesh).SetRoutingIdPrefix("match").Listen("tcp://127.0.0.1:0")
                .Objects().Server().AddEntrySpot<Entry>()
                .AddActorFactory<Player, PlayerFactory>(PlayerType, f => f.DisableRelocation())
                .AddSpotFactory<Room>(MatchType, f =>
                    f.ExecutionMode(ZLinkUserSpotExecutionMode.SpotWide).DisableRelocation()), ct);
        var spot = await match.Provider.GetRequiredService<IZLinkSpotManager>()
            .GetOrCreate(Match, MatchType).InMesh(Mesh).Async(ct);
        Assert.NotEqual(ZLinkSpotCreateState.Rejected, spot.State);

        await using var session = await StartAsync(prefix, fixture, options =>
        {
            options.AddRouteMesh(Mesh).SetRoutingIdPrefix("session").Listen("tcp://127.0.0.1:0")
                .Objects().Client();
            options.AddStreamNode(Stream).Bind("tcp://127.0.0.1:0")
                .EnableActorDispatch().AddSession<PlayerSession>();
        }, ct);
        Assert.NotEqual(match.Endpoint(ZLinkListenerKind.RouteMesh, Mesh),
            session.Endpoint(ZLinkListenerKind.RouteMesh, Mesh));

        await using var client = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri(session.Endpoint(ZLinkListenerKind.Stream, Stream)),
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
        });
        var pongs = Channel.CreateUnbounded<NetworkSyncPongV1>();
        client.On<NetworkSyncPongV1>((message, _) =>
        {
            pongs.Writer.TryWrite(message.Payload);
            return ValueTask.CompletedTask;
        });
        var snapshots = Channel.CreateUnbounded<NetworkSyncSnapshotV1>();
        client.On<NetworkSyncSnapshotV1>((message, _) =>
        {
            snapshots.Writer.TryWrite(message.Payload);
            return ValueTask.CompletedTask;
        });

        try
        {
            await client.Connect.Async(ct);
            await client.Send(new JoinMatch()).Async(ct);
            await fixture.Joined.Task.WaitAsync(ct);
            if (delayAdmission)
                await CheckDelayedAdmissionAsync(client, snapshots.Reader, fixture, ct);
            else
                await CheckLateInputAsync(client, snapshots.Reader, pongs.Reader, fixture, ct);
        }
        finally
        {
            fixture.ReleaseAdmission.TrySetResult();
            fixture.ReleaseTurn.TrySetResult();
        }
    }

    private static async Task CheckLateInputAsync(
        IZlinkStreamConnector client, ChannelReader<NetworkSyncSnapshotV1> snapshots,
        ChannelReader<NetworkSyncPongV1> pongs, MatchFixture fixture, CancellationToken ct)
    {
        await client.Send(new NetworkSyncPingV1(Match, 42, 12)).Async(ct);
        var pong = await pongs.ReadAsync(ct);
        Assert.Equal(42UL, pong.ProbeId);
        Assert.Equal(12UL, pong.T1Us);
        Assert.Equal(1UL, pong.Anchor.Epoch);
        Assert.Equal(0UL, pong.Anchor.Tick);
        Assert.Equal(33_334UL, pong.Anchor.TickUs);
        Assert.Equal(0UL, pong.HoldUs);

        await client.Send(new InputBarrier(true)).Async(ct);
        await fixture.TurnHeld.Task.WaitAsync(ct);
        await client.Send(new NetworkSyncInputV1(Match, 1, 1, 123, null, 0, [])).Async(ct);
        await fixture.InputAtSession.Task.WaitAsync(ct);
        fixture.Clock.Advance(TimeSpan.FromMilliseconds(100));
        fixture.ReleaseTurn.SetResult();
        await client.Send(new InputBarrier()).Async(ct);
        await fixture.Barriers.Reader.ReadAsync(ct);
        fixture.Clock.Advance(TimeSpan.FromMilliseconds(100));

        NetworkSyncSnapshotV1 snapshot;
        do
        {
            snapshot = await snapshots.ReadAsync(ct);
        } while (snapshot.InputFeedback?.T1Us != 123 && snapshot.Anchor.Tick < 6);

        var feedback = Assert.IsType<Feedback>(snapshot.InputFeedback);
        Assert.Equal("Late", feedback.Disposition);
        Assert.Equal(-66_667, feedback.ArrivalMarginUs);
        Assert.InRange(feedback.HoldUs, 0UL, 100_000UL);
        Assert.Equal(1UL, feedback.TargetTick);
        Assert.Equal(Match, snapshot.Anchor.MatchId);
        Assert.Null(snapshot.LastAppliedInputTick);
        Assert.True(snapshot.UsedFallbackInput);

        await client.Send(new NetworkSyncInputV1(Match, 1, 8, 456, snapshot.Anchor.Tick, 7,
            [new TestAction(ulong.MaxValue, 5)])).Async(ct);
        await client.Send(new InputBarrier()).Async(ct);
        await fixture.Barriers.Reader.ReadAsync(ct);
        fixture.Clock.Advance(TimeSpan.FromMilliseconds(100));

        do
        {
            snapshot = await snapshots.ReadAsync(ct);
        } while (snapshot.Anchor.Tick < 9);
        var accepted = Assert.IsType<Feedback>(snapshot.InputFeedback);
        Assert.Equal("Accepted", accepted.Disposition);
        Assert.Equal(456UL, accepted.T1Us);
        Assert.Equal(66_667, accepted.ArrivalMarginUs);
        Assert.Equal(8UL, snapshot.LastAppliedInputTick);
        Assert.Equal(19, snapshot.State);
    }

    private static async Task CheckDelayedAdmissionAsync(
        IZlinkStreamConnector client, ChannelReader<NetworkSyncSnapshotV1> snapshots,
        MatchFixture fixture, CancellationToken ct)
    {
        fixture.Clock.Advance(TimeSpan.FromMilliseconds(100));
        var first = await snapshots.ReadAsync(ct);
        Assert.Equal(3UL, first.Anchor.Tick);

        foreach (var expectedTick in new long[] { 6, 9 })
        {
            fixture.Clock.Advance(TimeSpan.FromMilliseconds(100));
            long tick;
            do
            {
                tick = await fixture.Steps.Reader.ReadAsync(ct);
            } while (tick < expectedTick);
            Assert.Equal(expectedTick, tick);
            await client.Send(new InputBarrier()).Async(ct);
            await fixture.Barriers.Reader.ReadAsync(ct);
            Assert.Equal(new[] { 3UL }, fixture.Submissions.ToArray());
        }

        fixture.ReleaseAdmission.SetResult();
        var next = await snapshots.ReadAsync(ct);
        Assert.Equal(9UL, next.Anchor.Tick);
        Assert.Equal(new[] { 3UL, 9UL }, fixture.Submissions.ToArray());
    }

    private static async Task<Node> StartAsync(
        string prefix, MatchFixture fixture, Action<IZLinkFrameworkOptions> configure, CancellationToken ct)
    {
        var services = new ServiceCollection();
        services.AddLogging();
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
        var hosted = provider.GetServices<IHostedService>().ToArray();
        var runtime = provider.GetRequiredService<IZLinkFrameworkRuntime>();
        var node = new Node(provider, hosted, runtime);
        try
        {
            foreach (var service in hosted)
                await service.StartAsync(ct);
            await foreach (var observation in runtime.ObserveAsync(ct))
            {
                if (observation.Status.State == ZLinkFrameworkRuntimeState.Serving)
                    return node;
                if (observation.Status.State != ZLinkFrameworkRuntimeState.Preparing)
                    throw new InvalidOperationException($"Unexpected startup state {observation.Status.State}.");
            }
            throw new InvalidOperationException("Runtime stopped before serving.");
        }
        catch
        {
            await node.DisposeAsync();
            throw;
        }
    }

    private sealed record Node(ServiceProvider Provider, IHostedService[] Hosted, IZLinkFrameworkRuntime Runtime)
        : IAsyncDisposable
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
                    File.AppendAllText(path, $"{activity.OperationName} {System.Text.Json.JsonSerializer.Serialize(activity.TagObjects)}{Environment.NewLine}");
            },
        };
        ActivitySource.AddActivityListener(listener);
        return listener;
    }

    public sealed class MatchFixture(bool delayAdmission)
    {
        public bool DelayAdmission { get; } = delayAdmission;
        public ManualClock Clock { get; } = new();
        public TaskCompletionSource Joined { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource ReleaseAdmission { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource TurnHeld { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource ReleaseTurn { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource InputAtSession { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public Channel<long> Steps { get; } = Channel.CreateUnbounded<long>();
        public Channel<bool> Barriers { get; } = Channel.CreateUnbounded<bool>();
        public ConcurrentQueue<ulong> Submissions { get; } = new();
    }

    public sealed class ManualClock : TimeProvider
    {
        private long _timestamp;
        public override long TimestampFrequency => TimeSpan.TicksPerSecond;
        public override long GetTimestamp() => Interlocked.Read(ref _timestamp);
        public void Advance(TimeSpan duration) => Interlocked.Add(ref _timestamp, duration.Ticks);
    }

    public sealed class Player(IZLinkActorContext context) : IZLinkActor
    {
        public IZLinkActorContext Context { get; } = context;
    }

    public sealed class PlayerFactory : IZLinkActorFactory<Player>
    {
        public ValueTask<Player> CreateAsync(IZLinkActorContext context, CancellationToken ct) =>
            ValueTask.FromResult(new Player(context));
    }

    private sealed class HoldingSession(IZLinkBoundSession session, MatchFixture fixture) : IZLinkBoundSession
    {
        private bool _heldFirst;

        public IZLinkBoundSessionSendCall Send<TMessage>(TMessage message)
        {
            var call = session.Send(message);
            if (message is not global::Zlink.NetworkSync.Server.NetworkSyncSnapshotV1<int> snapshot)
                return call;

            fixture.Submissions.Enqueue(snapshot.Anchor.Tick);
            if (_heldFirst)
                return call;

            _heldFirst = true;
            return new HoldingCall(call, fixture.ReleaseAdmission.Task);
        }

        public ValueTask DisconnectAsync(CancellationToken ct = default) => session.DisconnectAsync(ct);
    }

    private sealed class HoldingCall(IZLinkBoundSessionSendCall call, Task hold) : IZLinkBoundSessionSendCall
    {
        public IZLinkBoundSessionSendCall Metadata(string key, string value)
        {
            call.Metadata(key, value);
            return this;
        }

        public IZLinkBoundSessionSendCall Metadata(ZLinkMessageMetadata metadata)
        {
            call.Metadata(metadata);
            return this;
        }

        public async ValueTask Async()
        {
            await call.Async();
            await hold;
        }
    }

    public sealed class Entry(IZLinkEntrySpotContext context) : IZLinkEntrySpot<Player>
    {
        public IZLinkEntrySpotContext Context { get; } = context;
        public void Configure() => Context.Handlers.AddActorPacket<JoinHandler, Player>(nameof(JoinMatch));
        public ValueTask OnJoinedActorAsync(Player actor, CancellationToken ct) => ValueTask.CompletedTask;
        public ValueTask OnLeaveActorAsync(Player actor, CancellationToken ct) => ValueTask.CompletedTask;
    }

    public sealed class JoinHandler : IZLinkEntrySpotActorSendHandler<Entry, Player, JoinMatch>
    {
        public ValueTask HandleAsync(Entry spot, Player actor, IZLinkMessageContext context, JoinMatch message, CancellationToken ct)
        {
            actor.Context.JoinSpot(Match).Defer();
            return ValueTask.CompletedTask;
        }
    }

    public sealed class PlayerSession(IZLinkSessionContext context, IZLinkActorManager actors, MatchFixture fixture) : IZLinkSession
    {
        public IZLinkSessionContext Context { get; } = context;

        public async ValueTask OnDispatchAsync(ZLinkSessionDispatchContext dispatch, ZLinkMessage payload, CancellationToken ct)
        {
            if (dispatch.PacketName == nameof(JoinMatch))
            {
                var created = await actors.GetOrCreate(Actor, PlayerType).InMesh(Mesh)
                    .Request(ZLinkMessage.Empty).Async(ct);
                var actor = Assert.IsType<ZLinkActorCreateResult.Created>(created).Actor;
                var binding = await Context.Actors.BindAsync(actor, ct);
                await binding.RelayAsync(payload);
            }
            else if (dispatch.PacketName == nameof(InputBarrier))
            {
                await Context.Actors.Bound.Single().RelayAsync(payload);
            }
            else
            {
                await NetworkSyncSession.TryRelayAsync(dispatch, payload, Context.Actors.Bound.SingleOrDefault());
                if (dispatch.PacketName == nameof(NetworkSyncInputV1))
                    fixture.InputAtSession.TrySetResult();
            }
        }

        public ValueTask OnConnectedAsync(CancellationToken ct) => ValueTask.CompletedTask;
        public ValueTask OnDisconnectedAsync(CancellationToken ct) => ValueTask.CompletedTask;
        public ValueTask OnErrorAsync(ZLinkStreamError error, CancellationToken ct) =>
            ValueTask.FromException(new InvalidOperationException(error.Message));
    }

    public sealed class Room(IZLinkSpotContext context, MatchFixture fixture, ILogger<Room> logger)
        : NetworkSyncSpot<Room, Player, int, int, int>(context, logger,
            actor => fixture.DelayAdmission ? new HoldingSession(actor.Context.BoundSession, fixture) : actor.Context.BoundSession,
            clock: fixture.Clock)
    {
        private int _state;
        private MatchFixture Fixture { get; } = fixture;

        protected override void ConfigureMatch() =>
            Context.Handlers.AddActorPacket<BarrierHandler, Player>(nameof(InputBarrier));

        protected override ValueTask<ZLinkSpotActorJoinResult> AdmitPlayerAsync(
            string actorId, ZLinkMessage request, CancellationToken ct) =>
            ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept());

        protected override ValueTask OnPlayerJoinedAsync(Player actor, CancellationToken ct)
        {
            Fixture.Joined.TrySetResult();
            return ValueTask.CompletedTask;
        }

        protected override int GetMissingInput(string actorId) => 0;

        protected override void Step(long epoch, long tick, NetworkSyncInputs<int, int> inputs)
        {
            for (var index = 0; index < inputs.Count; index++)
            {
                var input = inputs[index];
                Assert.Equal(Actor, input.ActorId);
                _state += input.Input;
                for (var action = 0; action < input.ActionCount; action++)
                    _state += input.GetAction(action).Payload;
            }
            Fixture.Steps.Writer.TryWrite(tick);
        }

        protected override int CaptureSnapshot() => _state;

        public sealed class BarrierHandler : IZLinkSpotActorSendHandler<Room, Player, InputBarrier>
        {
            public async ValueTask HandleAsync(Room spot, Player actor, IZLinkMessageContext context,
                InputBarrier message, CancellationToken ct)
            {
                if (message.Hold)
                {
                    spot.Fixture.TurnHeld.TrySetResult();
                    await spot.Fixture.ReleaseTurn.Task.WaitAsync(ct);
                }
                else
                {
                    spot.Fixture.Barriers.Writer.TryWrite(true);
                }
            }
        }
    }

    public sealed record JoinMatch;
    public sealed record InputBarrier(bool Hold = false);
    public sealed record TestAction([property: JsonPropertyName("action_id")] ulong Id, int Payload);
    public sealed record NetworkSyncInputV1(
        [property: JsonPropertyName("match_id")] string MatchId,
        ulong Epoch,
        [property: JsonPropertyName("target_tick")] ulong TargetTick,
        [property: JsonPropertyName("t1_us")] ulong T1Us,
        [property: JsonPropertyName("last_snapshot_tick")] ulong? LastSnapshotTick,
        int Input,
        TestAction[] Actions);

    public sealed record NetworkSyncSnapshotV1(
        Anchor Anchor, int State,
        [property: JsonPropertyName("last_applied_input_tick")] ulong? LastAppliedInputTick,
        [property: JsonPropertyName("used_fallback_input")] bool UsedFallbackInput,
        [property: JsonPropertyName("input_feedback")] Feedback? InputFeedback);

    public sealed record Anchor(
        [property: JsonPropertyName("match_id")] string MatchId,
        ulong Epoch, ulong Tick,
        [property: JsonPropertyName("tick_us")] ulong TickUs,
        [property: JsonPropertyName("since_tick_us")] ulong SinceTickUs);

    public sealed record Feedback(
        [property: JsonPropertyName("target_tick")] ulong TargetTick,
        [property: JsonPropertyName("t1_us")] ulong T1Us,
        [property: JsonPropertyName("hold_us")] ulong HoldUs,
        [property: JsonPropertyName("arrival_margin_us")] long ArrivalMarginUs,
        string Disposition);
}
