using System.Reflection;
using Microsoft.Extensions.Logging.Abstractions;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Contracts.Calls;
using Xunit;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Contracts.Streams;
using Zlink.Framework.Contracts.Timers;

namespace Zlink.NetworkSync.Interop;

internal sealed record NetworkProfile(string Name, ulong UpUs, ulong DownUs, ulong JitterUs, ulong SpotQueueUs)
{
    public static readonly NetworkProfile[] All =
    [
        new("symmetric", 20_000, 20_000, 0, 0),
        new("jitter", 30_000, 30_000, 8_000, 0),
        new("asymmetric-backlog", 78_000, 12_000, 4_000, 34_000),
    ];
}

internal sealed class VirtualMatch(int rate, NetworkProfile profile)
{
    private readonly PriorityQueue<Func<ValueTask>, (ulong Due, long Order)> _events = new();
    private readonly Dictionary<string, Func<TestPlayer, ZlinkStreamEncodedPayload, ValueTask>> _handlers = new();
    private long _order;
    private IZLinkSpotTimerHandler<TestMatch> _timer = null!;

    public TestClock Clock { get; } = new();
    public ulong Now => Clock.Now;
    public ulong Origin { get; private set; }
    public ulong TickDeadline(ulong tick) => Origin + (tick * 1_000_000 + (ulong)rate - 1) / (ulong)rate;
    public int PeakEvents { get; private set; }
    public long PayloadBytes { get; private set; }

    public async Task StartAsync()
    {
        var handlers = TestProxy.Create<IZLinkSpotHandlerRegistry>((method, args) =>
        {
            if (method.Name != nameof(IZLinkSpotHandlerRegistry.AddActorPacket))
                throw new NotSupportedException(method.Name);
            var handlerType = method.GetGenericArguments()[0];
            var contract = handlerType.GetInterfaces().Single();
            var register = typeof(VirtualMatch).GetMethod(nameof(Register), BindingFlags.Instance | BindingFlags.NonPublic)!;
            register.MakeGenericMethod(contract.GetGenericArguments()[2])
                .Invoke(this, [args[0], Activator.CreateInstance(handlerType, nonPublic: true)]);
            return null;
        });
        var context = TestProxy.Create<IZLinkSpotContext>((method, _) => method.Name switch
        {
            "get_SpotId" => MatchSettings.MatchId,
            "get_Handlers" => handlers,
            nameof(IZLinkSpotContext.AddTimer) => RegisterTimer(method),
            _ => throw new NotSupportedException(method.Name),
        });

        Spot = new TestMatch(context, new MatchSettings(rate, Clock), NullLogger<TestMatch>.Instance);
        ((IZLinkSpot)Spot).Configure();
        await ((IZLinkSpot)Spot).OnInitializeAsync(default);
    }

    private TestMatch Spot { get; set; } = null!;

    private void Register<T>(string name, IZLinkSpotActorSendHandler<TestMatch, TestPlayer, T> handler) =>
        _handlers.Add(name, (actor, payload) => handler.HandleAsync(Spot, actor, null!, payload.FromJson<T>(), default));

    private ValueTask<IZLinkTimer> RegisterTimer(MethodInfo method)
    {
        _timer = (IZLinkSpotTimerHandler<TestMatch>)Activator.CreateInstance(method.GetGenericArguments()[0], nonPublic: true)!;
        return ValueTask.FromResult<IZLinkTimer>(new VirtualTimer());
    }

    public async Task<Peer> JoinAsync(string actorId)
    {
        var peer = new Peer(this, profile);
        var bound = TestProxy.Create<IZLinkBoundSession>((method, args) =>
        {
            if (method.Name != nameof(IZLinkBoundSession.Send))
                throw new NotSupportedException(method.Name);
            var push = typeof(Peer).GetMethod(nameof(Peer.Push))!.MakeGenericMethod(method.GetGenericArguments()[0]);
            return push.Invoke(peer, args);
        });
        var context = TestProxy.Create<IZLinkActorContext>((method, _) => method.Name switch
        {
            "get_ActorId" => actorId,
            "get_BoundSession" => bound,
            _ => throw new NotSupportedException(method.Name),
        });
        peer.Actor = new TestPlayer(context);
        await ((IZLinkSpotActorMembershipLifecycle<TestPlayer>)Spot).OnJoinedActorAsync(peer.Actor, default);
        return peer;
    }

