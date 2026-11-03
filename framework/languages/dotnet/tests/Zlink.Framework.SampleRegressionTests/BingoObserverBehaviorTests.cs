using System.Reflection;
using Bingo.Server.Play.Domain.Bingo;
using Bingo.Server.Play.Infrastructure.ZLink.Actors;
using Bingo.Server.Play.Infrastructure.ZLink.Spots.BingoRoomSpot;
using Bingo.Server.Play.Infrastructure.ZLink.Spots.BingoRoomSpot.Notifications;
using Bingo.Server.Play.Infrastructure.ZLink.Spots.EntrySpot;
using Bingo.Server.Play.Infrastructure.ZLink.Spots.EntrySpot.Handlers;
using Bingo.Shared.Contracts;
using Microsoft.Extensions.Logging.Abstractions;
using Xunit;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Errors;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Contracts.Streams;
using Zlink.Framework.Contracts.Timers;
using Zlink.Framework.Contracts.Workers;

namespace Zlink.Framework.UnitTests.Runtime;

// Compiled with the existing UnitTests access to construct fake request terminals.
// The sample itself uses only public Framework contracts.
public sealed class BingoObserverBehaviorTests
{
    [Fact]
    public async Task Finished_room_observation_request_returns_not_subscribed_before_room_creation()
    {
        var observation = new GetBingoObservationRes
        {
            State = new BingoRoomState { RoomId = "room", Status = BingoRoomStatus.Finished },
        };
        var request = Proxy<IZLinkSpotRequestCall>(method =>
        {
            Assert.Equal("Async", method.Name);
            return new ZLinkRequest<GetBingoObservationRes>(
                ValueTask.CompletedTask,
                ValueTask.FromResult(observation)
            );
        });
        var outbound = Proxy<IZLinkSpotOutbound>(method =>
        {
            Assert.Equal("RequestToSpot", method.Name);
            return request;
        });
        var context = Proxy<IZLinkEntrySpotContext>(method =>
        {
            Assert.Equal("get_Outbound", method.Name);
            return outbound;
        });
        var entrySpot = new BingoEntrySpot(context, NullLogger<BingoEntrySpot>.Instance);
        var actor = new PlayerActor("observer", null!, NullLogger<PlayerActor>.Instance);
        var handler = new ObserveBingoEventsHandler(null!);

        var response = await handler.HandleAsync(
            entrySpot,
            actor,
            null!,
            new ObserveBingoEventsReq { RoomId = "room" },
            default
        );

        Assert.False(response.Subscribed);
    }

    [Fact]
    public async Task Player_join_notification_failure_does_not_reject_admission_or_stop_recipients()
    {
        var state = new BingoRoomState { RoomId = "room", Status = "WaitingForPlayers" };
        var failingSession = new ObservationSession(message => message is PlayerJoinedNotify);
        var receivingSession = new ObservationSession();
        var failingActor = new PlayerActor(
            "disconnected",
            new ObserverContext(failingSession),
            NullLogger<PlayerActor>.Instance
        );
        var receivingActor = new PlayerActor(
            "connected",
            new ObserverContext(receivingSession),
            NullLogger<PlayerActor>.Instance
        );
        var notification = new BingoRoomEvent(
            BingoRoomEventKind.PlayerJoined,
            failingActor,
            state,
            "new-player",
            "New Player",
            1,
            false
        );
        var followingNotification = notification with { Recipient = receivingActor };
        var publisher = new BingoNotificationPublisher(
            NullLogger<BingoNotificationPublisher>.Instance
        );

        await publisher.PublishAsync([notification, followingNotification], default);

        Assert.Empty(failingSession.Messages);
        Assert.Collection(
            receivingSession.Messages,
            message => Assert.IsType<PlayerJoinedNotify>(message)
        );
    }

