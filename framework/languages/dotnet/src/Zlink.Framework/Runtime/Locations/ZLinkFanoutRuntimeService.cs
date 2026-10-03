using System.Runtime.CompilerServices;
using Zlink.Framework.Runtime.Diagnostics;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Identifiers;
using static Zlink.Framework.Runtime.Execution.ZLinkStateLaneWait;

namespace Zlink.Framework.Runtime.Locations;

internal sealed class ZLinkFanoutRuntimeService : IZLinkFanoutRuntime, IDisposable
{
    private readonly ZLinkStateLane _lane = new();
    private readonly HashSet<ZLinkChannelName> _automaticChannels;
    private readonly Dictionary<ZLinkChannelName, ChannelState> _states = [];
    private readonly Dictionary<
        ZLinkChannelName,
        List<ZLinkObservationQueue<ZLinkFanoutStatus>>
    > _observers = [];
    private readonly ZLinkFrameworkHostLifecycleState _hostLifecycle;

    internal ZLinkFanoutRuntimeService(
        ZLinkFrameworkRegistration registration,
        ZLinkFrameworkHostLifecycleState hostLifecycle
    )
    {
        _hostLifecycle = hostLifecycle;
        _automaticChannels = registration
            .Channels.Values.Where(static channel =>
                channel.AutoConnectType == ZLinkLocationAutoConnectType.Fanout
                && channel.Subscriber?.AutomaticDiscoveryEnabled == true
            )
            .Select(static channel =>
                ZLinkChannelName.FromBoundary(channel.ChannelName, nameof(channel.ChannelName))
            )
            .ToHashSet();
        foreach (var channelName in _automaticChannels)
            _states[channelName] = ChannelState.Empty(channelName.Value);
        _hostLifecycle.Changed += OnHostStateChanged;
    }

    internal ZLinkFanoutRuntimeService(ZLinkFrameworkRegistration registration)
        : this(registration, new ZLinkFrameworkHostLifecycleState()) { }

    private ZLinkFanoutChannelSnapshot SnapshotInternal(string channelName)
    {
        var channel = Channel(channelName);
        return AwaitStateLane(_lane.RunAsync(() => RequireState(channel).Snapshot));
    }

    public ZLinkFanoutStatus GetStatus(string channelName)
    {
        var snapshot = SnapshotInternal(channelName);
        return Project(snapshot, _hostLifecycle.State);
    }

    private static ZLinkFanoutStatus Project(
        ZLinkFanoutChannelSnapshot snapshot,
        ZLinkFrameworkRuntimeState hostState
    )
    {
        var publishers = snapshot
            .Publishers.Select(static publisher =>
            {
                var (state, reason) = MapPeer(publisher.State);
                return new ZLinkPeerStatus(publisher.PublisherRid, state, reason);
            })
            .ToArray();
        var isReady =
            hostState == ZLinkFrameworkRuntimeState.Serving && snapshot.ReadyConnectionCount > 0;
        return new ZLinkFanoutStatus(
            snapshot.ChannelName,
            isReady ? ZLinkTopologyState.Ready : HostTopologyState(hostState),
            isReady,
            snapshot.ReadyConnectionCount,
            publishers,
            snapshot.Sequence,
            snapshot.ObservedAt
        );
    }

    public async IAsyncEnumerable<ZLinkObservedStatus<ZLinkFanoutStatus>> ObserveAsync(
        string channelName,
        [EnumeratorCancellation] CancellationToken cancellationToken = default
    )
    {
        var channel = Channel(channelName);
        var observer = await _lane
            .RunAsync(() =>
            {
                var initial = Project(RequireState(channel).Snapshot, _hostLifecycle.State);
                var subscription = new ZLinkObservationQueue<ZLinkFanoutStatus>(
                    initial,
                    initial.State.IsTerminal(),
                    static status => status.ChannelName,
                    eventName: "fanout"
                );
                if (!_observers.TryGetValue(channel, out var observers))
                    _observers[channel] = observers = [];
                observers.Add(subscription);
                return subscription;
            })
            .ConfigureAwait(false);

        try
        {
            await foreach (
                var item in observer.ReadAllAsync(cancellationToken).ConfigureAwait(false)
            )
                yield return item;
        }
        finally
        {
            await _lane
                .RunAsync(() =>
                {
                    if (_observers.TryGetValue(channel, out var observers))
                    {
                        observers.Remove(observer);
                        if (observers.Count == 0)
                            _observers.Remove(channel);
                    }
                })
                .ConfigureAwait(false);
            observer.Complete();
        }
    }

    internal void RecordSnapshot(
        string channelName,
        IReadOnlyList<ZLinkFanoutPublisherConnectionSnapshot> publishers,
        ZLinkLocationRuntimeSnapshot location
    )
    {
        var channel = Channel(channelName);
        AwaitStateLane(_lane.RunAsync(() => RecordSnapshotOnLane(channel, publishers, location)));
    }