    public void Schedule(ulong time, Func<ValueTask> action)
    {
        Assert.True(time >= Now);
        _events.Enqueue(action, (time, _order++));
        PeakEvents = Math.Max(PeakEvents, _events.Count);
    }

    public async ValueTask AdvanceAsync(ulong time)
    {
        Assert.True(time >= Now);
        while (_events.TryPeek(out _, out var next) && next.Due <= time)
        {
            Clock.Set(next.Due);
            await _events.Dequeue()();
        }
        Clock.Set(time);
    }

    public async ValueTask TickAsync(ulong at, bool resync = false)
    {
        await AdvanceAsync(at);
        await _timer.HandleAsync(Spot, default, default);
        if (resync)
            Origin = at;
    }

    private sealed class VirtualTimer : IZLinkTimer
    {
        public bool IsDisposed { get; private set; }
        public ValueTask CancelAsync() => DisposeAsync();
        public ValueTask DisposeAsync()
        {
            IsDisposed = true;
            return ValueTask.CompletedTask;
        }
    }

    internal sealed class Peer(VirtualMatch match, NetworkProfile profile)
    {
        private readonly Dictionary<string, Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, CancellationToken, ValueTask>> _callbacks = new();
        private ulong _upArrival;
        private ulong _spotArrival;
        private ulong _downArrival;
        private uint _upRandom = 0x87c3_5921;
        private uint _downRandom = 0x7a5b_392b;

        public TestPlayer Actor { get; set; } = null!;
        public ulong PauseUntil { get; set; }
        public int SnapshotsSubmitted { get; private set; }
        public ulong ObservedJitterUs { get; private set; }
        public ulong MaxSpotWaitUs { get; private set; }
        public NetworkSyncInputV1<int, int>? LastInput { get; private set; }
        public List<NetworkSyncSnapshotV1<int>> Snapshots { get; } = [];
        public List<NetworkSyncPongV1> Pongs { get; } = [];
        public Dictionary<ulong, InputFeedback> Feedback { get; } = new();

        public IZlinkStreamConnector Connector()
        {
            var options = new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri("tcp://127.0.0.1:1"),
                DispatchMode = ZlinkStreamDispatchMode.Manual,
            };
            return TestProxy.Create<IZlinkStreamConnector>((method, args) => method.Name switch
            {
                "get_Options" => options,
                nameof(IZlinkStreamConnector.On) => On((string)args[0]!,
                    (Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, CancellationToken, ValueTask>)args[1]!),
                nameof(IZlinkStreamConnector.Send) => new ClientSend(this, (ZlinkStreamEncodedPayload)args[0]!),
                _ => throw new NotSupportedException(method.Name),
            });
        }

        private IDisposable On(string name, Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, CancellationToken, ValueTask> handler)
        {
            _callbacks.Add(name, handler);
            return new Registration(() => _callbacks.Remove(name));
        }

        public void Send(string name, ZlinkStreamEncodedPayload payload)
        {
            match.PayloadBytes += payload.Payload.Length;
            if (name == NetworkSyncPackets.Input)
                LastInput = payload.FromJson<NetworkSyncInputV1<int, int>>();

            var up = Delay(profile.UpUs, ref _upRandom);
            var ingress = _upArrival = Math.Max(_upArrival, match.Now + up);
            // All packets use the same ordered Spot mailbox. Queue wait precedes plugin receipt.
            var dispatch = _spotArrival = Math.Max(_spotArrival, ingress + profile.SpotQueueUs);
            MaxSpotWaitUs = Math.Max(MaxSpotWaitUs, dispatch - ingress);
            match.Schedule(dispatch, () => match._handlers[name](Actor, payload));
        }