    [Theory]
    [InlineData(false, null)]
    [InlineData(true, null)]
    [InlineData(false, ZLinkFrameworkErrorKind.NotFound)]
    [InlineData(false, ZLinkFrameworkErrorKind.Unavailable)]
    public async Task Finished_room_snapshot_does_not_compensate_a_late_observer(
        bool earlyEvent,
        ZLinkFrameworkErrorKind? failureKind
    )
    {
        var reward = new BingoRewardAcquiredEvent
        {
            RoomId = "room",
            ActorId = "winner",
            DrawSeq = 15,
            ItemId = "rare-golden-dauber",
            ItemName = "Golden Dauber",
            Rarity = "Legendary",
        };
        var state = new BingoRoomState
        {
            RoomId = "room",
            Status = "Finished",
            DrawSeq = 15,
        };
        state.Winners.Add("winner");
        var context = new ObservationContext(
            new GetBingoObservationRes { State = state, Reward = reward },
            failureKind is { } kind
                ? new ZLinkFrameworkException(kind, "Room request failed")
                : null
        );
        var room = new BingoRoom(
            context,
            new BingoRoomEventMapper(),
            new BingoNotificationPublisher(NullLogger<BingoNotificationPublisher>.Instance),
            NullLogger<BingoRoom>.Instance
        );
        room.ApplySettings(BingoRoomSettings.CreateObserver("room", "observer"));
        var session = new ObservationSession();
        var actor = new PlayerActor(
            "observer",
            new ObserverContext(session),
            NullLogger<PlayerActor>.Instance
        );
        if (earlyEvent)
            await room.AnnounceRewardAsync(reward, default);
        Assert.Empty(session.Messages);
        _ = await room.OnActorJoinAsync(
            "observer",
            ZLinkMessage.From(
                new BingoRoomJoinReq
                {
                    ActorId = "observer",
                    RoomId = "room",
                    DisplayName = "Observer",
                    ObserveOnly = true,
                }
            ),
            default
        );
        if (failureKind == ZLinkFrameworkErrorKind.Unavailable)
        {
            var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                room.OnJoinedActorAsync(actor, default).AsTask()
            );
            Assert.Equal(failureKind, error.Kind);
            Assert.Empty(session.Messages);
            return;
        }
        await room.OnJoinedActorAsync(actor, default);
        if (failureKind == ZLinkFrameworkErrorKind.NotFound)
        {
            Assert.Empty(session.Messages);
            return;
        }
        Assert.Equal(1, context.ReadinessSignals);
        Assert.Collection(
            session.Messages,
            value => Assert.Equal(state, Assert.IsType<BingoObservationStateNotify>(value).State)
        );
        await room.AnnounceRewardAsync(reward, default);
        Assert.Equal(2, session.Messages.Count);
    }

    [Fact]
    public async Task Finished_room_stops_its_existing_draw_timer_without_self_waiting()
    {
        var context = new ObservationContext(new GetBingoObservationRes());
        var room = new BingoRoom(
            context,
            new BingoRoomEventMapper(),
            new BingoNotificationPublisher(NullLogger<BingoNotificationPublisher>.Instance),
            NullLogger<BingoRoom>.Instance
        );
        var settings = BingoRoomSettings.Create(BingoSampleModes.TwoPlayer, 0);
        room.ApplySettings(settings);
        await room.OnInitializeAsync(default);
        Assert.Equal(1, context.TimerCreations);
        var game = new BingoRoomGame("room", settings);
        game.JoinPlayer("winner", "Winner", 0, 0);
        game.JoinPlayer("other", "Other", 0, 0);
        game.SubmitCard("winner", BingoCard.FromSubmittedNumbers([1, 2, 3, 4, 0, 5, 6, 7, 8]));
        game.SubmitCard("other", BingoCard.FromSubmittedNumbers([1, 2, 3, 4, 0, 5, 6, 7, 8]));
        while (game.IsReadyToDraw)
            game.DrawNextNumber();
        var state = game.Snapshot();
        room.RestoreRelocationState(settings, state);
        var cleanup = room.LeaveFinishedActorsAsync(default).AsTask();
        try
        {
            Assert.True(
                cleanup.IsCompleted,
                "The draw callback must not await its own cancellation terminal."
            );
        }
        finally
        {
            context.Timer.Release.TrySetResult();
        }
        await cleanup;
        Assert.True(context.Timer.IsDisposed);
        Assert.Equal("winner", room.Observation().Reward.ActorId);
        room.ApplySettings(BingoRoomSettings.CreateObserver("room", "observer"));
        await room.OnInitializeAsync(default);
        Assert.Equal(1, context.TimerCreations);
    }

    private sealed class ObservationTimer : IZLinkTimer
    {
        internal TaskCompletionSource Release { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public bool IsDisposed { get; private set; }

        public ValueTask CancelAsync()
        {
            IsDisposed = true;
            return new ValueTask(Release.Task);
        }

        public ValueTask DisposeAsync() => CancelAsync();
    }

    private static T Proxy<T>(Func<MethodInfo, object?> invoke)
        where T : class
    {
        var proxy = DispatchProxy.Create<T, TestProxy>();
        ((TestProxy)(object)proxy).InvokeHandler = invoke;
        return proxy;
    }

    public class TestProxy : DispatchProxy
    {
        internal Func<MethodInfo, object?> InvokeHandler { get; set; } = null!;

        protected override object? Invoke(MethodInfo? targetMethod, object?[]? args) =>
            InvokeHandler(targetMethod!);
    }

    private sealed class ObservationSession : IZLinkBoundSession, IZLinkBoundSessionSendCall
    {
        internal List<object> Messages { get; } = new();
        private readonly Func<object, bool>? _shouldFail;

        internal ObservationSession(Func<object, bool>? shouldFail = null)
        {
            _shouldFail = shouldFail;
        }

        public IZLinkBoundSessionSendCall Send<T>(T message)
        {
            _failCurrentSend = _shouldFail?.Invoke(message!) == true;
            if (!_failCurrentSend)
                Messages.Add(message!);
            return this;
        }

        public ValueTask DisconnectAsync(CancellationToken cancellationToken = default) =>
            throw new NotSupportedException();

        private bool _failCurrentSend;

        public ValueTask Async() =>
            _failCurrentSend
                ? ValueTask.FromException(
                    new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.Unavailable,
                        "The recipient session is closed."
                    )
                )
                : ValueTask.CompletedTask;

        public IZLinkBoundSessionSendCall Metadata(string key, string value) => this;

        public IZLinkBoundSessionSendCall Metadata(ZLinkMessageMetadata metadata) => this;
    }

    private sealed class ObserverContext(ObservationSession session) : IZLinkActorContext
    {
        public string ActorId => "observer";
        public ulong ObjectGeneration => 1;
        public string MeshName => "bingo.play";
        public string? SpotId => "observe:room:observer";
        public IZLinkBoundSession BoundSession => session;

        public IZLinkActorJoinSpotCall JoinSpot(string id, ZLinkMessage request) =>
            throw new NotSupportedException();

        public IZLinkActorJoinEntrySpotCall JoinEntrySpot(ZLinkMessage request) =>
            throw new NotSupportedException();
    }

    private sealed class ObservationContext(
        GetBingoObservationRes response,
        Exception? failure = null
    ) : IZLinkSpotContext, IZLinkSpotOutbound, IZLinkSpotRequestCall, IZLinkSpotRelocationReadyCall
    {
        public string MeshName => "bingo.play";
        public string SpotId => "observe:room:observer";
        public ulong ObjectGeneration => 1;
        public RoutingId NodeRid => RoutingId.From("play");
        public IZLinkSpotOutbound Outbound => this;
        public IZLinkSpotHandlerRegistry Handlers => throw new NotSupportedException();

        public IZLinkSpotRelocationReadyCall RelocationReady() => this;

        internal int ReadinessSignals { get; private set; }

        public void Defer() => ReadinessSignals++;

        public ValueTask LeaveActorAsync(
            IZLinkActor actor,
            CancellationToken cancellationToken = default
        ) => throw new NotSupportedException();

        public ValueTask<bool> CloseAsync(CancellationToken cancellationToken = default) =>
            throw new NotSupportedException();

        internal ObservationTimer Timer { get; } = new();
        internal int TimerCreations { get; private set; }

        public ValueTask<IZLinkTimer> AddTimer<T>(
            string name,
            TimeSpan period,
            ZLinkTimerOptions? options = null,
            CancellationToken cancellationToken = default
        )
            where T : class
        {
            Assert.Equal("bingo-draw", name);
            TimerCreations++;
            return ValueTask.FromResult<IZLinkTimer>(Timer);
        }

        public IZLinkWorkerCall<T> RunCpuWorker<T>(Func<CancellationToken, T> work) =>
            throw new NotSupportedException();

        public IZLinkWorkerCall<T> RunIoWorker<T>(Func<CancellationToken, ValueTask<T>> work) =>
            throw new NotSupportedException();

        public IZLinkSpotRequestCall RequestToSpot<T>(string spotId, T request)
        {
            Assert.Equal("room", spotId);
            Assert.IsType<GetBingoObservationReq>(request);
            return this;
        }

        public IZLinkSpotSendCall SendToSpot<T>(string spotId, T message) =>
            throw new NotSupportedException();

        public IZLinkPublishCall Publish<T>(string channel, string topic, T message) =>
            throw new NotSupportedException();

        public IZLinkSendCall SendToChannel<T>(string channel, T message) =>
            throw new NotSupportedException();

        public IZLinkRequestCall RequestToChannel<T>(string channel, T request) =>
            throw new NotSupportedException();

        public IZLinkSpotRequestCall InstanceSpot() => this;

        public IZLinkSpotRequestCall InstanceSpot(string type) => this;

        public IZLinkSpotRequestCall InMesh(string meshName) =>
            throw new InvalidOperationException("InMesh requires Instance Spot intent.");

        public IZLinkSpotRequestCall Timeout(TimeSpan timeout) => this;

        public IZLinkSpotRequestCall Metadata(string key, string value) => this;

        public IZLinkSpotRequestCall Metadata(ZLinkMessageMetadata metadata) => this;

        public ZLinkRequest<T> Async<T>(CancellationToken cancellationToken = default) =>
            new(
                ValueTask.CompletedTask,
                failure is null
                    ? ValueTask.FromResult((T)(object)response)
                    : ValueTask.FromException<T>(failure)
            );

        public ValueTask<T> Yield<T>(CancellationToken cancellationToken = default) =>
            throw new InvalidOperationException("Snapshot must retain the observer turn.");
    }
}