    private void RecordSnapshotOnLane(
        ZLinkChannelName channel,
        IReadOnlyList<ZLinkFanoutPublisherConnectionSnapshot> publishers,
        ZLinkLocationRuntimeSnapshot location
    )
    {
        var previous = RequireState(channel);
        var now = DateTimeOffset.UtcNow;
        var ordered = publishers
            .OrderBy(static entry => entry.PublisherRid, ZLinkRoutingIdOrder.Instance)
            .ThenBy(static entry => entry.LifecycleGeneration)
            .ToArray();
        var changed =
            !previous.Snapshot.Publishers.SequenceEqual(ordered)
            || previous.Snapshot.Location != location;
        if (!changed)
            return;
        var nextSequence = checked(previous.Snapshot.Sequence + 1);
        var next = new ZLinkFanoutChannelSnapshot(
            channel.Value,
            ordered.Count(static entry => entry.ConnectionIntent),
            ordered.Count(static entry => entry.Ready),
            nextSequence,
            now,
            ordered,
            location
        );
        _states[channel] = new ChannelState(next);
        Emit(channel, Project(next, _hostLifecycle.State));
    }

    internal void RecordLocationFailure(
        string channelName,
        DateTimeOffset? lastSuccessAt,
        DateTimeOffset failureAt
    )
    {
        var channel = Channel(channelName);
        AwaitStateLane(
            _lane.RunAsync(() =>
            {
                var current = RequireState(channel);
                RecordSnapshotOnLane(
                    channel,
                    current.Snapshot.Publishers,
                    ZLinkLocationStoreHealth.ProjectSnapshot(
                        new ZLinkLocationStoreHealth.Snapshot(false, lastSuccessAt, failureAt, null)
                    )
                );
            })
        );
    }

    private ChannelState RequireState(ZLinkChannelName channelName)
    {
        if (
            !_automaticChannels.Contains(channelName)
            || !_states.TryGetValue(channelName, out var state)
        )
            throw new ZLinkConfigurationException(
                $"Fanout channel '{channelName.Value}' is not an automatic subscriber."
            );
        return state;
    }

    private void Emit(ZLinkChannelName channelName, ZLinkFanoutStatus status)
    {
        if (!_observers.TryGetValue(channelName, out var observers))
            return;
        foreach (var observer in observers)
            observer.Publish(status, status.State.IsTerminal());
    }

    private void OnHostStateChanged(ZLinkFrameworkRuntimeState hostState)
    {
        AwaitStateLane(
            _lane.RunAsync(() =>
            {
                var now = DateTimeOffset.UtcNow;
                foreach (var (channelName, state) in _states.ToArray())
                {
                    var sequence = checked(state.Snapshot.Sequence + 1);
                    var next = state.Snapshot with { Sequence = sequence, ObservedAt = now };
                    _states[channelName] = new ChannelState(next);
                    Emit(channelName, Project(next, hostState));
                }
            })
        );
    }

    private static ZLinkTopologyState HostTopologyState(ZLinkFrameworkRuntimeState state) =>
        state switch
        {
            ZLinkFrameworkRuntimeState.Preparing => ZLinkTopologyState.Starting,
            ZLinkFrameworkRuntimeState.Relocating
            or ZLinkFrameworkRuntimeState.Relocated
            or ZLinkFrameworkRuntimeState.Draining => ZLinkTopologyState.Stopping,
            ZLinkFrameworkRuntimeState.Stopped => ZLinkTopologyState.Stopped,
            ZLinkFrameworkRuntimeState.Error => ZLinkTopologyState.Failed,
            _ => ZLinkTopologyState.Degraded,
        };

    public void Dispose()
    {
        _hostLifecycle.Changed -= OnHostStateChanged;
        AwaitStateLane(_lane.RunAsync(_observers.Clear));
    }

    private static ZLinkChannelName Channel(string channelName) =>
        ZLinkChannelName.FromBoundary(channelName, nameof(channelName));

    private static (ZLinkPeerState State, ZLinkTopologyReason? Reason) MapPeer(
        ZLinkFanoutPublisherConnectionState state
    ) =>
        state switch
        {
            ZLinkFanoutPublisherConnectionState.ExcludedDraining => (
                ZLinkPeerState.Draining,
                ZLinkTopologyReason.Draining
            ),
            ZLinkFanoutPublisherConnectionState.Ready => (ZLinkPeerState.Ready, null),
            ZLinkFanoutPublisherConnectionState.Connecting
            or ZLinkFanoutPublisherConnectionState.Reconnecting => (
                ZLinkPeerState.Connecting,
                ZLinkTopologyReason.NoReadyPeer
            ),
            _ => (ZLinkPeerState.NotConnected, ZLinkTopologyReason.NoReadyPeer),
        };

    private sealed record ChannelState(ZLinkFanoutChannelSnapshot Snapshot)
    {
        internal static ChannelState Empty(string channelName)
        {
            var now = DateTimeOffset.UtcNow;
            return new ChannelState(
                new ZLinkFanoutChannelSnapshot(
                    channelName,
                    0,
                    0,
                    0,
                    now,
                    Array.Empty<ZLinkFanoutPublisherConnectionSnapshot>(),
                    new ZLinkLocationRuntimeSnapshot(
                        ZLinkLocationRuntimeSnapshot.UnknownState,
                        null,
                        null
                    )
                )
            );
        }
    }
}