        public IZLinkBoundSessionSendCall Push<T>(T message)
        {
            var name = typeof(T).GetCustomAttribute<ZLinkPacketAttribute>()?.PacketName ?? typeof(T).Name;
            return new ServerSend(() =>
            {
                if (name == NetworkSyncPackets.Snapshot)
                    SnapshotsSubmitted++;
                var payload = message.ToJson();
                match.PayloadBytes += payload.Payload.Length;
                var down = Delay(profile.DownUs, ref _downRandom);
                var arrival = _downArrival = Math.Max(Math.Max(_downArrival, match.Now + down), PauseUntil);
                match.Schedule(arrival, () => Deliver(name, payload));
                if (PauseUntil <= match.Now)
                    return ValueTask.CompletedTask;

                var admission = new TaskCompletionSource();
                match.Schedule(PauseUntil, () =>
                {
                    admission.SetResult();
                    return ValueTask.CompletedTask;
                });
                return new ValueTask(admission.Task);
            });
        }

        private ValueTask Deliver(string name, ZlinkStreamEncodedPayload payload)
        {
            if (name == NetworkSyncPackets.Snapshot)
            {
                var snapshot = payload.FromJson<NetworkSyncSnapshotV1<int>>();
                Snapshots.Add(snapshot);
                if (snapshot.InputFeedback is { } feedback)
                    Feedback[feedback.T1Us] = feedback;
            }
            else if (name == NetworkSyncPackets.Pong)
            {
                Pongs.Add(payload.FromJson<NetworkSyncPongV1>());
            }

            return _callbacks.TryGetValue(name, out var callback)
                ? callback(new ZlinkStreamMessage<ZlinkStreamEncodedPayload>(name, default!, payload), default)
                : ValueTask.CompletedTask;
        }

        private ulong Delay(ulong baseline, ref uint state)
        {
            state = unchecked(state * 1664525 + 1013904223);
            var offset = (long)(state % (2 * profile.JitterUs + 1)) - (long)profile.JitterUs;
            ObservedJitterUs = Math.Max(ObservedJitterUs, (ulong)Math.Abs(offset));
            return (ulong)((long)baseline + offset);
        }

        private sealed class ClientSend(Peer peer, ZlinkStreamEncodedPayload payload) : IZlinkStreamSendCall
        {
            private string _name = null!;
            public IZlinkStreamSendCall PacketName(string name) { _name = name; return this; }
            public ValueTask Async(CancellationToken cancellationToken = default)
            {
                cancellationToken.ThrowIfCancellationRequested();
                peer.Send(_name, payload);
                return ValueTask.CompletedTask;
            }

            public IZlinkStreamSendCall Metadata(string key, string value) => throw new NotSupportedException();
            public IZlinkStreamSendCall Metadata(ZlinkStreamMetadata metadata) => throw new NotSupportedException();
            public IZlinkStreamSendCall Compress() => throw new NotSupportedException();
        }

        private sealed class ServerSend(Func<ValueTask> send) : IZLinkBoundSessionSendCall
        {
            public ValueTask Async() => send();
            public IZLinkBoundSessionSendCall Metadata(string key, string value) => throw new NotSupportedException();
            public IZLinkBoundSessionSendCall Metadata(ZLinkMessageMetadata metadata) => throw new NotSupportedException();
        }

        private sealed class Registration(Action remove) : IDisposable
        {
            public void Dispose() => remove();
        }
    }
}

public class TestProxy : DispatchProxy
{
    private Func<MethodInfo, object?[], object?> _invoke = null!;

    public static T Create<T>(Func<MethodInfo, object?[], object?> invoke) where T : class
    {
        var proxy = Create<T, TestProxy>();
        ((TestProxy)(object)proxy)._invoke = invoke;
        return proxy;
    }

    protected override object? Invoke(MethodInfo? targetMethod, object?[]? args) =>
        _invoke(targetMethod!, args ?? []);
}
